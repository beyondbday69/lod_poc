#include "mod/GlTrace.h"
#include "mod/LodPocMod.h"

#include <dlfcn.h>
#include <unistd.h>
#include <algorithm>
#include <sstream>

namespace lod_poc {

namespace {

const char *modeToString(GLenum mode) {
    switch (mode) {
        case GL_POINTS:         return "POINTS";
        case GL_LINES:          return "LINES";
        case GL_LINE_LOOP:      return "LINE_LOOP";
        case GL_LINE_STRIP:     return "LINE_STRIP";
        case GL_TRIANGLES:      return "TRIANGLES";
        case GL_TRIANGLE_STRIP: return "TRIANGLE_STRIP";
        case GL_TRIANGLE_FAN:   return "TRIANGLE_FAN";
        default:                return "UNKNOWN";
    }
}

const char *typeToString(GLenum type) {
    switch (type) {
        case GL_UNSIGNED_BYTE:  return "UNSIGNED_BYTE";
        case GL_UNSIGNED_SHORT: return "UNSIGNED_SHORT";
        case GL_UNSIGNED_INT:   return "UNSIGNED_INT";
        default:                return "UNKNOWN";
    }
}

const char *internalFormatToString(GLenum fmt) {
    switch (fmt) {
        case 0x8058: return "RGBA8";
        case 0x8051: return "RGB8";
        case 0x8056: return "RGBA4";
        case 0x8057: return "RGB5_A1";
        case 0x8D62: return "RGB565";
        case 0x81A5: return "DEPTH24";
        case 0x81A6: return "DEPTH32";
        case 0x81A7: return "DEPTH24_STENCIL8";
        case 0x88F0: return "DEPTH32F_STENCIL8";
        case 0x8D70: return "RGBA8UI";
        case 0x881A: return "RGBA16F";
        case 0x8814: return "RGBA32F";
        default:     return "UNKNOWN";
    }
}

} // namespace

GlTrace &GlTrace::instance() {
    static GlTrace sInstance;
    return sInstance;
}

GlTrace::GlTrace() = default;

void GlTrace::setConfig(bool traceFrame, bool traceRaw, const std::string &traceLabel) {
    std::lock_guard lock(mStateMutex);
    mConfigTraceFrame = traceFrame;
    mConfigTraceRaw = traceRaw;
    mConfigTraceLabel = traceLabel;
    if (traceFrame && !traceLabel.empty() && traceLabel != mLastTracedLabel) {
        armTrace(traceLabel);
    }
}

void GlTrace::armTrace(const std::string &label) {
    std::lock_guard lock(mStateMutex);
    mArmedLabel = label;
    if (!mHeavyHooksInstalled) {
        installHeavyHooks();
    }
    mState.store(TraceState::Armed, std::memory_order_release);
    LodPocMod::logAll(LogLevel::Info, "[GLTrace] Armed for single-frame trace: '{}'", label);
}

void GlTrace::onSwapBuffers(uint64_t frameCount, double currentFps) {
    const pid_t tid = gettid();
    setRenderThreadId(tid);

    const uint64_t frameDraws = mFrameDrawElements.exchange(0, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();

    TraceState currentState = mState.load(std::memory_order_acquire);

    // If currently recording, this swap buffers marks the END of the traced frame!
    if (currentState == TraceState::Recording) {
        mState.store(TraceState::Idle, std::memory_order_release);
        std::string label = mActiveLabel;
        mLastTracedLabel = label;

        dumpPassSummary(label);

        // Remove heavy hooks after tracing to recover game FPS!
        removeHeavyHooks();
        LodPocMod::logAll(LogLevel::Info,
                          "[GLTrace] Traced frame finished. Heavy hooks removed. (FPS before trace: {:.1f}, current: {:.1f})",
                          mLastRecordedFps, currentFps);
        return;
    }

    // If armed, start recording at this frame boundary!
    if (currentState == TraceState::Armed) {
        std::lock_guard lock(mStateMutex);
        mActiveLabel = mArmedLabel;
        mLastRecordedFps = currentFps;

        {
            std::lock_guard segLock(mFrameSegmentsMutex);
            mCurrentFrameSegments.clear();
            startNewSegment(mCurrentBoundDrawFbo);
        }

        mState.store(TraceState::Recording, std::memory_order_release);
        LodPocMod::logAll(LogLevel::Info,
                          "[GLTrace] Began recording frame trace: '{}' at frame #{} (tid={})",
                          mActiveLabel, frameCount, tid);
        return;
    }

    // Check automatic triggers when idle
    if (currentState == TraceState::Idle && mConfigTraceFrame) {
        // Trigger 1: Main menu trace (frame > 50 and at least 30 draws)
        if (!mMainMenuTraced && frameCount > 50 && frameDraws >= 30) {
            mMainMenuTraced = true;
            armTrace("main_menu");
            return;
        }

        // Trigger 2: In-world trace (~10s after first frame with glDrawElements > 500)
        if (frameDraws > 500 && mFirstHeavyFrameTime == std::chrono::steady_clock::time_point{}) {
            mFirstHeavyFrameTime = now;
            LodPocMod::logAll(LogLevel::Info,
                              "[GLTrace] In-world heavy draw detected (#{} draws in frame {}). Arming in-world trace in ~10s.",
                              frameDraws, frameCount);
        }

        if (!mInWorldTraced && mFirstHeavyFrameTime != std::chrono::steady_clock::time_point{}) {
            if (now >= mFirstHeavyFrameTime + std::chrono::seconds(10)) {
                mInWorldTraced = true;
                std::string label = mConfigTraceLabel.empty() ? "in_world" : mConfigTraceLabel;
                armTrace(label);
                return;
            }
        }
    }
}

std::string GlTrace::buildAttachmentDesc(GLuint fboId) {
    if (fboId == 0) {
        return "Default Screen (FBO 0)";
    }
    auto it = mFboMap.find(fboId);
    if (it == mFboMap.end()) {
        return "Unknown FBO";
    }
    const auto &desc = it->second;
    std::string s;
    for (const auto &pair : desc.colors) {
        const auto &img = pair.second;
        s += fmt::format("Color[0x{:x}]: id={} {}x{} fmt=0x{:x} ({}); ",
                         pair.first, img.id, img.width, img.height,
                         img.internalFormat, internalFormatToString(img.internalFormat));
    }
    if (desc.depth.id != 0) {
        s += fmt::format("Depth: id={} {}x{} fmt=0x{:x} ({}); ",
                         desc.depth.id, desc.depth.width, desc.depth.height,
                         desc.depth.internalFormat, internalFormatToString(desc.depth.internalFormat));
    }
    if (desc.stencil.id != 0) {
        s += fmt::format("Stencil: id={} {}x{} fmt=0x{:x}; ",
                         desc.stencil.id, desc.stencil.width, desc.stencil.height,
                         desc.stencil.internalFormat);
    }
    return s.empty() ? "None" : s;
}

void GlTrace::startNewSegment(GLuint fboId) {
    PassSegment seg;
    seg.segmentIndex = static_cast<uint32_t>(mCurrentFrameSegments.size() + 1);
    seg.fboId = fboId;
    seg.vpX = mCurrentVpX;
    seg.vpY = mCurrentVpY;
    seg.vpW = mCurrentVpW;
    seg.vpH = mCurrentVpH;
    seg.attachmentSummary = buildAttachmentDesc(fboId);
    seg.depthTestEnabled = mCurrentDepthTest;
    seg.depthMaskEnabled = mCurrentDepthMask;
    seg.blendEnabled = mCurrentBlend;
    if (mCurrentProgram != 0) {
        seg.distinctPrograms.insert(mCurrentProgram);
    }
    mCurrentFrameSegments.push_back(seg);
}

void GlTrace::onDraw(const RawDrawRecord &record) {
    if (mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != getRenderThreadId()) return;

    std::lock_guard lock(mFrameSegmentsMutex);
    if (mCurrentFrameSegments.empty()) {
        startNewSegment(mCurrentBoundDrawFbo);
    }
    auto &seg = mCurrentFrameSegments.back();

    seg.totalDrawCalls++;
    uint64_t vCount = static_cast<uint64_t>(record.count);
    if (record.primCount > 0) {
        vCount = static_cast<uint64_t>(record.count) * record.primCount;
    }
    seg.sumVertexCount += vCount;
    seg.sumInstanceCount += static_cast<uint64_t>(record.instanceCount);
    seg.drawsByFunction[record.functionName]++;

    if (mCurrentDepthTest) seg.depthTestEnabled = true;
    if (mCurrentBlend) seg.blendEnabled = true;
    seg.depthMaskEnabled = mCurrentDepthMask;
    if (mCurrentProgram != 0) {
        seg.distinctPrograms.insert(mCurrentProgram);
    }

    if (mConfigTraceRaw) {
        seg.rawDraws.push_back(record);
    }
}

void GlTrace::dumpPassSummary(const std::string &label) {
    std::lock_guard lock(mFrameSegmentsMutex);

    LodPocMod::logAll(LogLevel::Info, "==================== PASS SUMMARY: {} ====================", label);
    LodPocMod::logAll(LogLevel::Info, "Total FBO Pass Segments: {}", mCurrentFrameSegments.size());

    struct Candidate {
        size_t segIndex;
        uint32_t fboId;
        uint64_t sumCount;
        size_t distinctProgs;
    };
    std::vector<Candidate> depthCandidates;

    for (size_t i = 0; i < mCurrentFrameSegments.size(); ++i) {
        const auto &seg = mCurrentFrameSegments[i];

        std::string funcBreakdown;
        for (const auto &pair : seg.drawsByFunction) {
            if (!funcBreakdown.empty()) funcBreakdown += ", ";
            funcBreakdown += fmt::format("{}: {}", pair.first, pair.second);
        }
        if (funcBreakdown.empty()) funcBreakdown = "None";

        std::string progsList;
        for (GLuint p : seg.distinctPrograms) {
            if (!progsList.empty()) progsList += ", ";
            progsList += std::to_string(p);
        }

        LodPocMod::logAll(LogLevel::Info,
                          "SEGMENT #{}: FBO {} | Viewport: [{}, {}, {}, {}] | Attachments: [{}]",
                          seg.segmentIndex, seg.fboId, seg.vpX, seg.vpY, seg.vpW, seg.vpH, seg.attachmentSummary);
        LodPocMod::logAll(LogLevel::Info,
                          "  Draws: {} ({}) | SUM(count): {} | Instanced: {}",
                          seg.totalDrawCalls, funcBreakdown, seg.sumVertexCount, seg.sumInstanceCount);
        LodPocMod::logAll(LogLevel::Info,
                          "  State: DepthTest={}, DepthMask={}, Blend={} | Distinct Programs: {} (IDs: [{}])",
                          seg.depthTestEnabled ? "ON" : "OFF",
                          seg.depthMaskEnabled ? "ON" : "OFF",
                          seg.blendEnabled ? "ON" : "OFF",
                          seg.distinctPrograms.size(), progsList);

        if (mConfigTraceRaw && !seg.rawDraws.empty()) {
            for (const auto &raw : seg.rawDraws) {
                LodPocMod::logAll(LogLevel::Info,
                                  "    [RawDraw #{}] {}(mode=0x{:x} [{}], first={}, count={}, type=0x{:x} [{}], indices=0x{:x}, inst={}, baseVert={})",
                                  raw.callIndex, raw.functionName,
                                  raw.mode, modeToString(raw.mode),
                                  raw.first, raw.count,
                                  raw.type, typeToString(raw.type),
                                  raw.indicesOrOffset, raw.instanceCount, raw.baseVertex);
            }
        }

        if (seg.depthTestEnabled && seg.sumVertexCount > 0) {
            depthCandidates.push_back({seg.segmentIndex, seg.fboId, seg.sumVertexCount, seg.distinctPrograms.size()});
        }
    }

    std::sort(depthCandidates.begin(), depthCandidates.end(), [](const Candidate &a, const Candidate &b) {
        return a.sumCount > b.sumCount;
    });

    if (!depthCandidates.empty()) {
        const auto &top = depthCandidates.front();
        LodPocMod::logAll(LogLevel::Info,
                          ">>> TERRAIN CANDIDATE: Segment #{} (FBO {}) with SUM(count)={}, Programs={} <<<",
                          top.segIndex, top.fboId, top.sumCount, top.distinctProgs);

        LodPocMod::logAll(LogLevel::Info, "TOP 3 CANDIDATE SEGMENTS (DepthTest ON, highest vertex/index count):");
        for (size_t k = 0; k < std::min<size_t>(3, depthCandidates.size()); ++k) {
            const auto &c = depthCandidates[k];
            LodPocMod::logAll(LogLevel::Info,
                              "  {}. Segment #{}: FBO {} | SUM(count)={} | Programs={}",
                              k + 1, c.segIndex, c.fboId, c.sumCount, c.distinctProgs);
        }
    } else {
        LodPocMod::logAll(LogLevel::Info, "No segments found with DepthTest ON and draw calls > 0.");
    }

    LodPocMod::logAll(LogLevel::Info, "============================================================");
}

// -----------------------------------------------------------------------------
// State and Storage Handlers
// -----------------------------------------------------------------------------

void GlTrace::onBindFramebuffer(GLenum target, GLuint framebuffer) {
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
        mCurrentBoundDrawFbo = framebuffer;
    }
    if (mState.load(std::memory_order_relaxed) == TraceState::Recording && gettid() == getRenderThreadId()) {
        std::lock_guard lock(mFrameSegmentsMutex);
        startNewSegment(framebuffer);
    }
}

void GlTrace::onBindTexture(GLenum target, GLuint texture) {
    if (target == GL_TEXTURE_2D) {
        mCurrentBoundTexture2D = texture;
    }
}

void GlTrace::onBindRenderbuffer(GLenum target, GLuint renderbuffer) {
    if (target == GL_RENDERBUFFER) {
        mCurrentBoundRenderbuffer = renderbuffer;
    }
}

void GlTrace::onTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height) {
    (void)level;
    if (target == GL_TEXTURE_2D && mCurrentBoundTexture2D != 0) {
        mTextureMap[mCurrentBoundTexture2D] = {mCurrentBoundTexture2D, width, height, static_cast<GLenum>(internalformat), true};
    }
}

