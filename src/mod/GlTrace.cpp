#include "mod/GlTrace.h"
#include "mod/LodPocMod.h"

#include <dlfcn.h>
#include <unistd.h>
#include <algorithm>
#include <sstream>

namespace lod_poc {

namespace {

const char *capToString(GLenum cap) {
    switch (cap) {
        case 0x0B71: return "GL_DEPTH_TEST";
        case 0x0BE2: return "GL_BLEND";
        case 0x0B44: return "GL_CULL_FACE";
        case 0x0C11: return "GL_SCISSOR_TEST";
        case 0x0B90: return "GL_STENCIL_TEST";
        case 0x8037: return "GL_POLYGON_OFFSET_FILL";
        case 0x809E: return "GL_SAMPLE_ALPHA_TO_COVERAGE";
        case 0x80A0: return "GL_SAMPLE_COVERAGE";
        default:     return nullptr;
    }
}

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

std::string formatClearMask(GLbitfield mask) {
    std::string s;
    if (mask & GL_COLOR_BUFFER_BIT) s += "COLOR ";
    if (mask & GL_DEPTH_BUFFER_BIT) s += "DEPTH ";
    if (mask & GL_STENCIL_BUFFER_BIT) s += "STENCIL ";
    return s.empty() ? "NONE" : s;
}

} // namespace

GlTrace &GlTrace::instance() {
    static GlTrace sInstance;
    return sInstance;
}

GlTrace::GlTrace() {
    mEvents.resize(kMaxEvents);
}

void GlTrace::setConfig(bool traceFrame, const std::string &traceLabel) {
    std::lock_guard lock(mStateMutex);
    mConfigTraceFrame = traceFrame;
    mConfigTraceLabel = traceLabel;
    if (traceFrame && !traceLabel.empty() && traceLabel != mLastTracedLabel) {
        armTrace(traceLabel);
    }
}

void GlTrace::armTrace(const std::string &label) {
    std::lock_guard lock(mStateMutex);
    mArmedLabel = label;
    mState.store(TraceState::Armed, std::memory_order_release);
    LodPocMod::logAll(LogLevel::Info, "[Probe A: GLTrace] Armed for single-frame trace: '{}'", label);
}

void GlTrace::onSwapBuffers(uint64_t frameCount) {
    const pid_t tid = gettid();
    setRenderThreadId(tid);

    const uint64_t frameDraws = mFrameDrawElements.exchange(0, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();

    TraceState currentState = mState.load(std::memory_order_acquire);

    // If currently recording, this swap buffers marks the END of the traced frame!
    if (currentState == TraceState::Recording) {
        mState.store(TraceState::Idle, std::memory_order_release);
        size_t count = mEventCount.load(std::memory_order_acquire);
        std::string label = mActiveLabel;
        mLastTracedLabel = label;
        dumpCompressedSummary(label, count);
        return;
    }

    // If armed, start recording at this frame boundary!
    if (currentState == TraceState::Armed) {
        std::lock_guard lock(mStateMutex);
        mActiveLabel = mArmedLabel;
        mEventCount.store(0, std::memory_order_relaxed);
        mRunningDrawIndex.store(0, std::memory_order_relaxed);
        mState.store(TraceState::Recording, std::memory_order_release);
        LodPocMod::logAll(LogLevel::Info,
                          "[Probe A: GLTrace] Began recording frame trace: '{}' at frame #{} (tid={})",
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
                              "[Probe A: GLTrace] In-world heavy draw detected (#{} draws in frame {}). Arming in-world trace in ~10s.",
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

void GlTrace::recordEvent(const GlEvent &ev) {
    size_t idx = mEventCount.fetch_add(1, std::memory_order_relaxed);
    if (idx < kMaxEvents) {
        mEvents[idx] = ev;
    }
}

void GlTrace::dumpCompressedSummary(const std::string &label, size_t count) {
    const size_t totalEvents = std::min(count, kMaxEvents);
    LodPocMod::logAll(LogLevel::Info,
                      "[Probe A: GLTrace] ==================== FRAME TRACE: {} ====================",
                      label);
    LodPocMod::logAll(LogLevel::Info,
                      "[Probe A: GLTrace] Recorded {} events (capped at {})",
                      count, kMaxEvents);

    uint32_t drawsSinceLastStateChange = 0;
    bool depthTestOn = false;
    uint32_t currentRunDepthOn = 0;
    uint32_t maxRunDepthOn = 0;
    int32_t lastDepthClearDrawIndex = -1;
    int32_t firstDrawAfterLastDepthClear = -1;
    int32_t uiStartCandidate = -1;
    uint32_t totalDrawCalls = 0;

    // Helper to flush accumulated draw runs
    int collapsedDraws = 0;
    GLenum collapsedMode = 0;
    GLsizei collapsedCount = 0;
    GlEventType collapsedType = GlEventType::Clear;

    auto flushCollapsedDraws = [&]() {
        if (collapsedDraws > 0) {
            LodPocMod::logAll(LogLevel::Info,
                              "[GLTrace]   -> DRAW x {} (type={}, mode=0x{:x} [{}], count={})",
                              collapsedDraws,
                              collapsedType == GlEventType::DrawElements ? "DrawElements" : "DrawArrays",
                              collapsedMode, modeToString(collapsedMode), collapsedCount);
            collapsedDraws = 0;
        }
    };

    for (size_t i = 0; i < totalEvents; ++i) {
        const auto &ev = mEvents[i];

        if (ev.type == GlEventType::DrawElements || ev.type == GlEventType::DrawArrays ||
            ev.type == GlEventType::DrawElementsInstanced || ev.type == GlEventType::DrawArraysInstanced) {

            totalDrawCalls++;
            drawsSinceLastStateChange++;

            if (depthTestOn) {
                currentRunDepthOn++;
                if (currentRunDepthOn > maxRunDepthOn) {
                    maxRunDepthOn = currentRunDepthOn;
                }
            }

            if (lastDepthClearDrawIndex >= 0 && firstDrawAfterLastDepthClear < 0) {
                firstDrawAfterLastDepthClear = static_cast<int32_t>(ev.drawCallIndex);
            }

            // Check if identical to current collapsed run
            GLenum mode = (ev.type == GlEventType::DrawElements) ? ev.drawElements.mode : ev.drawArrays.mode;
            GLsizei countVal = (ev.type == GlEventType::DrawElements) ? ev.drawElements.count : ev.drawArrays.count;

            if (collapsedDraws > 0 && collapsedType == ev.type && collapsedMode == mode && collapsedCount == countVal) {
                collapsedDraws++;
            } else {
                flushCollapsedDraws();
                collapsedType = ev.type;
                collapsedMode = mode;
                collapsedCount = countVal;
                collapsedDraws = 1;
            }
            continue;
        }

        // State change: flush any pending draws first
        flushCollapsedDraws();
        if (drawsSinceLastStateChange > 0) {
            LodPocMod::logAll(LogLevel::Info, "[GLTrace]   [{} draw calls in this state]", drawsSinceLastStateChange);
            drawsSinceLastStateChange = 0;
        }

        switch (ev.type) {
            case GlEventType::Clear: {
                LodPocMod::logAll(LogLevel::Info,
                                  "[GLTrace] glClear(mask=0x{:08x} [{}])",
                                  ev.clear.mask, formatClearMask(ev.clear.mask));
                if (ev.clear.mask & GL_DEPTH_BUFFER_BIT) {
                    lastDepthClearDrawIndex = static_cast<int32_t>(ev.drawCallIndex);
                    firstDrawAfterLastDepthClear = -1;
                }
                break;
            }
            case GlEventType::Viewport: {
                LodPocMod::logAll(LogLevel::Info,
                                  "[GLTrace] glViewport(x={}, y={}, w={}, h={})",
                                  ev.viewport.x, ev.viewport.y, ev.viewport.w, ev.viewport.h);
                break;
            }
            case GlEventType::Scissor: {
                LodPocMod::logAll(LogLevel::Info,
                                  "[GLTrace] glScissor(x={}, y={}, w={}, h={})",
                                  ev.scissor.x, ev.scissor.y, ev.scissor.w, ev.scissor.h);
                break;
            }
            case GlEventType::Enable: {
                const char *name = capToString(ev.cap.cap);
                if (name != nullptr) {
                    LodPocMod::logAll(LogLevel::Info, "[GLTrace] glEnable(0x{:04x} [{}])", ev.cap.cap, name);
                } else {
                    LodPocMod::logAll(LogLevel::Info, "[GLTrace] glEnable(0x{:04x})", ev.cap.cap);
                }
                if (ev.cap.cap == 0x0B71 /* GL_DEPTH_TEST */) {
                    depthTestOn = true;
                }
                break;
            }
            case GlEventType::Disable: {
                const char *name = capToString(ev.cap.cap);
                if (name != nullptr) {
                    LodPocMod::logAll(LogLevel::Info, "[GLTrace] glDisable(0x{:04x} [{}])", ev.cap.cap, name);
                } else {
                    LodPocMod::logAll(LogLevel::Info, "[GLTrace] glDisable(0x{:04x})", ev.cap.cap);
                }
                if (ev.cap.cap == 0x0B71 /* GL_DEPTH_TEST */) {
                    depthTestOn = false;
                    if (maxRunDepthOn > 40 && uiStartCandidate < 0) {
                        uiStartCandidate = static_cast<int32_t>(ev.drawCallIndex);
                    }
                    currentRunDepthOn = 0;
                }
                break;
            }
            case GlEventType::DepthMask: {
                LodPocMod::logAll(LogLevel::Info, "[GLTrace] glDepthMask(flag={})", ev.depthMask.flag ? "GL_TRUE" : "GL_FALSE");
                break;
            }
            case GlEventType::DepthFunc: {
                LodPocMod::logAll(LogLevel::Info, "[GLTrace] glDepthFunc(func=0x{:04x})", ev.depthFunc.func);
                break;
            }
            case GlEventType::BlendFunc: {
                LodPocMod::logAll(LogLevel::Info, "[GLTrace] glBlendFunc(sfactor=0x{:04x}, dfactor=0x{:04x})",
                                  ev.blendFunc.sfactor, ev.blendFunc.dfactor);
                break;
            }
            case GlEventType::BlendFuncSeparate: {
                LodPocMod::logAll(LogLevel::Info,
                                  "[GLTrace] glBlendFuncSeparate(srcRGB=0x{:04x}, dstRGB=0x{:04x}, srcA=0x{:04x}, dstA=0x{:04x})",
                                  ev.blendFuncSep.srgb, ev.blendFuncSep.drgb, ev.blendFuncSep.sa, ev.blendFuncSep.da);
                break;
            }
            case GlEventType::CullFace: {
                LodPocMod::logAll(LogLevel::Info, "[GLTrace] glCullFace(mode=0x{:04x})", ev.cullFace.mode);
                break;
            }
            case GlEventType::UseProgram: {
                LodPocMod::logAll(LogLevel::Info, "[GLTrace] glUseProgram(program={})", ev.useProgram.program);
                break;
            }
            case GlEventType::BindFbo: {
                LodPocMod::logAll(LogLevel::Info, "[GLTrace] glBindFramebuffer(target=0x{:04x}, fbo={})",
                                  ev.bindFbo.target, ev.bindFbo.fbo);
                break;
            }
            case GlEventType::FboTexture2D: {
                LodPocMod::logAll(LogLevel::Info,
                                  "[GLTrace] glFramebufferTexture2D(target=0x{:04x}, attachment=0x{:04x}, textarget=0x{:04x}, texture={}, level={})",
                                  ev.fboTex2D.target, ev.fboTex2D.attachment, ev.fboTex2D.textarget, ev.fboTex2D.texture, ev.fboTex2D.level);
                break;
            }
            case GlEventType::InvalidateFbo: {
                LodPocMod::logAll(LogLevel::Info, "[GLTrace] glInvalidateFramebuffer(target=0x{:04x}, num={})",
                                  ev.invalidateFbo.target, ev.invalidateFbo.num);
                break;
            }
            case GlEventType::BlitFbo: {
                LodPocMod::logAll(LogLevel::Info, "[GLTrace] glBlitFramebuffer(mask=0x{:08x}, filter=0x{:04x})",
                                  ev.blitFbo.mask, ev.blitFbo.filter);
                break;
            }
            default:
                break;
        }
    }

    flushCollapsedDraws();

    LodPocMod::logAll(LogLevel::Info,
                      "[Probe A: GLTrace] ===== FRAME TRACE SUMMARY ({}) =====",
                      label);
    LodPocMod::logAll(LogLevel::Info, "[Probe A: GLTrace] Total draw calls: {}", totalDrawCalls);
    LodPocMod::logAll(LogLevel::Info, "[Probe A: GLTrace] Largest run of draws with depth test ON: {}", maxRunDepthOn);
    LodPocMod::logAll(LogLevel::Info, "[Probe A: GLTrace] Index of first draw after last depth clear: {}", firstDrawAfterLastDepthClear);
    LodPocMod::logAll(LogLevel::Info, "[Probe A: GLTrace] Index where depth test is first disabled after big run (UI start candidate): {}", uiStartCandidate);
    LodPocMod::logAll(LogLevel::Info, "[Probe A: GLTrace] ===================================================");
}

// -----------------------------------------------------------------------------
// Hook Detour Implementations
// -----------------------------------------------------------------------------

#define DISPATCH_HOOK(NAME, ...)                                                               \
    auto &inst = GlTrace::instance();                                                          \
    if (inst.mOrig##NAME != nullptr) {                                                         \
        inst.mOrig##NAME(__VA_ARGS__);                                                         \
    }                                                                                          \
    if (gettid() != inst.getRenderThreadId()) return;                                         \
    if (inst.mState.load(std::memory_order_relaxed) != GlTrace::TraceState::Recording) return;

void GlTrace::detourClear(GLbitfield mask) {
    DISPATCH_HOOK(Clear, mask)
    inst.recordClear(mask);
}

void GlTrace::detourViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    DISPATCH_HOOK(Viewport, x, y, width, height)
    inst.recordViewport(x, y, width, height);
}

void GlTrace::detourScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    DISPATCH_HOOK(Scissor, x, y, width, height)
    inst.recordScissor(x, y, width, height);
}

void GlTrace::detourEnable(GLenum cap) {
    DISPATCH_HOOK(Enable, cap)
    inst.recordEnable(cap);
}

void GlTrace::detourDisable(GLenum cap) {
    DISPATCH_HOOK(Disable, cap)
    inst.recordDisable(cap);
}

void GlTrace::detourDepthMask(GLboolean flag) {
    DISPATCH_HOOK(DepthMask, flag)
    inst.recordDepthMask(flag);
}

void GlTrace::detourDepthFunc(GLenum func) {
    DISPATCH_HOOK(DepthFunc, func)
    inst.recordDepthFunc(func);
}

void GlTrace::detourBlendFunc(GLenum sfactor, GLenum dfactor) {
    DISPATCH_HOOK(BlendFunc, sfactor, dfactor)
    inst.recordBlendFunc(sfactor, dfactor);
}

void GlTrace::detourBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha) {
    DISPATCH_HOOK(BlendFuncSeparate, srcRGB, dstRGB, srcAlpha, dstAlpha)
    inst.recordBlendFuncSeparate(srcRGB, dstRGB, srcAlpha, dstAlpha);
}

void GlTrace::detourCullFace(GLenum mode) {
    DISPATCH_HOOK(CullFace, mode)
    inst.recordCullFace(mode);
}

void GlTrace::detourUseProgram(GLuint program) {
    DISPATCH_HOOK(UseProgram, program)
    inst.recordUseProgram(program);
}

void GlTrace::detourBindFramebuffer(GLenum target, GLuint framebuffer) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigBindFramebuffer != nullptr) {
        inst.mOrigBindFramebuffer(target, framebuffer);
    }
    LodPocMod::instance().onBindFramebuffer(target, framebuffer);
    if (gettid() != inst.getRenderThreadId()) return;
    if (inst.mState.load(std::memory_order_relaxed) != GlTrace::TraceState::Recording) return;
    inst.recordBindFbo(target, framebuffer);
}

