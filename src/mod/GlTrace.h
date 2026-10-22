#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

#include <pl/memory/Hook.hpp>

namespace lod_poc {

struct ClearRecord {
    uint32_t clearIndex{0};
    GLuint fboId{0};
    GLbitfield mask{0};
    float clearDepth{1.0f};
    uint32_t drawCountBefore{0};
};

struct InvalidateRecord {
    uint32_t callIndex{0};
    std::string functionName;
    GLuint fboId{0};
    std::vector<std::string> attachments;
    uint32_t drawCountBefore{0};
    bool invalidatedDepth{false};
};

struct DrawRunState {
    bool depthTest{false};
    bool depthMask{true};
    GLenum depthFunc{GL_LESS};
    float depthNear{0.0f};
    float depthFar{1.0f};
    bool blend{false};
    GLenum blendSrcRgb{GL_ONE};
    GLenum blendDstRgb{GL_ZERO};
    GLenum blendSrcAlpha{GL_ONE};
    GLenum blendDstAlpha{GL_ZERO};
    bool cullFace{false};
    GLenum cullFaceMode{GL_BACK};
    GLuint program{0};

    bool operator==(const DrawRunState &o) const {
        return depthTest == o.depthTest &&
               depthMask == o.depthMask &&
               depthFunc == o.depthFunc &&
               depthNear == o.depthNear &&
               depthFar == o.depthFar &&
               blend == o.blend &&
               blendSrcRgb == o.blendSrcRgb &&
               blendDstRgb == o.blendDstRgb &&
               blendSrcAlpha == o.blendSrcAlpha &&
               blendDstAlpha == o.blendDstAlpha &&
               cullFace == o.cullFace &&
               cullFaceMode == o.cullFaceMode &&
               program == o.program;
    }
};

struct DrawRun {
    uint32_t runIndex{0};
    DrawRunState state;
    uint32_t drawCount{0};
    uint64_t sumVertexCount{0};
    uint64_t sumInstanceCount{0};
    std::string functionSummary;
};

struct MatrixCandidate {
    GLuint program{0};
    uint32_t drawRunIndexBefore{0};
    std::string type; // "PROJECTION" or "VIEW-PROJECTION"
    float fovDeg{0.0f};
    float aspect{0.0f};
    float values[16]{};
};

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
    std::vector<DrawRun> drawRuns;
};

class GlTrace {
public:
    static GlTrace &instance();

    void install();
    void remove();

    void installHeavyHooks();
    void removeHeavyHooks();

    void setConfig(bool traceFrame, bool traceRaw, const std::string &traceLabel, int traceDelayAfterWorldSec = 10);
    void armTrace(const std::string &label);
    void onSwapBuffers(uint64_t frameCount, double currentFps);

    bool isInWorld() const { return mInWorld.load(std::memory_order_relaxed); }

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

    bool isFbo0DepthInvalidated() const { return mFbo0DepthInvalidated; }
    GLenum getCurrentDepthFunc() const { return mCurrentDepthFunc; }

    std::atomic<uint64_t> mPermanentFrameDraws{0};

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