void GlTrace::onTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
    (void)levels;
    if (target == GL_TEXTURE_2D && mCurrentBoundTexture2D != 0) {
        mTextureMap[mCurrentBoundTexture2D] = {mCurrentBoundTexture2D, width, height, internalformat, true};
    }
}

void GlTrace::onRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    if (target == GL_RENDERBUFFER && mCurrentBoundRenderbuffer != 0) {
        mRenderbufferMap[mCurrentBoundRenderbuffer] = {mCurrentBoundRenderbuffer, width, height, internalformat, false};
    }
}

void GlTrace::onFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    (void)target; (void)textarget; (void)level;
    GLuint fbo = mCurrentBoundDrawFbo;
    if (fbo == 0) return;

    ImageDesc desc = mTextureMap[texture];
    desc.id = texture;
    desc.isTexture = true;

    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT15) {
        mFboMap[fbo].colors[attachment] = desc;
    } else if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        mFboMap[fbo].depth = desc;
    } else if (attachment == GL_STENCIL_ATTACHMENT) {
        mFboMap[fbo].stencil = desc;
    }
}

void GlTrace::onFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
    (void)target; (void)level; (void)layer;
    GLuint fbo = mCurrentBoundDrawFbo;
    if (fbo == 0) return;

    ImageDesc desc = mTextureMap[texture];
    desc.id = texture;
    desc.isTexture = true;

    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT15) {
        mFboMap[fbo].colors[attachment] = desc;
    } else if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        mFboMap[fbo].depth = desc;
    }
}