void GlTrace::detourFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    DISPATCH_HOOK(FramebufferTexture2D, target, attachment, textarget, texture, level)
    inst.recordFboTexture2D(target, attachment, textarget, texture, level);
}

void GlTrace::detourInvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum *attachments) {
    DISPATCH_HOOK(InvalidateFramebuffer, target, numAttachments, attachments)
    inst.recordInvalidateFbo(target, numAttachments, attachments);
}

void GlTrace::detourBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                    GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                    GLbitfield mask, GLenum filter) {
    DISPATCH_HOOK(BlitFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter)
    inst.recordBlitFbo(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

void GlTrace::detourDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawElements != nullptr) {
        inst.mOrigDrawElements(mode, count, type, indices);
    }
    inst.mDrawElementsCount.fetch_add(1, std::memory_order_relaxed);
    inst.mFrameDrawElements.fetch_add(1, std::memory_order_relaxed);
    if (gettid() != inst.getRenderThreadId()) return;
    if (inst.mState.load(std::memory_order_relaxed) != GlTrace::TraceState::Recording) return;
    inst.recordDrawElements(mode, count, type);
}

void GlTrace::detourDrawArrays(GLenum mode, GLint first, GLsizei count) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawArrays != nullptr) {
        inst.mOrigDrawArrays(mode, first, count);
    }
    inst.mDrawArraysCount.fetch_add(1, std::memory_order_relaxed);
    if (gettid() != inst.getRenderThreadId()) return;
    if (inst.mState.load(std::memory_order_relaxed) != GlTrace::TraceState::Recording) return;
    inst.recordDrawArrays(mode, first, count);
}

