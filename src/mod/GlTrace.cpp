#include "mod/GlTrace.h"
#include "mod/LodPocMod.h"
#include "mod/PocQuad.h"

#include <dlfcn.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstring>
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

const char *depthFuncToString(GLenum func) {
    switch (func) {
        case GL_NEVER:    return "NEVER";
        case GL_LESS:     return "LESS";
        case GL_EQUAL:    return "EQUAL";
        case GL_LEQUAL:   return "LEQUAL";
        case GL_GREATER:  return "GREATER";
        case GL_NOTEQUAL: return "NOTEQUAL";
        case GL_GEQUAL:   return "GEQUAL";
        case GL_ALWAYS:   return "ALWAYS";
        default:          return "UNKNOWN";
    }
}

const char *blendFactorToString(GLenum factor) {
    switch (factor) {
        case GL_ZERO:                return "ZERO";
        case GL_ONE:                 return "ONE";
        case GL_SRC_COLOR:           return "SRC_COLOR";
        case GL_ONE_MINUS_SRC_COLOR: return "ONE_MINUS_SRC_COLOR";
        case GL_SRC_ALPHA:           return "SRC_ALPHA";
        case GL_ONE_MINUS_SRC_ALPHA: return "ONE_MINUS_SRC_ALPHA";
        case GL_DST_ALPHA:           return "DST_ALPHA";
        case GL_ONE_MINUS_DST_ALPHA: return "ONE_MINUS_DST_ALPHA";
        case GL_DST_COLOR:           return "DST_COLOR";
        case GL_ONE_MINUS_DST_COLOR: return "ONE_MINUS_DST_COLOR";
        case GL_SRC_ALPHA_SATURATE:  return "SRC_ALPHA_SATURATE";
        default:                     return "UNKNOWN";
    }
}

const char *cullFaceToString(GLenum mode) {
    switch (mode) {
        case GL_FRONT:          return "FRONT";
        case GL_BACK:           return "BACK";
        case GL_FRONT_AND_BACK: return "FRONT_AND_BACK";
        default:                return "UNKNOWN";
    }
}

std::string clearMaskToString(GLbitfield mask) {
    std::string s;
    if (mask & GL_COLOR_BUFFER_BIT) s += "COLOR";
    if (mask & GL_DEPTH_BUFFER_BIT) {
        if (!s.empty()) s += " | ";
        s += "DEPTH";
    }
    if (mask & GL_STENCIL_BUFFER_BIT) {
        if (!s.empty()) s += " | ";
        s += "STENCIL";
    }
    return s.empty() ? "0" : s;
}

std::string attachmentToString(GLenum att) {
    if (att >= GL_COLOR_ATTACHMENT0 && att <= GL_COLOR_ATTACHMENT15) {
        return fmt::format("COLOR_ATTACHMENT{}", att - GL_COLOR_ATTACHMENT0);
    }
    switch (att) {
        case GL_DEPTH_ATTACHMENT:         return "DEPTH_ATTACHMENT";
        case GL_STENCIL_ATTACHMENT:       return "STENCIL_ATTACHMENT";
        case 0x821A:                      return "DEPTH_STENCIL_ATTACHMENT";
        case 0x8CD5:                      return "COLOR_ATTACHMENT0";
        case 0x84F9:                      return "DEPTH_ATTACHMENT_EXT";
        case 0x8D00:                      return "COLOR_ATTACHMENT0_EXT";
        case 0x8D20:                      return "STENCIL_ATTACHMENT_EXT";
        default:                          return fmt::format("0x{:x}", att);
    }
}

std::string formatFloats(const float *data, size_t count, size_t maxCount = 16) {
    size_t n = std::min(count, maxCount);
    std::string s = "[";
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) s += ", ";
        s += fmt::format("{:.4f}", data[i]);
    }
    if (count > maxCount) s += fmt::format(", ... (total {})", count);
    s += "]";
    return s;
}

} // namespace

GlTrace &GlTrace::instance() {
    static GlTrace sInstance;
    return sInstance;
}

GlTrace::GlTrace() = default;