void GlTrace::onFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    (void)target; (void)renderbuffertarget;
    GLuint fbo = mCurrentBoundDrawFbo;
    if (fbo == 0) return;

    ImageDesc desc = mRenderbufferMap[renderbuffer];
    desc.id = renderbuffer;
    desc.isTexture = false;

    if (attachment >= GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT15) {
        mFboMap[fbo].colors[attachment] = desc;
    } else if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT) {
        mFboMap[fbo].depth = desc;
    } else if (attachment == GL_STENCIL_ATTACHMENT) {
        mFboMap[fbo].stencil = desc;
    }
}

void GlTrace::onDrawBuffers(GLsizei n, const GLenum *bufs) {
    (void)n; (void)bufs;
}

void GlTrace::onViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    mCurrentVpX = x;
    mCurrentVpY = y;
    mCurrentVpW = width;
    mCurrentVpH = height;
    if (mState.load(std::memory_order_relaxed) == TraceState::Recording && gettid() == getRenderThreadId()) {
        std::lock_guard lock(mFrameSegmentsMutex);
        if (!mCurrentFrameSegments.empty()) {
            mCurrentFrameSegments.back().vpX = x;
            mCurrentFrameSegments.back().vpY = y;
            mCurrentFrameSegments.back().vpW = width;
            mCurrentFrameSegments.back().vpH = height;
        }
    }
}