void GlTrace::detourDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawElementsInstanced != nullptr) {
        inst.mOrigDrawElementsInstanced(mode, count, type, indices, instancecount);
    }
    inst.mDrawElementsCount.fetch_add(1, std::memory_order_relaxed);
    inst.mFrameDrawElements.fetch_add(1, std::memory_order_relaxed);
    if (gettid() != inst.getRenderThreadId()) return;
    if (inst.mState.load(std::memory_order_relaxed) != GlTrace::TraceState::Recording) return;
    inst.recordDrawElementsInstanced(mode, count, instancecount);
}

void GlTrace::detourDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount) {
    auto &inst = GlTrace::instance();
    if (inst.mOrigDrawArraysInstanced != nullptr) {
        inst.mOrigDrawArraysInstanced(mode, first, count, instancecount);
    }
    inst.mDrawArraysCount.fetch_add(1, std::memory_order_relaxed);
    if (gettid() != inst.getRenderThreadId()) return;
    if (inst.mState.load(std::memory_order_relaxed) != GlTrace::TraceState::Recording) return;
    inst.recordDrawArraysInstanced(mode, first, count, instancecount);
}

// -----------------------------------------------------------------------------
// Event recording methods
// -----------------------------------------------------------------------------

void GlTrace::recordClear(GLbitfield mask) {
    GlEvent ev;
    ev.type = GlEventType::Clear;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.clear.mask = mask;
    recordEvent(ev);
}

