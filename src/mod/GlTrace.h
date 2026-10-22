#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include <pl/memory/Hook.hpp>

namespace lod_poc {

enum class GlEventType : uint8_t {
    Clear,
    Viewport,
    Scissor,
    Enable,
    Disable,
    DepthMask,
    DepthFunc,
    BlendFunc,
    BlendFuncSeparate,
    CullFace,
    UseProgram,
    BindFbo,
    FboTexture2D,
    InvalidateFbo,
    BlitFbo,
    DrawElements,
    DrawArrays,
    DrawElementsInstanced,
    DrawArraysInstanced
};

struct GlEvent {
    GlEventType type;
    uint32_t drawCallIndex;
    union {
        struct { GLbitfield mask; } clear;
        struct { GLint x, y; GLsizei w, h; } viewport;
        struct { GLint x, y; GLsizei w, h; } scissor;
        struct { GLenum cap; } cap;
        struct { GLboolean flag; } depthMask;
        struct { GLenum func; } depthFunc;
        struct { GLenum sfactor, dfactor; } blendFunc;
        struct { GLenum srgb, drgb, sa, da; } blendFuncSep;
        struct { GLenum mode; } cullFace;
        struct { GLuint program; } useProgram;
        struct { GLenum target; GLuint fbo; } bindFbo;
        struct { GLenum target, attachment, textarget; GLuint texture; GLint level; } fboTex2D;
        struct { GLenum target; GLsizei num; } invalidateFbo;
        struct { GLbitfield mask; GLenum filter; } blitFbo;
        struct { GLenum mode; GLsizei count; GLenum type; } drawElements;
        struct { GLenum mode; GLint first; GLsizei count; } drawArrays;
        struct { GLenum mode; GLsizei count; GLsizei instanceCount; } drawInstanced;
    };
};

class GlTrace {
public:
    static GlTrace &instance();

    void install();
    void remove();

    void setConfig(bool traceFrame, const std::string &traceLabel);
    void armTrace(const std::string &label);
    void onSwapBuffers(uint64_t frameCount);

    void setRenderThreadId(pid_t tid) { mRenderThreadId.store(tid, std::memory_order_relaxed); }
    pid_t getRenderThreadId() const { return mRenderThreadId.load(std::memory_order_relaxed); }

    // Event recording helpers called by detours
    void recordClear(GLbitfield mask);
    void recordViewport(GLint x, GLint y, GLsizei w, GLsizei h);
    void recordScissor(GLint x, GLint y, GLsizei w, GLsizei h);
    void recordEnable(GLenum cap);
    void recordDisable(GLenum cap);
    void recordDepthMask(GLboolean flag);
    void recordDepthFunc(GLenum func);
    void recordBlendFunc(GLenum sfactor, GLenum dfactor);
    void recordBlendFuncSeparate(GLenum srgb, GLenum drgb, GLenum sa, GLenum da);
    void recordCullFace(GLenum mode);
    void recordUseProgram(GLuint program);
    void recordBindFbo(GLenum target, GLuint fbo);
    void recordFboTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void recordInvalidateFbo(GLenum target, GLsizei numAttachments, const GLenum *attachments);
    void recordBlitFbo(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                       GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                       GLbitfield mask, GLenum filter);
    void recordDrawElements(GLenum mode, GLsizei count, GLenum type);
    void recordDrawArrays(GLenum mode, GLint first, GLsizei count);
    void recordDrawElementsInstanced(GLenum mode, GLsizei count, GLsizei instanceCount);
    void recordDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instanceCount);

    // Call counters
    std::atomic<uint64_t> mDrawElementsCount{0};
    std::atomic<uint64_t> mDrawArraysCount{0};
    std::atomic<uint64_t> mFrameDrawElements{0};

