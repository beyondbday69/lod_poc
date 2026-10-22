#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>

#include <pl/memory/Hook.hpp>

namespace lod_poc {

struct ImageDesc {
    GLuint id{0};
    GLsizei width{0};
    GLsizei height{0};
    GLenum internalFormat{0};
    bool isTexture{true};
};

struct FboDesc {
    GLuint id{0};
    std::unordered_map<GLenum, ImageDesc> colors;
    ImageDesc depth;
    ImageDesc stencil;
};

struct RawDrawRecord {
    uint32_t callIndex{0};
    std::string functionName;
    GLenum mode{0};
    GLint first{0};
    GLsizei count{0};
    GLenum type{0};
    uintptr_t indicesOrOffset{0};
    GLsizei instanceCount{1};
    GLuint start{0};
    GLuint end{0};
    GLint baseVertex{0};
    GLsizei primCount{0};
};

struct PassSegment {
    uint32_t segmentIndex{0};
    GLuint fboId{0};
    GLint vpX{0}, vpY{0};
    GLsizei vpW{0}, vpH{0};
    std::string attachmentSummary;

    uint32_t totalDrawCalls{0};
    uint64_t sumVertexCount{0};
    uint64_t sumInstanceCount{0};
    std::map<std::string, uint32_t> drawsByFunction;

    bool depthTestEnabled{false};
    bool depthMaskEnabled{true};
    bool blendEnabled{false};
    std::set<GLuint> distinctPrograms;

    std::vector<RawDrawRecord> rawDraws;
};

class GlTrace {
public:
    static GlTrace &instance();

    void install();
    void remove();

    void installHeavyHooks();
    void removeHeavyHooks();

    void setConfig(bool traceFrame, bool traceRaw, const std::string &traceLabel);
    void armTrace(const std::string &label);
    void onSwapBuffers(uint64_t frameCount, double currentFps);

    void setRenderThreadId(pid_t tid) { mRenderThreadId.store(tid, std::memory_order_relaxed); }
    pid_t getRenderThreadId() const { return mRenderThreadId.load(std::memory_order_relaxed); }

    // FBO and Texture tracking
    void onBindFramebuffer(GLenum target, GLuint framebuffer);
    void onBindTexture(GLenum target, GLuint texture);
    void onBindRenderbuffer(GLenum target, GLuint renderbuffer);
    void onTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height);
    void onTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    void onRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
    void onFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void onFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
    void onFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
    void onDrawBuffers(GLsizei n, const GLenum *bufs);

    // State Tracking
    void onViewport(GLint x, GLint y, GLsizei width, GLsizei height);
    void onEnable(GLenum cap);
    void onDisable(GLenum cap);
    void onDepthMask(GLboolean flag);
    void onUseProgram(GLuint program);

    // Draw Tracking
    void onDraw(const RawDrawRecord &record);

    std::atomic<uint64_t> mFrameDrawElements{0};