void GlTrace::recordViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    GlEvent ev;
    ev.type = GlEventType::Viewport;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.viewport = {x, y, w, h};
    recordEvent(ev);
}

void GlTrace::recordScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    GlEvent ev;
    ev.type = GlEventType::Scissor;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.scissor = {x, y, w, h};
    recordEvent(ev);
}

void GlTrace::recordEnable(GLenum cap) {
    GlEvent ev;
    ev.type = GlEventType::Enable;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.cap.cap = cap;
    recordEvent(ev);
}

void GlTrace::recordDisable(GLenum cap) {
    GlEvent ev;
    ev.type = GlEventType::Disable;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.cap.cap = cap;
    recordEvent(ev);
}

void GlTrace::recordDepthMask(GLboolean flag) {
    GlEvent ev;
    ev.type = GlEventType::DepthMask;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.depthMask.flag = flag;
    recordEvent(ev);
}

void GlTrace::recordDepthFunc(GLenum func) {
    GlEvent ev;
    ev.type = GlEventType::DepthFunc;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.depthFunc.func = func;
    recordEvent(ev);
}

void GlTrace::recordBlendFunc(GLenum sfactor, GLenum dfactor) {
    GlEvent ev;
    ev.type = GlEventType::BlendFunc;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.blendFunc = {sfactor, dfactor};
    recordEvent(ev);
}