    // Original GL function pointers
    using GlClearFn = void (*)(GLbitfield mask);
    using GlViewportFn = void (*)(GLint x, GLint y, GLsizei width, GLsizei height);
    using GlScissorFn = void (*)(GLint x, GLint y, GLsizei width, GLsizei height);
    using GlEnableFn = void (*)(GLenum cap);
    using GlDisableFn = void (*)(GLenum cap);
    using GlDepthMaskFn = void (*)(GLboolean flag);
    using GlDepthFuncFn = void (*)(GLenum func);
    using GlBlendFuncFn = void (*)(GLenum sfactor, GLenum dfactor);
    using GlBlendFuncSeparateFn = void (*)(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
    using GlCullFaceFn = void (*)(GLenum mode);
    using GlUseProgramFn = void (*)(GLuint program);
    using GlBindFramebufferFn = void (*)(GLenum target, GLuint framebuffer);
    using GlFramebufferTexture2DFn = void (*)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    using GlInvalidateFramebufferFn = void (*)(GLenum target, GLsizei numAttachments, const GLenum *attachments);
    using GlBlitFramebufferFn = void (*)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                         GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                         GLbitfield mask, GLenum filter);
    using GlDrawElementsFn = void (*)(GLenum mode, GLsizei count, GLenum type, const void *indices);
    using GlDrawArraysFn = void (*)(GLenum mode, GLint first, GLsizei count);
    using GlDrawElementsInstancedFn = void (*)(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount);
    using GlDrawArraysInstancedFn = void (*)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);

    GlClearFn mOrigClear{nullptr};
    GlViewportFn mOrigViewport{nullptr};
    GlScissorFn mOrigScissor{nullptr};
    GlEnableFn mOrigEnable{nullptr};
    GlDisableFn mOrigDisable{nullptr};
    GlDepthMaskFn mOrigDepthMask{nullptr};
    GlDepthFuncFn mOrigDepthFunc{nullptr};
    GlBlendFuncFn mOrigBlendFunc{nullptr};
    GlBlendFuncSeparateFn mOrigBlendFuncSeparate{nullptr};
    GlCullFaceFn mOrigCullFace{nullptr};
    GlUseProgramFn mOrigUseProgram{nullptr};
    GlBindFramebufferFn mOrigBindFramebuffer{nullptr};
    GlFramebufferTexture2DFn mOrigFramebufferTexture2D{nullptr};
    GlInvalidateFramebufferFn mOrigInvalidateFramebuffer{nullptr};
    GlBlitFramebufferFn mOrigBlitFramebuffer{nullptr};
    GlDrawElementsFn mOrigDrawElements{nullptr};
    GlDrawArraysFn mOrigDrawArrays{nullptr};
    GlDrawElementsInstancedFn mOrigDrawElementsInstanced{nullptr};
    GlDrawArraysInstancedFn mOrigDrawArraysInstanced{nullptr};

private:
    GlTrace();

    enum class TraceState {
        Idle,
        Armed,
        Recording
    };

    static constexpr size_t kMaxEvents = 6000;

    std::atomic<pid_t> mRenderThreadId{0};
    std::atomic<TraceState> mState{TraceState::Idle};
    std::string mArmedLabel;
    std::string mActiveLabel;
    std::mutex mStateMutex;

    bool mConfigTraceFrame{true};
    std::string mConfigTraceLabel;
    std::string mLastTracedLabel;

    bool mMainMenuTraced{false};
    bool mInWorldTraced{false};
    std::chrono::steady_clock::time_point mFirstHeavyFrameTime{};

    std::vector<GlEvent> mEvents;
    std::atomic<size_t> mEventCount{0};
    std::atomic<uint32_t> mRunningDrawIndex{0};

    std::vector<pl::memory::HookHandle> mHooks;

    void recordEvent(const GlEvent &ev);
    void dumpCompressedSummary(const std::string &label, size_t count);

    // Static detours
    static void detourClear(GLbitfield mask);
    static void detourViewport(GLint x, GLint y, GLsizei width, GLsizei height);
    static void detourScissor(GLint x, GLint y, GLsizei width, GLsizei height);
    static void detourEnable(GLenum cap);
    static void detourDisable(GLenum cap);
    static void detourDepthMask(GLboolean flag);
    static void detourDepthFunc(GLenum func);
    static void detourBlendFunc(GLenum sfactor, GLenum dfactor);
    static void detourBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
    static void detourCullFace(GLenum mode);
    static void detourUseProgram(GLuint program);
    static void detourBindFramebuffer(GLenum target, GLuint framebuffer);
    static void detourFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    static void detourInvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum *attachments);
    static void detourBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                      GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                      GLbitfield mask, GLenum filter);
    static void detourDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
    static void detourDrawArrays(GLenum mode, GLint first, GLsizei count);
    static void detourDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount);
    static void detourDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
};

} // namespace lod_poc