void GlTrace::setConfig(bool traceFrame, bool traceRaw, const std::string &traceLabel, int traceDelayAfterWorldSec) {
    std::lock_guard lock(mStateMutex);
    mConfigTraceFrame = traceFrame;
    mConfigTraceRaw = traceRaw;
    mConfigTraceLabel = traceLabel;
    mConfigTraceDelayAfterWorldSec = traceDelayAfterWorldSec >= 0 ? traceDelayAfterWorldSec : 10;
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

    PocQuad::instance().onSwapBuffers(currentFps);

    const uint64_t frameDraws = mPermanentFrameDraws.exchange(0, std::memory_order_relaxed);
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
            mClearRecords.clear();
            mInvalidateRecords.clear();
            mFbo0DepthInvalidated = false;
            mUniformLogCount = 0;
            mMatrixCandidates.clear();
            startNewSegment(mCurrentBoundDrawFbo);
        }

        mState.store(TraceState::Recording, std::memory_order_release);
        LodPocMod::logAll(LogLevel::Info,
                          "[GLTrace] Began recording frame trace: '{}' at frame #{} (tid={})",
                          mActiveLabel, frameCount, tid);
        return;
    }

    // Check consecutive heavy frames for world detection (>500 draws)
    if (frameDraws > 500) {
        mConsecutiveHeavyFrames++;
    } else {
        mConsecutiveHeavyFrames = 0;
    }

    if (!mWorldDetected && mConsecutiveHeavyFrames >= 60) {
        mWorldDetected = true;
        mWorldDetectedTime = now;
        LodPocMod::logAll(LogLevel::Info,
                          "[GLTrace] world detected (60 consecutive frames with >500 draws, frame #{} had {} draws). Arming in-world trace in ~{}s.",
                          frameCount, frameDraws, mConfigTraceDelayAfterWorldSec);
    }

    // Check automatic triggers when idle
    if (currentState == TraceState::Idle && mConfigTraceFrame) {
        // Trigger 1: Main menu trace (frame > 50 and at least 30 draws)
        if (!mMainMenuTraced && frameCount > 50 && frameDraws >= 30) {
            mMainMenuTraced = true;
            armTrace("main_menu");
            return;
        }

        // Trigger 2: In-world trace (~10s after world detected)
        if (mWorldDetected && !mInWorldTraced) {
            if (now >= mWorldDetectedTime + std::chrono::seconds(mConfigTraceDelayAfterWorldSec)) {
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

    // Draw run tracking (collapse consecutive draws with identical state)
    DrawRunState currentState;
    currentState.depthTest = mCurrentDepthTest;
    currentState.depthMask = mCurrentDepthMask;
    currentState.depthFunc = mCurrentDepthFunc;
    currentState.depthNear = mCurrentDepthNear;
    currentState.depthFar = mCurrentDepthFar;
    currentState.blend = mCurrentBlend;
    currentState.blendSrcRgb = mCurrentBlendSrcRgb;
    currentState.blendDstRgb = mCurrentBlendDstRgb;
    currentState.blendSrcAlpha = mCurrentBlendSrcAlpha;
    currentState.blendDstAlpha = mCurrentBlendDstAlpha;
    currentState.cullFace = mCurrentCullFace;
    currentState.cullFaceMode = mCurrentCullFaceMode;
    currentState.program = mCurrentProgram;

    if (!seg.drawRuns.empty() && seg.drawRuns.back().state == currentState) {
        auto &run = seg.drawRuns.back();
        run.drawCount++;
        run.sumVertexCount += vCount;
        run.sumInstanceCount += static_cast<uint64_t>(record.instanceCount);
    } else {
        DrawRun newRun;
        newRun.runIndex = static_cast<uint32_t>(seg.drawRuns.size() + 1);
        newRun.state = currentState;
        newRun.drawCount = 1;
        newRun.sumVertexCount = vCount;
        newRun.sumInstanceCount = static_cast<uint64_t>(record.instanceCount);
        newRun.functionSummary = record.functionName;
        seg.drawRuns.push_back(newRun);
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

    // -------------------------------------------------------------------------
    // Stage 1d: Full Pass Segment Breakdown (Before / At / After FBO 1)
    // -------------------------------------------------------------------------
    LodPocMod::logAll(LogLevel::Info, "--- FULL PASS SEGMENT BREAKDOWN ---");
    bool seenNonZeroFbo = false;
    for (const auto &seg : mCurrentFrameSegments) {
        if (seg.fboId != 0) seenNonZeroFbo = true;
        std::string role = "FBO 0 Pass";
        if (seg.fboId != 0) {
            role = fmt::format("Offscreen FBO {} Pass", seg.fboId);
        } else if (!seenNonZeroFbo) {
            if (seg.totalDrawCalls > 500) role = "Main 3D World Pass (Pre-FBO1)";
            else if (seg.totalDrawCalls > 0) role = "Early Sky / Clear Pass (Pre-FBO1)";
            else role = "Setup / Target Pass (Pre-FBO1)";
        } else {
            role = "Post-Processing / 2D UI Pass (Post-FBO1)";
        }

        LodPocMod::logAll(LogLevel::Info,
            "  Segment #{:2d}: FBO {:2d} | Viewport: [{:4d}, {:4d}, {:4d}x{:4d}] | Draws: {:4d} | Verts: {:8d} | Role: [{}]",
            seg.segmentIndex, seg.fboId, seg.vpX, seg.vpY, seg.vpW, seg.vpH,
            seg.totalDrawCalls, seg.sumVertexCount, role);
    }

    // -------------------------------------------------------------------------
    // Stage 1a: Draw Runs for the Big FBO 0 Segment (First 80 Runs)
    // -------------------------------------------------------------------------
    const PassSegment *bigFbo0 = nullptr;
    for (const auto &seg : mCurrentFrameSegments) {
        if (seg.fboId == 0) {
            if (!bigFbo0 || seg.totalDrawCalls > bigFbo0->totalDrawCalls) {
                bigFbo0 = &seg;
            }
        }
    }

    if (bigFbo0 != nullptr && !bigFbo0->drawRuns.empty()) {
        size_t showCount = std::min<size_t>(80, bigFbo0->drawRuns.size());
        LodPocMod::logAll(LogLevel::Info,
            "--- DRAW RUNS: Segment #{} (Big FBO-0 Terrain Segment, Total Runs={}, Showing First {}) ---",
            bigFbo0->segmentIndex, bigFbo0->drawRuns.size(), showCount);
        for (size_t r = 0; r < showCount; ++r) {
            const auto &run = bigFbo0->drawRuns[r];
            const auto &st = run.state;
            LodPocMod::logAll(LogLevel::Info,
                "  Run #{:2d}: Draws={:4d} | Verts={:7d} | Prog={:3d} | DepthTest={} | DepthMask={} | DepthFunc={} | DepthRange=[{:.2f}, {:.2f}] | Blend={} (SrcRGB={}, DstRGB={}) | Cull={} ({}) | Func={}",
                run.runIndex, run.drawCount, run.sumVertexCount, st.program,
                st.depthTest ? "ON " : "OFF",
                st.depthMask ? "ON " : "OFF",
                depthFuncToString(st.depthFunc),
                st.depthNear, st.depthFar,
                st.blend ? "ON " : "OFF",
                blendFactorToString(st.blendSrcRgb), blendFactorToString(st.blendDstRgb),
                st.cullFace ? "ON " : "OFF", cullFaceToString(st.cullFaceMode),
                run.functionSummary);
        }
    }

    // -------------------------------------------------------------------------
    // Stage 1a: GL Clear Events
    // -------------------------------------------------------------------------
    LodPocMod::logAll(LogLevel::Info, "--- GL CLEAR EVENTS IN TRACED FRAME ---");
    if (mClearRecords.empty()) {
        LodPocMod::logAll(LogLevel::Info, "  (No glClear calls intercepted)");
    } else {
        for (const auto &c : mClearRecords) {
            LodPocMod::logAll(LogLevel::Info,
                "  Clear #{:2d}: FBO {:2d} | Mask=0x{:04x} ({}) | ClearDepth={:.4f} | Before Draw #{:4d}",
                c.clearIndex, c.fboId, c.mask, clearMaskToString(c.mask), c.clearDepth, c.drawCountBefore);
        }
    }

    // -------------------------------------------------------------------------
    // Stage 1b: Framebuffer Invalidate & Discard Events
    // -------------------------------------------------------------------------
    LodPocMod::logAll(LogLevel::Info, "--- FRAMEBUFFER INVALIDATE / DISCARD EVENTS ---");
    if (mInvalidateRecords.empty()) {
        LodPocMod::logAll(LogLevel::Info, "  (No glInvalidateFramebuffer or glDiscardFramebufferEXT calls intercepted)");
    } else {
        for (const auto &inv : mInvalidateRecords) {
            std::string attStr;
            for (const auto &a : inv.attachments) {
                if (!attStr.empty()) attStr += ", ";
                attStr += a;
            }
            LodPocMod::logAll(LogLevel::Info,
                "  Invalidate #{:2d}: {} on FBO {:2d} | Attachments: [{}] | Before Draw #{:4d}{}",
                inv.callIndex, inv.functionName, inv.fboId, attStr, inv.drawCountBefore,
                inv.invalidatedDepth ? " (DEPTH BUFFER INVALIDATED!)" : "");
        }
    }

    // -------------------------------------------------------------------------
    // Stage 1c: Candidate Perspective & View-Projection Matrices
    // -------------------------------------------------------------------------
    LodPocMod::logAll(LogLevel::Info, "--- MATRIX FINDER CANDIDATES SUMMARY ---");
    if (mMatrixCandidates.empty()) {
        LodPocMod::logAll(LogLevel::Info, "  (No candidate projection or view-projection matrices detected)");
    } else {
        for (const auto &mc : mMatrixCandidates) {
            LodPocMod::logAll(LogLevel::Info,
                "  [MatrixCandidate] Type: {} | Program: {:3d} | Before Draw Run #{:2d}: fov={:.1f} deg, aspect={:.3f}",
                mc.type, mc.program, mc.drawRunIndexBefore, mc.fovDeg, mc.aspect);
        }
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
    else if (cap == 0x0B44 /* GL_CULL_FACE */) mCurrentCullFace = true;
}

void GlTrace::onDisable(GLenum cap) {
    if (cap == 0x0B71 /* GL_DEPTH_TEST */) mCurrentDepthTest = false;
    else if (cap == 0x0BE2 /* GL_BLEND */) mCurrentBlend = false;
    else if (cap == 0x0B44 /* GL_CULL_FACE */) mCurrentCullFace = false;
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
    if (PocQuad::isRendering()) {
        if (inst.mOrigBindFramebuffer != nullptr) inst.mOrigBindFramebuffer(target, framebuffer);
        return;
    }

    if (PocQuad::instance().isEnabled()) {
        uint64_t frameDraws = inst.mPermanentFrameDraws.load(std::memory_order_relaxed);
        if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
            PocQuad::instance().onBindFramebufferBefore(framebuffer, inst.mCurrentBoundDrawFbo, frameDraws, inst.mCurrentDepthFunc, inst.mFbo0DepthInvalidated);
        }
    }

    if (inst.mOrigBindFramebuffer != nullptr) {
        inst.mOrigBindFramebuffer(target, framebuffer);
    }
    LodPocMod::instance().onBindFramebuffer(target, framebuffer);
    inst.onBindFramebuffer(target, framebuffer);
}

void GlTrace::detourBindTexture(GLenum target, GLuint texture) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigBindTexture != nullptr) inst.mOrigBindTexture(target, texture);
        return;
    }
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
    if (PocQuad::isRendering()) {
        if (inst.mOrigViewport != nullptr) inst.mOrigViewport(x, y, width, height);
        return;
    }
    if (inst.mOrigViewport != nullptr) inst.mOrigViewport(x, y, width, height);
    inst.onViewport(x, y, width, height);
}