void GlTrace::recordBlendFuncSeparate(GLenum srgb, GLenum drgb, GLenum sa, GLenum da) {
    GlEvent ev;
    ev.type = GlEventType::BlendFuncSeparate;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.blendFuncSep = {srgb, drgb, sa, da};
    recordEvent(ev);
}

void GlTrace::recordCullFace(GLenum mode) {
    GlEvent ev;
    ev.type = GlEventType::CullFace;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.cullFace.mode = mode;
    recordEvent(ev);
}

void GlTrace::recordUseProgram(GLuint program) {
    GlEvent ev;
    ev.type = GlEventType::UseProgram;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.useProgram.program = program;
    recordEvent(ev);
}

void GlTrace::recordBindFbo(GLenum target, GLuint fbo) {
    GlEvent ev;
    ev.type = GlEventType::BindFbo;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.bindFbo = {target, fbo};
    recordEvent(ev);
}

void GlTrace::recordFboTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    GlEvent ev;
    ev.type = GlEventType::FboTexture2D;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.fboTex2D = {target, attachment, textarget, texture, level};
    recordEvent(ev);
}

void GlTrace::recordInvalidateFbo(GLenum target, GLsizei numAttachments, const GLenum *attachments) {
    (void)attachments;
    GlEvent ev;
    ev.type = GlEventType::InvalidateFbo;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.invalidateFbo = {target, numAttachments};
    recordEvent(ev);
}