void GlTrace::onEnable(GLenum cap) {
    if (cap == 0x0B71 /* GL_DEPTH_TEST */) mCurrentDepthTest = true;
    else if (cap == 0x0BE2 /* GL_BLEND */) mCurrentBlend = true;
}

void GlTrace::onDisable(GLenum cap) {
    if (cap == 0x0B71 /* GL_DEPTH_TEST */) mCurrentDepthTest = false;
    else if (cap == 0x0BE2 /* GL_BLEND */) mCurrentBlend = false;
}

void GlTrace::onDepthMask(GLboolean flag) {
    mCurrentDepthMask = (flag != GL_FALSE);
}

void GlTrace::onUseProgram(GLuint program) {
    mCurrentProgram = program;
}

// -----------------------------------------------------------------------------
// Detour Implementations
// -----------------------------------------------------------------------------

void GlTrace::detourBindFramebuffer(GLenum target, GLuint framebuffer) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigBindFramebuffer != nullptr) {
        inst.mOrigBindFramebuffer(target, framebuffer);
    }
    LodPocMod::instance().onBindFramebuffer(target, framebuffer);
    inst.onBindFramebuffer(target, framebuffer);
}

void GlTrace::detourBindTexture(GLenum target, GLuint texture) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigBindTexture != nullptr) inst.mOrigBindTexture(target, texture);
    inst.onBindTexture(target, texture);
}

void GlTrace::detourBindRenderbuffer(GLenum target, GLuint renderbuffer) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigBindRenderbuffer != nullptr) inst.mOrigBindRenderbuffer(target, renderbuffer);
    inst.onBindRenderbuffer(target, renderbuffer);
}