void GlTrace::detourEnable(GLenum cap) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigEnable != nullptr) inst.mOrigEnable(cap);
        return;
    }
    if (inst.mOrigEnable != nullptr) inst.mOrigEnable(cap);
    inst.onEnable(cap);
}

void GlTrace::detourDisable(GLenum cap) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigDisable != nullptr) inst.mOrigDisable(cap);
        return;
    }
    if (inst.mOrigDisable != nullptr) inst.mOrigDisable(cap);
    inst.onDisable(cap);
}

void GlTrace::detourDepthMask(GLboolean flag) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigDepthMask != nullptr) inst.mOrigDepthMask(flag);
        return;
    }
    if (inst.mOrigDepthMask != nullptr) inst.mOrigDepthMask(flag);
    inst.onDepthMask(flag);
}

void GlTrace::detourUseProgram(GLuint program) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigUseProgram != nullptr) inst.mOrigUseProgram(program);
        return;
    }
    if (inst.mOrigUseProgram != nullptr) inst.mOrigUseProgram(program);
    inst.onUseProgram(program);
}

void GlTrace::detourDrawArrays(GLenum mode, GLint first, GLsizei count) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigDrawArrays != nullptr) inst.mOrigDrawArrays(mode, first, count);
        return;
    }
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
    if (PocQuad::isRendering()) {
        if (inst.mOrigDrawArraysInstanced != nullptr) inst.mOrigDrawArraysInstanced(mode, first, count, instancecount);
        return;
    }
    if (inst.mOrigDrawArraysInstanced != nullptr) inst.mOrigDrawArraysInstanced(mode, first, count, instancecount);
    inst.mPermanentFrameDraws.fetch_add(1, std::memory_order_relaxed);
    if (inst.mState.load(std::memory_order_relaxed) == TraceState::Recording) {
        RawDrawRecord rec;
        rec.functionName = "glDrawArraysInstanced";
        rec.mode = mode;
        rec.first = first;
        rec.count = count;
        rec.instanceCount = instancecount;
        inst.onDraw(rec);
    }
}