    // Original GL function pointers
    using GlBindFramebufferFn = void (*)(GLenum target, GLuint framebuffer);
    using GlBindTextureFn = void (*)(GLenum target, GLuint texture);
    using GlBindRenderbufferFn = void (*)(GLenum target, GLuint renderbuffer);
    using GlTexImage2DFn = void (*)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
    using GlTexStorage2DFn = void (*)(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    using GlRenderbufferStorageFn = void (*)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
    using GlFramebufferTexture2DFn = void (*)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    using GlFramebufferTextureLayerFn = void (*)(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
    using GlFramebufferRenderbufferFn = void (*)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
    using GlDrawBuffersFn = void (*)(GLsizei n, const GLenum *bufs);

    using GlViewportFn = void (*)(GLint x, GLint y, GLsizei width, GLsizei height);
    using GlEnableFn = void (*)(GLenum cap);
    using GlDisableFn = void (*)(GLenum cap);
    using GlDepthMaskFn = void (*)(GLboolean flag);
    using GlUseProgramFn = void (*)(GLuint program);

    using GlDrawArraysFn = void (*)(GLenum mode, GLint first, GLsizei count);
    using GlDrawArraysInstancedFn = void (*)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
    using GlDrawElementsFn = void (*)(GLenum mode, GLsizei count, GLenum type, const void *indices);
    using GlDrawElementsInstancedFn = void (*)(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount);
    using GlDrawRangeElementsFn = void (*)(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices);
    using GlDrawElementsBaseVertexFn = void (*)(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex);
    using GlDrawElementsInstancedBaseVertexFn = void (*)(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount, GLint basevertex);
    using GlDrawRangeElementsBaseVertexFn = void (*)(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint basevertex);
    using GlDrawArraysIndirectFn = void (*)(GLenum mode, const void *indirect);
    using GlDrawElementsIndirectFn = void (*)(GLenum mode, GLenum type, const void *indirect);
    using GlMultiDrawArraysEXTFn = void (*)(GLenum mode, const GLint *first, const GLsizei *count, GLsizei primcount);
    using GlMultiDrawElementsEXTFn = void (*)(GLenum mode, const GLsizei *count, GLenum type, const void *const *indices, GLsizei primcount);

    GlBindFramebufferFn mOrigBindFramebuffer{nullptr};
    GlBindTextureFn mOrigBindTexture{nullptr};
    GlBindRenderbufferFn mOrigBindRenderbuffer{nullptr};
    GlTexImage2DFn mOrigTexImage2D{nullptr};
    GlTexStorage2DFn mOrigTexStorage2D{nullptr};
    GlRenderbufferStorageFn mOrigRenderbufferStorage{nullptr};
    GlFramebufferTexture2DFn mOrigFramebufferTexture2D{nullptr};
    GlFramebufferTextureLayerFn mOrigFramebufferTextureLayer{nullptr};
    GlFramebufferRenderbufferFn mOrigFramebufferRenderbuffer{nullptr};
    GlDrawBuffersFn mOrigDrawBuffers{nullptr};

    GlViewportFn mOrigViewport{nullptr};
    GlEnableFn mOrigEnable{nullptr};
    GlDisableFn mOrigDisable{nullptr};
    GlDepthMaskFn mOrigDepthMask{nullptr};
    GlUseProgramFn mOrigUseProgram{nullptr};

    GlDrawArraysFn mOrigDrawArrays{nullptr};
    GlDrawArraysInstancedFn mOrigDrawArraysInstanced{nullptr};
    GlDrawElementsFn mOrigDrawElements{nullptr};
    GlDrawElementsInstancedFn mOrigDrawElementsInstanced{nullptr};
    GlDrawRangeElementsFn mOrigDrawRangeElements{nullptr};
    GlDrawElementsBaseVertexFn mOrigDrawElementsBaseVertex{nullptr};
    GlDrawElementsInstancedBaseVertexFn mOrigDrawElementsInstancedBaseVertex{nullptr};
    GlDrawRangeElementsBaseVertexFn mOrigDrawRangeElementsBaseVertex{nullptr};
    GlDrawArraysIndirectFn mOrigDrawArraysIndirect{nullptr};
    GlDrawElementsIndirectFn mOrigDrawElementsIndirect{nullptr};
    GlMultiDrawArraysEXTFn mOrigMultiDrawArraysEXT{nullptr};
    GlMultiDrawElementsEXTFn mOrigMultiDrawElementsEXT{nullptr};

private:
    GlTrace();

    enum class TraceState {
        Idle,
        Armed,
        Recording
    };

    std::atomic<pid_t> mRenderThreadId{0};
    std::atomic<TraceState> mState{TraceState::Idle};
    std::string mArmedLabel;
    std::string mActiveLabel;
    std::mutex mStateMutex;

    bool mConfigTraceFrame{true};
    bool mConfigTraceRaw{false};
    std::string mConfigTraceLabel;
    std::string mLastTracedLabel;

    bool mMainMenuTraced{false};
    bool mInWorldTraced{false};
    bool mHeavyHooksInstalled{false};
    std::chrono::steady_clock::time_point mFirstHeavyFrameTime{};

    double mLastRecordedFps{0.0};

    // Tracking state
    GLuint mCurrentBoundTexture2D{0};
    GLuint mCurrentBoundRenderbuffer{0};
    GLuint mCurrentBoundDrawFbo{0};
    GLint mCurrentVpX{0}, mCurrentVpY{0};
    GLsizei mCurrentVpW{0}, mCurrentVpH{0};
    bool mCurrentDepthTest{false};
    bool mCurrentDepthMask{true};
    bool mCurrentBlend{false};
    GLuint mCurrentProgram{0};

    std::unordered_map<GLuint, ImageDesc> mTextureMap;
    std::unordered_map<GLuint, ImageDesc> mRenderbufferMap;
    std::unordered_map<GLuint, FboDesc> mFboMap;

    std::vector<PassSegment> mCurrentFrameSegments;
    std::mutex mFrameSegmentsMutex;

    std::vector<pl::memory::HookHandle> mPermanentHooks;
    std::vector<pl::memory::HookHandle> mHeavyHooks;

    void startNewSegment(GLuint fboId);
    void dumpPassSummary(const std::string &label);
    std::string buildAttachmentDesc(GLuint fboId);

    // Static detours
    static void detourBindFramebuffer(GLenum target, GLuint framebuffer);
    static void detourBindTexture(GLenum target, GLuint texture);
    static void detourBindRenderbuffer(GLenum target, GLuint renderbuffer);
    static void detourTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
    static void detourTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    static void detourRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
    static void detourFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    static void detourFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer);
    static void detourFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
    static void detourDrawBuffers(GLsizei n, const GLenum *bufs);

    static void detourViewport(GLint x, GLint y, GLsizei width, GLsizei height);
    static void detourEnable(GLenum cap);
    static void detourDisable(GLenum cap);
    static void detourDepthMask(GLboolean flag);
    static void detourUseProgram(GLuint program);

    static void detourDrawArrays(GLenum mode, GLint first, GLsizei count);
    static void detourDrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
    static void detourDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);
    static void detourDrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount);
    static void detourDrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices);
    static void detourDrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex);
    static void detourDrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount, GLint basevertex);
    static void detourDrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void *indices, GLint basevertex);
    static void detourDrawArraysIndirect(GLenum mode, const void *indirect);
    static void detourDrawElementsIndirect(GLenum mode, GLenum type, const void *indirect);
    static void detourMultiDrawArraysEXT(GLenum mode, const GLint *first, const GLsizei *count, GLsizei primcount);
    static void detourMultiDrawElementsEXT(GLenum mode, const GLsizei *count, GLenum type, const void *const *indices, GLsizei primcount);
};

} // namespace lod_poc