void GlTrace::detourTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigTexImage2D != nullptr) inst.mOrigTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    inst.onTexImage2D(target, level, internalformat, width, height);
}

void GlTrace::detourTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigTexStorage2D != nullptr) inst.mOrigTexStorage2D(target, levels, internalformat, width, height);
    inst.onTexStorage2D(target, levels, internalformat, width, height);
}

void GlTrace::detourRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigRenderbufferStorage != nullptr) inst.mOrigRenderbufferStorage(target, internalformat, width, height);
    inst.onRenderbufferStorage(target, internalformat, width, height);
}

void GlTrace::detourFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigFramebufferTexture2D != nullptr) inst.mOrigFramebufferTexture2D(target, attachment, textarget, texture, level);
    inst.onFramebufferTexture2D(target, attachment, textarget, texture, level);
}

void GlTrace::detourFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigFramebufferTextureLayer != nullptr) inst.mOrigFramebufferTextureLayer(target, attachment, texture, level, layer);
    inst.onFramebufferTextureLayer(target, attachment, texture, level, layer);
}

void GlTrace::detourFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigFramebufferRenderbuffer != nullptr) inst.mOrigFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer);
    inst.onFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer);
}

void GlTrace::detourDrawBuffers(GLsizei n, const GLenum *bufs) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawBuffers != nullptr) inst.mOrigDrawBuffers(n, bufs);
    inst.onDrawBuffers(n, bufs);
}

void GlTrace::detourViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigViewport != nullptr) inst.mOrigViewport(x, y, width, height);
    inst.onViewport(x, y, width, height);
}

void GlTrace::detourEnable(GLenum cap) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigEnable != nullptr) inst.mOrigEnable(cap);
    inst.onEnable(cap);
}

void GlTrace::detourDisable(GLenum cap) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDisable != nullptr) inst.mOrigDisable(cap);
    inst.onDisable(cap);
}

void GlTrace::detourDepthMask(GLboolean flag) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDepthMask != nullptr) inst.mOrigDepthMask(flag);
    inst.onDepthMask(flag);
}

void GlTrace::detourUseProgram(GLuint program) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigUseProgram != nullptr) inst.mOrigUseProgram(program);
    inst.onUseProgram(program);
}

void GlTrace::detourDrawArrays(GLenum mode, GLint first, GLsizei count) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawArrays != nullptr) inst.mOrigDrawArrays(mode, first, count);
    RawDrawRecord rec;
    rec.functionName = "glDrawArrays";
    rec.mode = mode;
    rec.first = first;
    rec.count = count;
    rec.instanceCount = 1;
    inst.onDraw(rec);
}

void GlTrace::detourDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawArraysInstanced != nullptr) inst.mOrigDrawArraysInstanced(mode, first, count, instancecount);
    RawDrawRecord rec;
    rec.functionName = "glDrawArraysInstanced";
    rec.mode = mode;
    rec.first = first;
    rec.count = count;
    rec.instanceCount = instancecount;
    inst.onDraw(rec);
}

void GlTrace::detourDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawElements != nullptr) inst.mOrigDrawElements(mode, count, type, indices);
    inst.mFrameDrawElements.fetch_add(1, std::memory_order_relaxed);
    RawDrawRecord rec;
    rec.functionName = "glDrawElements";
    rec.mode = mode;
    rec.count = count;
    rec.type = type;
    rec.indicesOrOffset = reinterpret_cast<uintptr_t>(indices);
    rec.instanceCount = 1;
    inst.onDraw(rec);
}

void GlTrace::detourDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawElementsInstanced != nullptr) inst.mOrigDrawElementsInstanced(mode, count, type, indices, instancecount);
    inst.mFrameDrawElements.fetch_add(1, std::memory_order_relaxed);
    RawDrawRecord rec;
    rec.functionName = "glDrawElementsInstanced";
    rec.mode = mode;
    rec.count = count;
    rec.type = type;
    rec.indicesOrOffset = reinterpret_cast<uintptr_t>(indices);
    rec.instanceCount = instancecount;
    inst.onDraw(rec);
}

void GlTrace::detourDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawRangeElements != nullptr) inst.mOrigDrawRangeElements(mode, start, end, count, type, indices);
    inst.mFrameDrawElements.fetch_add(1, std::memory_order_relaxed);
    RawDrawRecord rec;
    rec.functionName = "glDrawRangeElements";
    rec.mode = mode;
    rec.start = start;
    rec.end = end;
    rec.count = count;
    rec.type = type;
    rec.indicesOrOffset = reinterpret_cast<uintptr_t>(indices);
    rec.instanceCount = 1;
    inst.onDraw(rec);
}