void GlTrace::detourDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigDrawElements != nullptr) inst.mOrigDrawElements(mode, count, type, indices);
        return;
    }
    if (inst.mOrigDrawElements != nullptr) inst.mOrigDrawElements(mode, count, type, indices);
    inst.mPermanentFrameDraws.fetch_add(1, std::memory_order_relaxed);
    if (inst.mState.load(std::memory_order_relaxed) == TraceState::Recording) {
        RawDrawRecord rec;
        rec.functionName = "glDrawElements";
        rec.mode = mode;
        rec.count = count;
        rec.type = type;
        rec.indicesOrOffset = reinterpret_cast<uintptr_t>(indices);
        rec.instanceCount = 1;
        inst.onDraw(rec);
    }
}

void GlTrace::detourDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigDrawElementsInstanced != nullptr) inst.mOrigDrawElementsInstanced(mode, count, type, indices, instancecount);
        return;
    }
    if (inst.mOrigDrawElementsInstanced != nullptr) inst.mOrigDrawElementsInstanced(mode, count, type, indices, instancecount);
    inst.mPermanentFrameDraws.fetch_add(1, std::memory_order_relaxed);
    if (inst.mState.load(std::memory_order_relaxed) == TraceState::Recording) {
        RawDrawRecord rec;
        rec.functionName = "glDrawElementsInstanced";
        rec.mode = mode;
        rec.count = count;
        rec.type = type;
        rec.indicesOrOffset = reinterpret_cast<uintptr_t>(indices);
        rec.instanceCount = instancecount;
        inst.onDraw(rec);
    }
}

void GlTrace::detourDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawRangeElements != nullptr) inst.mOrigDrawRangeElements(mode, start, end, count, type, indices);
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
// Stage 1: Additional Detour Implementations
// -----------------------------------------------------------------------------

void GlTrace::detourDepthFunc(GLenum func) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigDepthFunc != nullptr) inst.mOrigDepthFunc(func);
        return;
    }
    if (inst.mOrigDepthFunc != nullptr) inst.mOrigDepthFunc(func);
    inst.mCurrentDepthFunc = func;
}

void GlTrace::detourDepthRangef(GLfloat n, GLfloat f) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigDepthRangef != nullptr) inst.mOrigDepthRangef(n, f);
        return;
    }
    if (inst.mOrigDepthRangef != nullptr) inst.mOrigDepthRangef(n, f);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;
    inst.mCurrentDepthNear = n;
    inst.mCurrentDepthFar = f;
}

void GlTrace::detourBlendFunc(GLenum sfactor, GLenum dfactor) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigBlendFunc != nullptr) inst.mOrigBlendFunc(sfactor, dfactor);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;
    inst.mCurrentBlendSrcRgb = sfactor;
    inst.mCurrentBlendDstRgb = dfactor;
    inst.mCurrentBlendSrcAlpha = sfactor;
    inst.mCurrentBlendDstAlpha = dfactor;
}

void GlTrace::detourBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigBlendFuncSeparate != nullptr) inst.mOrigBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
        return;
    }
    if (inst.mOrigBlendFuncSeparate != nullptr) inst.mOrigBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;
    inst.mCurrentBlendSrcRgb = srcRGB;
    inst.mCurrentBlendDstRgb = dstRGB;
    inst.mCurrentBlendSrcAlpha = srcAlpha;
    inst.mCurrentBlendDstAlpha = dstAlpha;
}

void GlTrace::detourCullFace(GLenum mode) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigCullFace != nullptr) inst.mOrigCullFace(mode);
        return;
    }
    if (inst.mOrigCullFace != nullptr) inst.mOrigCullFace(mode);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;
    inst.mCurrentCullFaceMode = mode;
}

void GlTrace::detourClearDepthf(GLfloat depth) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigClearDepthf != nullptr) inst.mOrigClearDepthf(depth);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;
    inst.mCurrentClearDepth = depth;
}

void GlTrace::detourClear(GLbitfield mask) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigClear != nullptr) inst.mOrigClear(mask);
        return;
    }

    if (inst.mOrigClear != nullptr) inst.mOrigClear(mask);

    if (PocQuad::instance().isEnabled()) {
        uint64_t frameDraws = inst.mPermanentFrameDraws.load(std::memory_order_relaxed);
        PocQuad::instance().onClearAfter(mask, inst.mCurrentBoundDrawFbo, frameDraws, inst.mCurrentDepthFunc);
    }

    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;

    ClearRecord cr;
    cr.clearIndex = static_cast<uint32_t>(inst.mClearRecords.size() + 1);
    cr.fboId = inst.mCurrentBoundDrawFbo;
    cr.mask = mask;
    cr.clearDepth = inst.mCurrentClearDepth;
    cr.drawCountBefore = static_cast<uint32_t>(inst.mPermanentFrameDraws.load(std::memory_order_relaxed));
    inst.mClearRecords.push_back(cr);

    LodPocMod::logAll(LogLevel::Info,
        "[GLTrace: Clear] glClear(mask=0x{:04x} [{}]) on FBO {} (clearDepth={:.4f}) at frame draw #{}",
        mask, clearMaskToString(mask), inst.mCurrentBoundDrawFbo, inst.mCurrentClearDepth, cr.drawCountBefore);
}