    using GlDepthFuncFn = void (*)(GLenum func);
    using GlDepthRangefFn = void (*)(GLfloat n, GLfloat f);
    using GlBlendFuncFn = void (*)(GLenum sfactor, GLenum dfactor);
    using GlBlendFuncSeparateFn = void (*)(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
    using GlCullFaceFn = void (*)(GLenum mode);
    using GlClearDepthfFn = void (*)(GLfloat depth);
    using GlClearFn = void (*)(GLbitfield mask);

    using GlInvalidateFramebufferFn = void (*)(GLenum target, GLsizei numAttachments, const GLenum *attachments);
    using GlInvalidateSubFramebufferFn = void (*)(GLenum target, GLsizei numAttachments, const GLenum *attachments, GLint x, GLint y, GLsizei width, GLsizei height);
    using GlDiscardFramebufferEXTFn = void (*)(GLenum target, GLsizei numAttachments, const GLenum *attachments);

    using GlUniformMatrix4fvFn = void (*)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    using GlUniform4fvFn = void (*)(GLint location, GLsizei count, const GLfloat *value);
    using GlBindBufferBaseFn = void (*)(GLenum target, GLuint index, GLuint buffer);
    using GlBindBufferRangeFn = void (*)(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    using GlBufferSubDataFn = void (*)(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
    using GlBufferDataFn = void (*)(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
    using GlMapBufferRangeFn = void *(*)(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
    using GlUnmapBufferFn = GLboolean (*)(GLenum target);

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

    GlDepthFuncFn mOrigDepthFunc{nullptr};
    GlDepthRangefFn mOrigDepthRangef{nullptr};
    GlBlendFuncFn mOrigBlendFunc{nullptr};
    GlBlendFuncSeparateFn mOrigBlendFuncSeparate{nullptr};
    GlCullFaceFn mOrigCullFace{nullptr};
    GlClearDepthfFn mOrigClearDepthf{nullptr};
    GlClearFn mOrigClear{nullptr};

    GlInvalidateFramebufferFn mOrigInvalidateFramebuffer{nullptr};
    GlInvalidateSubFramebufferFn mOrigInvalidateSubFramebuffer{nullptr};
    GlDiscardFramebufferEXTFn mOrigDiscardFramebufferEXT{nullptr};

    GlUniformMatrix4fvFn mOrigUniformMatrix4fv{nullptr};
    GlUniform4fvFn mOrigUniform4fv{nullptr};
    GlBindBufferBaseFn mOrigBindBufferBase{nullptr};
    GlBindBufferRangeFn mOrigBindBufferRange{nullptr};
    GlBufferSubDataFn mOrigBufferSubData{nullptr};
    GlBufferDataFn mOrigBufferData{nullptr};
    GlMapBufferRangeFn mOrigMapBufferRange{nullptr};
    GlUnmapBufferFn mOrigUnmapBuffer{nullptr};

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

    std::atomic<bool> mInWorld{false};
    bool mInWorldTraced{false};
    bool mHeavyHooksInstalled{false};
    uint32_t mConsecutiveDrawFrames{0};
    uint32_t mConsecutiveConditionLostFrames{0};
    std::deque<bool> mWorldFrameHistory;
    std::string mPendingTraceLabel;
    std::chrono::steady_clock::time_point mWorldDetectedTime{};
    int mConfigTraceDelayAfterWorldSec{10};

    double mLastRecordedFps{0.0};

    // Tracking state
    GLuint mCurrentBoundTexture2D{0};
    GLuint mCurrentBoundRenderbuffer{0};
    GLuint mCurrentBoundDrawFbo{0};
    GLint mCurrentVpX{0}, mCurrentVpY{0};
    GLsizei mCurrentVpW{0}, mCurrentVpH{0};
    bool mCurrentDepthTest{false};
    bool mCurrentDepthMask{true};
    GLenum mCurrentDepthFunc{GL_LESS};
    float mCurrentDepthNear{0.0f};
    float mCurrentDepthFar{1.0f};
    bool mCurrentBlend{false};
    GLenum mCurrentBlendSrcRgb{GL_ONE};
    GLenum mCurrentBlendDstRgb{GL_ZERO};
    GLenum mCurrentBlendSrcAlpha{GL_ONE};
    GLenum mCurrentBlendDstAlpha{GL_ZERO};
    bool mCurrentCullFace{false};
    GLenum mCurrentCullFaceMode{GL_BACK};
    float mCurrentClearDepth{1.0f};
    GLuint mCurrentProgram{0};

    std::vector<ClearRecord> mClearRecords;
    std::vector<InvalidateRecord> mInvalidateRecords;
    bool mFbo0DepthInvalidated{false};
    uint32_t mUniformLogCount{0};
    std::vector<MatrixCandidate> mMatrixCandidates;

    std::unordered_map<GLuint, ImageDesc> mTextureMap;
    std::unordered_map<GLuint, ImageDesc> mRenderbufferMap;
    std::unordered_map<GLuint, FboDesc> mFboMap;

    std::vector<PassSegment> mCurrentFrameSegments;
    std::mutex mFrameSegmentsMutex;

    std::vector<pl::memory::HookHandle> mPermanentHooks;
    std::vector<pl::memory::HookHandle> mHeavyHooks;

    void armTraceNow(const std::string &label);
    void updateWorldState(uint64_t frameCount, uint64_t frameDraws,
                          bool hasPerspective, uint32_t chunkTranslationCount,
                          const std::chrono::steady_clock::time_point &now);
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

    static void detourDepthFunc(GLenum func);
    static void detourDepthRangef(GLfloat n, GLfloat f);
    static void detourBlendFunc(GLenum sfactor, GLenum dfactor);
    static void detourBlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
    static void detourCullFace(GLenum mode);
    static void detourClearDepthf(GLfloat depth);
    static void detourClear(GLbitfield mask);

    static void detourInvalidateFramebuffer(GLenum target, GLsizei numAttachments, const GLenum *attachments);
    static void detourInvalidateSubFramebuffer(GLenum target, GLsizei numAttachments, const GLenum *attachments, GLint x, GLint y, GLsizei width, GLsizei height);
    static void detourDiscardFramebufferEXT(GLenum target, GLsizei numAttachments, const GLenum *attachments);

    static void detourUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    static void detourUniform4fv(GLint location, GLsizei count, const GLfloat *value);
    static void detourBindBufferBase(GLenum target, GLuint index, GLuint buffer);
    static void detourBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    static void detourBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
    static void detourBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
    static void *detourMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
    static GLboolean detourUnmapBuffer(GLenum target);

    void checkMatrixCandidate(GLuint prog, uint32_t drawRunIdx, const float *m, const char *source);

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