void GlTrace::detourDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawElementsBaseVertex != nullptr) inst.mOrigDrawElementsBaseVertex(mode, count, type, indices, basevertex);
    inst.mFrameDrawElements.fetch_add(1, std::memory_order_relaxed);
    RawDrawRecord rec;
    rec.functionName = "glDrawElementsBaseVertex";
    rec.mode = mode;
    rec.count = count;
    rec.type = type;
    rec.indicesOrOffset = reinterpret_cast<uintptr_t>(indices);
    rec.baseVertex = basevertex;
    rec.instanceCount = 1;
    inst.onDraw(rec);
}

void GlTrace::detourDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount, GLint basevertex) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawElementsInstancedBaseVertex != nullptr) inst.mOrigDrawElementsInstancedBaseVertex(mode, count, type, indices, instancecount, basevertex);
    inst.mFrameDrawElements.fetch_add(1, std::memory_order_relaxed);
    RawDrawRecord rec;
    rec.functionName = "glDrawElementsInstancedBaseVertex";
    rec.mode = mode;
    rec.count = count;
    rec.type = type;
    rec.indicesOrOffset = reinterpret_cast<uintptr_t>(indices);
    rec.instanceCount = instancecount;
    rec.baseVertex = basevertex;
    inst.onDraw(rec);
}

void GlTrace::detourDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint basevertex) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawRangeElementsBaseVertex != nullptr) inst.mOrigDrawRangeElementsBaseVertex(mode, start, end, count, type, indices, basevertex);
    inst.mFrameDrawElements.fetch_add(1, std::memory_order_relaxed);
    RawDrawRecord rec;
    rec.functionName = "glDrawRangeElementsBaseVertex";
    rec.mode = mode;
    rec.start = start;
    rec.end = end;
    rec.count = count;
    rec.type = type;
    rec.indicesOrOffset = reinterpret_cast<uintptr_t>(indices);
    rec.baseVertex = basevertex;
    rec.instanceCount = 1;
    inst.onDraw(rec);
}

void GlTrace::detourDrawArraysIndirect(GLenum mode, const void *indirect) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawArraysIndirect != nullptr) inst.mOrigDrawArraysIndirect(mode, indirect);
    RawDrawRecord rec;
    rec.functionName = "glDrawArraysIndirect";
    rec.mode = mode;
    rec.indicesOrOffset = reinterpret_cast<uintptr_t>(indirect);
    inst.onDraw(rec);
}

void GlTrace::detourDrawElementsIndirect(GLenum mode, GLenum type, const void *indirect) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawElementsIndirect != nullptr) inst.mOrigDrawElementsIndirect(mode, type, indirect);
    inst.mFrameDrawElements.fetch_add(1, std::memory_order_relaxed);
    RawDrawRecord rec;
    rec.functionName = "glDrawElementsIndirect";
    rec.mode = mode;
    rec.type = type;
    rec.indicesOrOffset = reinterpret_cast<uintptr_t>(indirect);
    inst.onDraw(rec);
}

void GlTrace::detourMultiDrawArraysEXT(GLenum mode, const GLint *first, const GLsizei *count, GLsizei primcount) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigMultiDrawArraysEXT != nullptr) inst.mOrigMultiDrawArraysEXT(mode, first, count, primcount);
    RawDrawRecord rec;
    rec.functionName = "glMultiDrawArraysEXT";
    rec.mode = mode;
    rec.primCount = primcount;
    rec.instanceCount = 1;
    if (count != nullptr) {
        GLsizei sum = 0;
        for (GLsizei i = 0; i < primcount; ++i) sum += count[i];
        rec.count = sum;
    }
    inst.onDraw(rec);
}

void GlTrace::detourMultiDrawElementsEXT(GLenum mode, const GLsizei *count, GLenum type, const void *const *indices, GLsizei primcount) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigMultiDrawElementsEXT != nullptr) inst.mOrigMultiDrawElementsEXT(mode, count, type, indices, primcount);
    inst.mFrameDrawElements.fetch_add(1, std::memory_order_relaxed);
    RawDrawRecord rec;
    rec.functionName = "glMultiDrawElementsEXT";
    rec.mode = mode;
    rec.type = type;
    rec.primCount = primcount;
    rec.instanceCount = 1;
    if (count != nullptr) {
        GLsizei sum = 0;
        for (GLsizei i = 0; i < primcount; ++i) sum += count[i];
        rec.count = sum;
    }
    inst.onDraw(rec);
}

// -----------------------------------------------------------------------------
// Lifecycle & Hook Installation
// -----------------------------------------------------------------------------