void GlTrace::detourInvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum *attachments) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigInvalidateFramebuffer != nullptr) inst.mOrigInvalidateFramebuffer(target, numAttachments, attachments);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;

    InvalidateRecord ir;
    ir.callIndex = static_cast<uint32_t>(inst.mInvalidateRecords.size() + 1);
    ir.functionName = "glInvalidateFramebuffer";
    ir.fboId = inst.mCurrentBoundDrawFbo;
    ir.drawCountBefore = static_cast<uint32_t>(inst.mPermanentFrameDraws.load(std::memory_order_relaxed));
    ir.invalidatedDepth = false;

    std::string attStr;
    if (attachments != nullptr) {
        for (GLsizei i = 0; i < numAttachments; ++i) {
            std::string name = attachmentToString(attachments[i]);
            ir.attachments.push_back(name);
            if (!attStr.empty()) attStr += ", ";
            attStr += name;
            if (attachments[i] == GL_DEPTH_ATTACHMENT || attachments[i] == 0x821A || attachments[i] == 0x84F9) {
                ir.invalidatedDepth = true;
                if (inst.mCurrentBoundDrawFbo == 0) {
                    inst.mFbo0DepthInvalidated = true;
                }
            }
        }
    }
    inst.mInvalidateRecords.push_back(ir);

    LodPocMod::logAll(LogLevel::Info,
        "[GLTrace: Invalidate] glInvalidateFramebuffer(target=0x{:x}, count={}) on FBO {} with attachments: [{}] at frame draw #{}{}",
        target, numAttachments, inst.mCurrentBoundDrawFbo, attStr, ir.drawCountBefore,
        ir.invalidatedDepth ? " (DEPTH BUFFER INVALIDATED!)" : "");
}

void GlTrace::detourInvalidateSubFramebuffer(GLenum target, GLsizei numAttachments, const GLenum *attachments, GLint x, GLint y, GLsizei width, GLsizei height) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigInvalidateSubFramebuffer != nullptr) inst.mOrigInvalidateSubFramebuffer(target, numAttachments, attachments, x, y, width, height);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;

    InvalidateRecord ir;
    ir.callIndex = static_cast<uint32_t>(inst.mInvalidateRecords.size() + 1);
    ir.functionName = "glInvalidateSubFramebuffer";
    ir.fboId = inst.mCurrentBoundDrawFbo;
    ir.drawCountBefore = static_cast<uint32_t>(inst.mPermanentFrameDraws.load(std::memory_order_relaxed));
    ir.invalidatedDepth = false;

    std::string attStr;
    if (attachments != nullptr) {
        for (GLsizei i = 0; i < numAttachments; ++i) {
            std::string name = attachmentToString(attachments[i]);
            ir.attachments.push_back(name);
            if (!attStr.empty()) attStr += ", ";
            attStr += name;
            if (attachments[i] == GL_DEPTH_ATTACHMENT || attachments[i] == 0x821A || attachments[i] == 0x84F9) {
                ir.invalidatedDepth = true;
                if (inst.mCurrentBoundDrawFbo == 0) {
                    inst.mFbo0DepthInvalidated = true;
                }
            }
        }
    }
    inst.mInvalidateRecords.push_back(ir);

    LodPocMod::logAll(LogLevel::Info,
        "[GLTrace: Invalidate] glInvalidateSubFramebuffer(rect=[{},{},{}x{}], count={}) on FBO {} with attachments: [{}] at frame draw #{}",
        x, y, width, height, numAttachments, inst.mCurrentBoundDrawFbo, attStr, ir.drawCountBefore);
}

void GlTrace::detourDiscardFramebufferEXT(GLenum target, GLsizei numAttachments, const GLenum *attachments) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDiscardFramebufferEXT != nullptr) inst.mOrigDiscardFramebufferEXT(target, numAttachments, attachments);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;

    InvalidateRecord ir;
    ir.callIndex = static_cast<uint32_t>(inst.mInvalidateRecords.size() + 1);
    ir.functionName = "glDiscardFramebufferEXT";
    ir.fboId = inst.mCurrentBoundDrawFbo;
    ir.drawCountBefore = static_cast<uint32_t>(inst.mPermanentFrameDraws.load(std::memory_order_relaxed));
    ir.invalidatedDepth = false;

    std::string attStr;
    if (attachments != nullptr) {
        for (GLsizei i = 0; i < numAttachments; ++i) {
            std::string name = attachmentToString(attachments[i]);
            ir.attachments.push_back(name);
            if (!attStr.empty()) attStr += ", ";
            attStr += name;
            if (attachments[i] == GL_DEPTH_ATTACHMENT || attachments[i] == 0x821A || attachments[i] == 0x84F9) {
                ir.invalidatedDepth = true;
                if (inst.mCurrentBoundDrawFbo == 0) {
                    inst.mFbo0DepthInvalidated = true;
                }
            }
        }
    }
    inst.mInvalidateRecords.push_back(ir);

    LodPocMod::logAll(LogLevel::Info,
        "[GLTrace: Invalidate] glDiscardFramebufferEXT on FBO {} with attachments: [{}] at frame draw #{}{}",
        inst.mCurrentBoundDrawFbo, attStr, ir.drawCountBefore,
        ir.invalidatedDepth ? " (DEPTH BUFFER DISCARDED!)" : "");
}

void GlTrace::detourUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigUniformMatrix4fv != nullptr) inst.mOrigUniformMatrix4fv(location, count, transpose, value);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;

    uint32_t currentRunIdx = 1;
    if (!inst.mCurrentFrameSegments.empty()) {
        currentRunIdx = static_cast<uint32_t>(inst.mCurrentFrameSegments.back().drawRuns.size() + 1);
    }

    if (value != nullptr && count > 0) {
        for (GLsizei i = 0; i < count; ++i) {
            inst.checkMatrixCandidate(inst.mCurrentProgram, currentRunIdx, value + (i * 16), "glUniformMatrix4fv");
        }
    }

    if (inst.mUniformLogCount < 300) {
        inst.mUniformLogCount++;
        std::string valStr = (value != nullptr && count > 0) ? formatFloats(value, count * 16, 16) : "null";
        LodPocMod::logAll(LogLevel::Info,
            "[GLTrace: Uniform] glUniformMatrix4fv(loc={}, count={}, trans={}) Prog={}: {}",
            location, count, transpose ? "TRUE" : "FALSE", inst.mCurrentProgram, valStr);
    }
}

void GlTrace::detourUniform4fv(GLint location, GLsizei count, const GLfloat *value) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigUniform4fv != nullptr) inst.mOrigUniform4fv(location, count, value);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;

    if (inst.mUniformLogCount < 300) {
        inst.mUniformLogCount++;
        std::string valStr = (value != nullptr && count > 0) ? formatFloats(value, count * 4, 16) : "null";
        LodPocMod::logAll(LogLevel::Info,
            "[GLTrace: Uniform] glUniform4fv(loc={}, count={}) Prog={}: {}",
            location, count, inst.mCurrentProgram, valStr);
    }
}