void GlTrace::recordBlitFbo(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                            GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                            GLbitfield mask, GLenum filter) {
    (void)srcX0; (void)srcY0; (void)srcX1; (void)srcY1;
    (void)dstX0; (void)dstY0; (void)dstX1; (void)dstY1;
    GlEvent ev;
    ev.type = GlEventType::BlitFbo;
    ev.drawCallIndex = mRunningDrawIndex.load(std::memory_order_relaxed);
    ev.blitFbo = {mask, filter};
    recordEvent(ev);
}

void GlTrace::recordDrawElements(GLenum mode, GLsizei count, GLenum type) {
    uint32_t drawIdx = mRunningDrawIndex.fetch_add(1, std::memory_order_relaxed);
    GlEvent ev;
    ev.type = GlEventType::DrawElements;
    ev.drawCallIndex = drawIdx;
    ev.drawElements = {mode, count, type};
    recordEvent(ev);
}

void GlTrace::recordDrawArrays(GLenum mode, GLint first, GLsizei count) {
    uint32_t drawIdx = mRunningDrawIndex.fetch_add(1, std::memory_order_relaxed);
    GlEvent ev;
    ev.type = GlEventType::DrawArrays;
    ev.drawCallIndex = drawIdx;
    ev.drawArrays = {mode, first, count};
    recordEvent(ev);
}

void GlTrace::recordDrawElementsInstanced(GLenum mode, GLsizei count, GLsizei instanceCount) {
    uint32_t drawIdx = mRunningDrawIndex.fetch_add(1, std::memory_order_relaxed);
    GlEvent ev;
    ev.type = GlEventType::DrawElementsInstanced;
    ev.drawCallIndex = drawIdx;
    ev.drawInstanced = {mode, count, instanceCount};
    recordEvent(ev);
}

void GlTrace::recordDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount) {
    (void)first;
    uint32_t drawIdx = mRunningDrawIndex.fetch_add(1, std::memory_order_relaxed);
    GlEvent ev;
    ev.type = GlEventType::DrawArraysInstanced;
    ev.drawCallIndex = drawIdx;
    ev.drawInstanced = {mode, count, instanceCount};
    recordEvent(ev);
}

// -----------------------------------------------------------------------------
// Hook Installation / Removal
// -----------------------------------------------------------------------------