void GlTrace::install() {
    remove();

    void *glesLib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!glesLib) glesLib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_GLOBAL);

    void *target = glesLib ? dlsym(glesLib, "glBindFramebuffer") : nullptr;
    if (!target) target = dlsym(RTLD_DEFAULT, "glBindFramebuffer");

    if (target != nullptr) {
        pl::memory::HookHandle handle(target,
                                      reinterpret_cast<void *>(&detourBindFramebuffer),
                                      reinterpret_cast<void **>(&mOrigBindFramebuffer));
        if (handle.installed()) {
            mPermanentHooks.push_back(std::move(handle));
            LodPocMod::logAll(LogLevel::Info, "[GLTrace] Permanent glBindFramebuffer hooked at {:p}", target);
        }
    }

    installHeavyHooks();
}

void GlTrace::remove() {
    removeHeavyHooks();
    mPermanentHooks.clear();
    mOrigBindFramebuffer = nullptr;
}

void GlTrace::installHeavyHooks() {
    if (mHeavyHooksInstalled) return;

    void *glesLib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!glesLib) glesLib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_GLOBAL);

    auto hookFn = [this, glesLib](const char *primaryName, const std::vector<const char *> &alts, void *detour, void **orig) {
        void *target = nullptr;
        std::string foundName;

        std::vector<const char *> candidates;
        candidates.push_back(primaryName);
        for (const auto *a : alts) candidates.push_back(a);

        for (const auto *cand : candidates) {
            if (glesLib) target = dlsym(glesLib, cand);
            if (!target) target = dlsym(RTLD_DEFAULT, cand);
            if (target) {
                foundName = cand;
                break;
            }
        }

        if (target == nullptr) {
            LodPocMod::logAll(LogLevel::Info, "[GLTrace] Function '{}' not present in GLES driver.", primaryName);
            return;
        }

        pl::memory::HookHandle handle(target, detour, orig);
        if (!handle.installed()) {
            LodPocMod::logAll(LogLevel::Warn, "[GLTrace] Failed to hook '{}' at {:p}", foundName, target);
        } else {
            mHeavyHooks.push_back(std::move(handle));
            LodPocMod::logAll(LogLevel::Info, "[GLTrace] Hooked '{}' at {:p}", foundName, target);
        }
    };

    // Draw Hooks
    hookFn("glDrawArrays", {}, reinterpret_cast<void *>(&detourDrawArrays), reinterpret_cast<void **>(&mOrigDrawArrays));
    hookFn("glDrawArraysInstanced", {"glDrawArraysInstancedEXT", "glDrawArraysInstancedOES"}, reinterpret_cast<void *>(&detourDrawArraysInstanced), reinterpret_cast<void **>(&mOrigDrawArraysInstanced));
    hookFn("glDrawElements", {}, reinterpret_cast<void *>(&detourDrawElements), reinterpret_cast<void **>(&mOrigDrawElements));
    hookFn("glDrawElementsInstanced", {"glDrawElementsInstancedEXT", "glDrawElementsInstancedOES"}, reinterpret_cast<void *>(&detourDrawElementsInstanced), reinterpret_cast<void **>(&mOrigDrawElementsInstanced));
    hookFn("glDrawRangeElements", {"glDrawRangeElementsEXT", "glDrawRangeElementsOES"}, reinterpret_cast<void *>(&detourDrawRangeElements), reinterpret_cast<void **>(&mOrigDrawRangeElements));
    hookFn("glDrawElementsBaseVertex", {"glDrawElementsBaseVertexOES", "glDrawElementsBaseVertexEXT"}, reinterpret_cast<void *>(&detourDrawElementsBaseVertex), reinterpret_cast<void **>(&mOrigDrawElementsBaseVertex));
    hookFn("glDrawElementsInstancedBaseVertex", {"glDrawElementsInstancedBaseVertexOES", "glDrawElementsInstancedBaseVertexEXT"}, reinterpret_cast<void *>(&detourDrawElementsInstancedBaseVertex), reinterpret_cast<void **>(&mOrigDrawElementsInstancedBaseVertex));
    hookFn("glDrawRangeElementsBaseVertex", {"glDrawRangeElementsBaseVertexOES", "glDrawRangeElementsBaseVertexEXT"}, reinterpret_cast<void *>(&detourDrawRangeElementsBaseVertex), reinterpret_cast<void **>(&mOrigDrawRangeElementsBaseVertex));
    hookFn("glDrawArraysIndirect", {}, reinterpret_cast<void *>(&detourDrawArraysIndirect), reinterpret_cast<void **>(&mOrigDrawArraysIndirect));
    hookFn("glDrawElementsIndirect", {}, reinterpret_cast<void *>(&detourDrawElementsIndirect), reinterpret_cast<void **>(&mOrigDrawElementsIndirect));
    hookFn("glMultiDrawArraysEXT", {}, reinterpret_cast<void *>(&detourMultiDrawArraysEXT), reinterpret_cast<void **>(&mOrigMultiDrawArraysEXT));
    hookFn("glMultiDrawElementsEXT", {}, reinterpret_cast<void *>(&detourMultiDrawElementsEXT), reinterpret_cast<void **>(&mOrigMultiDrawElementsEXT));

    // State & Texture Tracking Hooks
    hookFn("glBindTexture", {}, reinterpret_cast<void *>(&detourBindTexture), reinterpret_cast<void **>(&mOrigBindTexture));
    hookFn("glBindRenderbuffer", {}, reinterpret_cast<void *>(&detourBindRenderbuffer), reinterpret_cast<void **>(&mOrigBindRenderbuffer));
    hookFn("glTexImage2D", {}, reinterpret_cast<void *>(&detourTexImage2D), reinterpret_cast<void **>(&mOrigTexImage2D));
    hookFn("glTexStorage2D", {"glTexStorage2DEXT"}, reinterpret_cast<void *>(&detourTexStorage2D), reinterpret_cast<void **>(&mOrigTexStorage2D));
    hookFn("glRenderbufferStorage", {}, reinterpret_cast<void *>(&detourRenderbufferStorage), reinterpret_cast<void **>(&mOrigRenderbufferStorage));
    hookFn("glFramebufferTexture2D", {}, reinterpret_cast<void *>(&detourFramebufferTexture2D), reinterpret_cast<void **>(&mOrigFramebufferTexture2D));
    hookFn("glFramebufferTextureLayer", {}, reinterpret_cast<void *>(&detourFramebufferTextureLayer), reinterpret_cast<void **>(&mOrigFramebufferTextureLayer));
    hookFn("glFramebufferRenderbuffer", {}, reinterpret_cast<void *>(&detourFramebufferRenderbuffer), reinterpret_cast<void **>(&mOrigFramebufferRenderbuffer));
    hookFn("glDrawBuffers", {"glDrawBuffersEXT", "glDrawBuffersNV"}, reinterpret_cast<void *>(&detourDrawBuffers), reinterpret_cast<void **>(&mOrigDrawBuffers));

    hookFn("glViewport", {}, reinterpret_cast<void *>(&detourViewport), reinterpret_cast<void **>(&mOrigViewport));
    hookFn("glEnable", {}, reinterpret_cast<void *>(&detourEnable), reinterpret_cast<void **>(&mOrigEnable));
    hookFn("glDisable", {}, reinterpret_cast<void *>(&detourDisable), reinterpret_cast<void **>(&mOrigDisable));
    hookFn("glDepthMask", {}, reinterpret_cast<void *>(&detourDepthMask), reinterpret_cast<void **>(&mOrigDepthMask));
    hookFn("glUseProgram", {}, reinterpret_cast<void *>(&detourUseProgram), reinterpret_cast<void **>(&mOrigUseProgram));

    mHeavyHooksInstalled = true;
    LodPocMod::logAll(LogLevel::Info, "[GLTrace] Installed {} heavy GL trace hooks", mHeavyHooks.size());
}