void GlTrace::detourBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigBindBufferBase != nullptr) inst.mOrigBindBufferBase(target, index, buffer);
        return;
    }
    if (inst.mOrigBindBufferBase != nullptr) inst.mOrigBindBufferBase(target, index, buffer);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;

    if (inst.mUniformLogCount < 300) {
        inst.mUniformLogCount++;
        LodPocMod::logAll(LogLevel::Info,
            "[GLTrace: Buffer] glBindBufferBase(target=0x{:x}, index={}, buffer={}) Prog={}",
            target, index, buffer, inst.mCurrentProgram);
    }
}

void GlTrace::detourBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    auto &inst = GlTrace::instance();
    if (PocQuad::isRendering()) {
        if (inst.mOrigBindBufferRange != nullptr) inst.mOrigBindBufferRange(target, index, buffer, offset, size);
        return;
    }
    if (inst.mOrigBindBufferRange != nullptr) inst.mOrigBindBufferRange(target, index, buffer, offset, size);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;

    if (inst.mUniformLogCount < 300) {
        inst.mUniformLogCount++;
        LodPocMod::logAll(LogLevel::Info,
            "[GLTrace: Buffer] glBindBufferRange(target=0x{:x}, index={}, buffer={}, offset={}, size={}) Prog={}",
            target, index, buffer, offset, size, inst.mCurrentProgram);
    }
}

void GlTrace::detourBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigBufferSubData != nullptr) inst.mOrigBufferSubData(target, offset, size, data);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;

    uint32_t currentRunIdx = 1;
    if (!inst.mCurrentFrameSegments.empty()) {
        currentRunIdx = static_cast<uint32_t>(inst.mCurrentFrameSegments.back().drawRuns.size() + 1);
    }

    if (data != nullptr && size >= 64) {
        const float *fData = reinterpret_cast<const float *>(data);
        size_t floatCount = size / sizeof(float);
        for (size_t i = 0; i + 16 <= floatCount && i < 64; i += 16) {
            inst.checkMatrixCandidate(inst.mCurrentProgram, currentRunIdx, fData + i, "glBufferSubData");
        }
    }

    if (size <= 1024 && inst.mUniformLogCount < 300) {
        inst.mUniformLogCount++;
        std::string valStr = "null";
        if (data != nullptr) {
            valStr = formatFloats(reinterpret_cast<const float *>(data), size / sizeof(float), 16);
        }
        LodPocMod::logAll(LogLevel::Info,
            "[GLTrace: Buffer] glBufferSubData(target=0x{:x}, offset={}, size={}) Prog={}: {}",
            target, offset, size, inst.mCurrentProgram, valStr);
    }
}

void GlTrace::detourBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigBufferData != nullptr) inst.mOrigBufferData(target, size, data, usage);
    if (inst.mState.load(std::memory_order_relaxed) != TraceState::Recording) return;
    if (gettid() != inst.getRenderThreadId()) return;

    uint32_t currentRunIdx = 1;
    if (!inst.mCurrentFrameSegments.empty()) {
        currentRunIdx = static_cast<uint32_t>(inst.mCurrentFrameSegments.back().drawRuns.size() + 1);
    }

    if (data != nullptr && size >= 64) {
        const float *fData = reinterpret_cast<const float *>(data);
        size_t floatCount = size / sizeof(float);
        for (size_t i = 0; i + 16 <= floatCount && i < 64; i += 16) {
            inst.checkMatrixCandidate(inst.mCurrentProgram, currentRunIdx, fData + i, "glBufferData");
        }
    }

    if (size <= 1024 && inst.mUniformLogCount < 300) {
        inst.mUniformLogCount++;
        std::string valStr = "null";
        if (data != nullptr) {
            valStr = formatFloats(reinterpret_cast<const float *>(data), size / sizeof(float), 16);
        }
        LodPocMod::logAll(LogLevel::Info,
            "[GLTrace: Buffer] glBufferData(target=0x{:x}, size={}, usage=0x{:x}) Prog={}: {}",
            target, size, usage, inst.mCurrentProgram, valStr);
    }
}

void *GlTrace::detourMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    auto &inst = GlTrace::instance();
    void *res = inst.mOrigMapBufferRange ? inst.mOrigMapBufferRange(target, offset, length, access) : nullptr;
    if (inst.mState.load(std::memory_order_relaxed) == TraceState::Recording && gettid() == inst.getRenderThreadId()) {
        if (inst.mUniformLogCount < 300) {
            inst.mUniformLogCount++;
            LodPocMod::logAll(LogLevel::Info,
                "[GLTrace: Buffer] glMapBufferRange(target=0x{:x}, off={}, len={}, access=0x{:x}) -> {:p} Prog={}",
                target, offset, length, access, res, inst.mCurrentProgram);
        }
    }
    return res;
}

GLboolean GlTrace::detourUnmapBuffer(GLenum target) {
    auto &inst = GlTrace::instance();
    GLboolean res = inst.mOrigUnmapBuffer ? inst.mOrigUnmapBuffer(target) : GL_TRUE;
    if (inst.mState.load(std::memory_order_relaxed) == TraceState::Recording && gettid() == inst.getRenderThreadId()) {
        if (inst.mUniformLogCount < 300) {
            inst.mUniformLogCount++;
            LodPocMod::logAll(LogLevel::Info,
                "[GLTrace: Buffer] glUnmapBuffer(target=0x{:x}) Prog={}", target, inst.mCurrentProgram);
        }
    }
    return res;
}