void GlTrace::install() {
    remove();

    void *glesLib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!glesLib) {
        glesLib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_GLOBAL);
    }

    auto hookFn = [this, glesLib](const char *name, void *detour, void **orig) {
        void *target = nullptr;
        if (glesLib != nullptr) {
            target = dlsym(glesLib, name);
        }
        if (target == nullptr) {
            target = dlsym(RTLD_DEFAULT, name);
        }
        if (target == nullptr) {
            LodPocMod::logAll(LogLevel::Warn, "[Probe A: GLTrace] {} NOT FOUND", name);
            return;
        }

        pl::memory::HookHandle handle(target, detour, orig);
        if (!handle.installed()) {
            LodPocMod::logAll(LogLevel::Error, "[Probe A: GLTrace] {} FAILED at {:p}", name, target);
        } else {
            mHooks.push_back(std::move(handle));
        }
    };

    hookFn("glClear", reinterpret_cast<void *>(&detourClear), reinterpret_cast<void **>(&mOrigClear));
    hookFn("glViewport", reinterpret_cast<void *>(&detourViewport), reinterpret_cast<void **>(&mOrigViewport));
    hookFn("glScissor", reinterpret_cast<void *>(&detourScissor), reinterpret_cast<void **>(&mOrigScissor));
    hookFn("glEnable", reinterpret_cast<void *>(&detourEnable), reinterpret_cast<void **>(&mOrigEnable));
    hookFn("glDisable", reinterpret_cast<void *>(&detourDisable), reinterpret_cast<void **>(&mOrigDisable));
    hookFn("glDepthMask", reinterpret_cast<void *>(&detourDepthMask), reinterpret_cast<void **>(&mOrigDepthMask));
    hookFn("glDepthFunc", reinterpret_cast<void *>(&detourDepthFunc), reinterpret_cast<void **>(&mOrigDepthFunc));
    hookFn("glBlendFunc", reinterpret_cast<void *>(&detourBlendFunc), reinterpret_cast<void **>(&mOrigBlendFunc));
    hookFn("glBlendFuncSeparate", reinterpret_cast<void *>(&detourBlendFuncSeparate), reinterpret_cast<void **>(&mOrigBlendFuncSeparate));
    hookFn("glCullFace", reinterpret_cast<void *>(&detourCullFace), reinterpret_cast<void **>(&mOrigCullFace));
    hookFn("glUseProgram", reinterpret_cast<void *>(&detourUseProgram), reinterpret_cast<void **>(&mOrigUseProgram));
    hookFn("glBindFramebuffer", reinterpret_cast<void *>(&detourBindFramebuffer), reinterpret_cast<void **>(&mOrigBindFramebuffer));
    hookFn("glFramebufferTexture2D", reinterpret_cast<void *>(&detourFramebufferTexture2D), reinterpret_cast<void **>(&mOrigFramebufferTexture2D));
    hookFn("glInvalidateFramebuffer", reinterpret_cast<void *>(&detourInvalidateFramebuffer), reinterpret_cast<void **>(&mOrigInvalidateFramebuffer));
    hookFn("glBlitFramebuffer", reinterpret_cast<void *>(&detourBlitFramebuffer), reinterpret_cast<void **>(&mOrigBlitFramebuffer));
    hookFn("glDrawElements", reinterpret_cast<void *>(&detourDrawElements), reinterpret_cast<void **>(&mOrigDrawElements));
    hookFn("glDrawArrays", reinterpret_cast<void *>(&detourDrawArrays), reinterpret_cast<void **>(&mOrigDrawArrays));
    hookFn("glDrawElementsInstanced", reinterpret_cast<void *>(&detourDrawElementsInstanced), reinterpret_cast<void **>(&mOrigDrawElementsInstanced));
    hookFn("glDrawArraysInstanced", reinterpret_cast<void *>(&detourDrawArraysInstanced), reinterpret_cast<void **>(&mOrigDrawArraysInstanced));

    LodPocMod::logAll(LogLevel::Info, "[Probe A: GLTrace] Installed {} GL hooks successfully", mHooks.size());
}

void GlTrace::remove() {
    mHooks.clear();

    mOrigClear = nullptr;
    mOrigViewport = nullptr;
    mOrigScissor = nullptr;
    mOrigEnable = nullptr;
    mOrigDisable = nullptr;
    mOrigDepthMask = nullptr;
    mOrigDepthFunc = nullptr;
    mOrigBlendFunc = nullptr;
    mOrigBlendFuncSeparate = nullptr;
    mOrigCullFace = nullptr;
    mOrigUseProgram = nullptr;
    mOrigBindFramebuffer = nullptr;
    mOrigFramebufferTexture2D = nullptr;
    mOrigInvalidateFramebuffer = nullptr;
    mOrigBlitFramebuffer = nullptr;
    mOrigDrawElements = nullptr;
    mOrigDrawArrays = nullptr;
    mOrigDrawElementsInstanced = nullptr;
    mOrigDrawArraysInstanced = nullptr;
}

} // namespace lod_poc