void GlTrace::removeHeavyHooks() {
    if (!mHeavyHooksInstalled) return;

    mHeavyHooks.clear();

    mOrigBindTexture = nullptr;
    mOrigBindRenderbuffer = nullptr;
    mOrigTexImage2D = nullptr;
    mOrigTexStorage2D = nullptr;
    mOrigRenderbufferStorage = nullptr;
    mOrigFramebufferTexture2D = nullptr;
    mOrigFramebufferTextureLayer = nullptr;
    mOrigFramebufferRenderbuffer = nullptr;
    mOrigDrawBuffers = nullptr;

    mOrigViewport = nullptr;
    mOrigEnable = nullptr;
    mOrigDisable = nullptr;
    mOrigDepthMask = nullptr;
    mOrigUseProgram = nullptr;

    mOrigDrawArrays = nullptr;
    mOrigDrawArraysInstanced = nullptr;
    mOrigDrawElements = nullptr;
    mOrigDrawElementsInstanced = nullptr;
    mOrigDrawRangeElements = nullptr;
    mOrigDrawElementsBaseVertex = nullptr;
    mOrigDrawElementsInstancedBaseVertex = nullptr;
    mOrigDrawRangeElementsBaseVertex = nullptr;
    mOrigDrawArraysIndirect = nullptr;
    mOrigDrawElementsIndirect = nullptr;
    mOrigMultiDrawArraysEXT = nullptr;
    mOrigMultiDrawElementsEXT = nullptr;

    mHeavyHooksInstalled = false;
    LodPocMod::logAll(LogLevel::Info, "[GLTrace] Heavy GL trace hooks successfully uninstalled");
}

} // namespace lod_poc