void GlTrace::checkMatrixCandidate(GLuint prog, uint32_t drawRunIdx, const float *m, const char *source) {
    if (m == nullptr) return;
    GLsizei vpW = mCurrentVpW > 0 ? mCurrentVpW : 2392;
    GLsizei vpH = mCurrentVpH > 0 ? mCurrentVpH : 1080;
    const float aspect = static_cast<float>(vpW) / static_cast<float>(vpH);

    // 1. Perspective Projection Check
    auto testPerspective = [&](float m00, float m11, float mZSign, bool transposed) -> bool {
        if (m11 < 0.2f || m11 > 10.0f) return false;
        if (std::abs(mZSign - (-1.0f)) > 0.15f && std::abs(mZSign - 1.0f) > 0.15f) return false;

        float expectedM00 = m11 / aspect;
        if (std::abs(m00 - expectedM00) > 0.15f * expectedM00) return false;

        float fovRad = 2.0f * std::atan(1.0f / m11);
        float fovDeg = fovRad * (180.0f / 3.141592653589793f);
        if (fovDeg < 25.0f || fovDeg > 125.0f) return false;

        MatrixCandidate mc;
        mc.program = prog;
        mc.drawRunIndexBefore = drawRunIdx;
        mc.type = transposed ? "PROJECTION (Transposed/Row-Major)" : "PROJECTION (Col-Major)";
        mc.fovDeg = fovDeg;
        mc.aspect = aspect;
        std::copy(m, m + 16, mc.values);
        mMatrixCandidates.push_back(mc);

        LodPocMod::logAll(LogLevel::Info,
            "[MatrixFinder] >>> PERSPECTIVE PROJECTION MATRIX DETECTED ({}) <<<", mc.type);
        LodPocMod::logAll(LogLevel::Info,
            "[MatrixFinder]   Prog: {} | Before Draw Run #{:2d} | Source: {} | FOV: {:.1f} deg | Aspect: {:.3f}",
            prog, drawRunIdx, source, fovDeg, aspect);
        LodPocMod::logAll(LogLevel::Info,
            "[MatrixFinder]   Row 0: [{:8.4f}, {:8.4f}, {:8.4f}, {:8.4f}]", m[0], m[1], m[2], m[3]);
        LodPocMod::logAll(LogLevel::Info,
            "[MatrixFinder]   Row 1: [{:8.4f}, {:8.4f}, {:8.4f}, {:8.4f}]", m[4], m[5], m[6], m[7]);
        LodPocMod::logAll(LogLevel::Info,
            "[MatrixFinder]   Row 2: [{:8.4f}, {:8.4f}, {:8.4f}, {:8.4f}]", m[8], m[9], m[10], m[11]);
        LodPocMod::logAll(LogLevel::Info,
            "[MatrixFinder]   Row 3: [{:8.4f}, {:8.4f}, {:8.4f}, {:8.4f}]", m[12], m[13], m[14], m[15]);
        return true;
    };

    if (!testPerspective(m[0], m[5], m[11], false)) {
        testPerspective(m[0], m[5], m[14], true);
    }

    // 2. View-Projection Check
    float row3LenSq = m[3] * m[3] + m[7] * m[7] + m[11] * m[11];
    float col3LenSq = m[12] * m[12] + m[13] * m[13] + m[14] * m[14];

    if ((std::abs(row3LenSq - 1.0f) < 0.25f && (std::abs(m[3]) > 0.05f || std::abs(m[7]) > 0.05f || std::abs(m[11]) > 0.05f)) ||
        (std::abs(col3LenSq - 1.0f) < 0.25f && (std::abs(m[12]) > 0.05f || std::abs(m[13]) > 0.05f || std::abs(m[14]) > 0.05f))) {
        bool alreadyLogged = false;
        for (const auto &c : mMatrixCandidates) {
            if (c.program == prog && c.drawRunIndexBefore == drawRunIdx && std::memcmp(c.values, m, 16 * sizeof(float)) == 0) {
                alreadyLogged = true;
                break;
            }
        }
        if (!alreadyLogged && mMatrixCandidates.size() < 50) {
            MatrixCandidate mc;
            mc.program = prog;
            mc.drawRunIndexBefore = drawRunIdx;
            mc.type = "VIEW-PROJECTION";
            mc.fovDeg = 0.0f;
            mc.aspect = aspect;
            std::copy(m, m + 16, mc.values);
            mMatrixCandidates.push_back(mc);

            LodPocMod::logAll(LogLevel::Info,
                "[MatrixFinder] >>> VIEW-PROJECTION MATRIX DETECTED <<<");
            LodPocMod::logAll(LogLevel::Info,
                "[MatrixFinder]   Prog: {} | Before Draw Run #{:2d} | Source: {}",
                prog, drawRunIdx, source);
            LodPocMod::logAll(LogLevel::Info,
                "[MatrixFinder]   Row 0: [{:8.4f}, {:8.4f}, {:8.4f}, {:8.4f}]", m[0], m[1], m[2], m[3]);
            LodPocMod::logAll(LogLevel::Info,
                "[MatrixFinder]   Row 1: [{:8.4f}, {:8.4f}, {:8.4f}, {:8.4f}]", m[4], m[5], m[6], m[7]);
            LodPocMod::logAll(LogLevel::Info,
                "[MatrixFinder]   Row 2: [{:8.4f}, {:8.4f}, {:8.4f}, {:8.4f}]", m[8], m[9], m[10], m[11]);
            LodPocMod::logAll(LogLevel::Info,
                "[MatrixFinder]   Row 3: [{:8.4f}, {:8.4f}, {:8.4f}, {:8.4f}]", m[12], m[13], m[14], m[15]);
        }
    }
}
// -----------------------------------------------------------------------------

