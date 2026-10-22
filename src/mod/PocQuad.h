#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <GLES3/gl3.h>

namespace lod_poc {

class PocQuad {
public:
    static PocQuad &instance();

    static bool isRendering();

    bool isEnabled() const {
        return mEnabled.load(std::memory_order_relaxed) && !mKilled.load(std::memory_order_relaxed);
    }

    void setConfig(bool enabled, float depth, const std::string &colorHex, const std::string &trigger);

    void onBindFramebufferBefore(GLuint newFbo, GLuint currentBoundFbo, uint64_t frameDraws, GLenum terrainDepthFunc, bool depthInvalidated);
    void onClearAfter(GLbitfield mask, GLuint currentBoundFbo, uint64_t frameDraws, GLenum terrainDepthFunc);

    void onSwapBuffers(double currentFps);

    void cleanup();

private:
    PocQuad() = default;
    ~PocQuad() { cleanup(); }

    bool initGL();
    void drawQuad(GLuint targetFbo, GLenum terrainDepthFunc, bool depthInvalidated);

    std::atomic<bool> mEnabled{false};
    std::atomic<bool> mKilled{false};
    std::atomic<float> mDepth{0.5f};

    std::mutex mConfigMutex;
    std::string mColorHex{"ff00ff"};
    std::array<float, 4> mColorRgba{1.0f, 0.0f, 1.0f, 1.0f};
    std::string mTrigger{"before_fbo1"};

    // Render resources (created on render thread)
    bool mInitialized{false};
    GLuint mProgram{0};
    GLint mLocDepth{-1};
    GLint mLocColor{-1};
    GLuint mVao{0};
    GLuint mVbo{0};

    // Frame tracking
    std::atomic<bool> mDrawnThisFrame{false};
    uint64_t mTotalDrawCount{0};
    GLenum mLastDepthFuncUsed{GL_LEQUAL};
    bool mLastDepthInvalidated{false};

    // Telemetry
    std::chrono::steady_clock::time_point mLastTelemetryTime{std::chrono::steady_clock::now()};
};

} // namespace lod_poc