void GlTrace::install() {
    remove();

    void *glesLib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!glesLib) glesLib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_GLOBAL);

    auto hookPermanent = [this, glesLib](const char *primaryName, const std::vector<const char *> &alts, void *detour, void **orig) {
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
            LodPocMod::logAll(LogLevel::Info, "[GLTrace] Permanent hook target '{}' not present in GLES driver.", primaryName);
            return;
        }

        pl::memory::HookHandle handle(target, detour, orig);
        if (!handle.installed()) {
            LodPocMod::logAll(LogLevel::Warn, "[GLTrace] Failed to hook permanent '{}' at {:p}", foundName, target);
        } else {
            mPermanentHooks.push_back(std::move(handle));
            LodPocMod::logAll(LogLevel::Info, "[GLTrace] Permanent hook '{}' installed at {:p}", foundName, target);
        }
    };

    hookPermanent("glBindFramebuffer", {},
                  reinterpret_cast<void *>(&detourBindFramebuffer),
                  reinterpret_cast<void **>(&mOrigBindFramebuffer));

    hookPermanent("glDrawElements", {},
                  reinterpret_cast<void *>(&detourDrawElements),
                  reinterpret_cast<void **>(&mOrigDrawElements));

    hookPermanent("glDrawElementsInstanced", {"glDrawElementsInstancedEXT", "glDrawElementsInstancedOES"},
                  reinterpret_cast<void *>(&detourDrawElementsInstanced),
                  reinterpret_cast<void **>(&mOrigDrawElementsInstanced));

    hookPermanent("glDrawArraysInstanced", {"glDrawArraysInstancedEXT", "glDrawArraysInstancedOES"},
                  reinterpret_cast<void *>(&detourDrawArraysInstanced),
                  reinterpret_cast<void **>(&mOrigDrawArraysInstanced));

    hookPermanent("glClear", {},
                  reinterpret_cast<void *>(&detourClear),
                  reinterpret_cast<void **>(&mOrigClear));

    hookPermanent("glDepthFunc", {},
                  reinterpret_cast<void *>(&detourDepthFunc),
                  reinterpret_cast<void **>(&mOrigDepthFunc));
}

void GlTrace::remove() {
    removeHeavyHooks();
    mPermanentHooks.clear();
    mOrigBindFramebuffer = nullptr;
    mOrigDrawElements = nullptr;
    mOrigDrawElementsInstanced = nullptr;
    mOrigDrawArraysInstanced = nullptr;
    mOrigClear = nullptr;
    mOrigDepthFunc = nullptr;
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

    // Draw Hooks (glDrawElements, glDrawElementsInstanced, glDrawArraysInstanced are handled by permanent hooks)
    hookFn("glDrawArrays", {}, reinterpret_cast<void *>(&detourDrawArrays), reinterpret_cast<void **>(&mOrigDrawArrays));
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

    // Stage 1: Depth, Blend, Cull, Invalidate, Uniforms & Buffers
    hookFn("glDepthRangef", {}, reinterpret_cast<void *>(&detourDepthRangef), reinterpret_cast<void **>(&mOrigDepthRangef));
    hookFn("glBlendFunc", {}, reinterpret_cast<void *>(&detourBlendFunc), reinterpret_cast<void **>(&mOrigBlendFunc));
    hookFn("glBlendFuncSeparate", {}, reinterpret_cast<void *>(&detourBlendFuncSeparate), reinterpret_cast<void **>(&mOrigBlendFuncSeparate));
    hookFn("glCullFace", {}, reinterpret_cast<void *>(&detourCullFace), reinterpret_cast<void **>(&mOrigCullFace));
    hookFn("glClearDepthf", {"glClearDepth"}, reinterpret_cast<void *>(&detourClearDepthf), reinterpret_cast<void **>(&mOrigClearDepthf));
    hookFn("glInvalidateFramebuffer", {}, reinterpret_cast<void *>(&detourInvalidateFramebuffer), reinterpret_cast<void **>(&mOrigInvalidateFramebuffer));
    hookFn("glInvalidateSubFramebuffer", {}, reinterpret_cast<void *>(&detourInvalidateSubFramebuffer), reinterpret_cast<void **>(&mOrigInvalidateSubFramebuffer));
    hookFn("glDiscardFramebufferEXT", {"glDiscardFramebufferOES"}, reinterpret_cast<void *>(&detourDiscardFramebufferEXT), reinterpret_cast<void **>(&mOrigDiscardFramebufferEXT));
    hookFn("glUniformMatrix4fv", {}, reinterpret_cast<void *>(&detourUniformMatrix4fv), reinterpret_cast<void **>(&mOrigUniformMatrix4fv));
    hookFn("glUniform4fv", {}, reinterpret_cast<void *>(&detourUniform4fv), reinterpret_cast<void **>(&mOrigUniform4fv));
    hookFn("glBindBufferBase", {}, reinterpret_cast<void *>(&detourBindBufferBase), reinterpret_cast<void **>(&mOrigBindBufferBase));
    hookFn("glBindBufferRange", {}, reinterpret_cast<void *>(&detourBindBufferRange), reinterpret_cast<void **>(&mOrigBindBufferRange));
    hookFn("glBufferSubData", {}, reinterpret_cast<void *>(&detourBufferSubData), reinterpret_cast<void **>(&mOrigBufferSubData));
    hookFn("glBufferData", {}, reinterpret_cast<void *>(&detourBufferData), reinterpret_cast<void **>(&mOrigBufferData));
    hookFn("glMapBufferRange", {"glMapBufferRangeEXT"}, reinterpret_cast<void *>(&detourMapBufferRange), reinterpret_cast<void **>(&mOrigMapBufferRange));
    hookFn("glUnmapBuffer", {"glUnmapBufferOES", "glUnmapBufferEXT"}, reinterpret_cast<void *>(&detourUnmapBuffer), reinterpret_cast<void **>(&mOrigUnmapBuffer));

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

    mOrigDepthRangef = nullptr;
    mOrigBlendFunc = nullptr;
    mOrigBlendFuncSeparate = nullptr;
    mOrigCullFace = nullptr;
    mOrigClearDepthf = nullptr;
    mOrigInvalidateFramebuffer = nullptr;
    mOrigInvalidateSubFramebuffer = nullptr;
    mOrigDiscardFramebufferEXT = nullptr;
    mOrigUniformMatrix4fv = nullptr;
    mOrigUniform4fv = nullptr;
    mOrigBindBufferBase = nullptr;
    mOrigBindBufferRange = nullptr;
    mOrigBufferSubData = nullptr;
    mOrigBufferData = nullptr;
    mOrigMapBufferRange = nullptr;
    mOrigUnmapBuffer = nullptr;

    mOrigDrawArrays = nullptr;
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
