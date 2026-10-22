#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <GLES3/gl3.h>

#include "mod/ChunkTranslationMath.h"
#include "mod/LodMesh.h"

namespace lod_poc {

class PocQuad {
public:
    struct FrameMatrixStats {
        bool hasPerspective{false};
        uint32_t chunkTranslationCount{0};
    };

    static PocQuad &instance();

    static bool isRendering();

    bool isEnabled() const {
        return mEnabled.load(std::memory_order_relaxed) && !mKilled.load(std::memory_order_relaxed);
    }

    bool isInWorld() const {
        return mInWorld.load(std::memory_order_relaxed);
    }

    void setConfig(bool enabled, float depth, const std::string &colorHex, const std::string &trigger, bool poc3d = true, float poc3dDist = 8.0f, float minLodDist = 64.0f, float maxLodDist = 512.0f);

    /** Update the F3 origin.  Applied on the render thread at the next frame boundary. */
    void setOriginConfig(float x, float y, float z, bool valid);

    /** Receive newly generated LOD terrain meshes from WorldReader (background thread). */
    void setLodMeshes(std::vector<LodTileMesh> &&meshes);

    /** Receive discovered player world coordinates from WorldReader. */
    void setDiscoveredPlayerPos(float x, float y, float z);

    void onUniformMatrix4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
    void onBindFramebufferBefore(GLuint newFbo, GLuint currentBoundFbo, uint64_t frameDraws, GLenum terrainDepthFunc, bool depthInvalidated);
    void onClearAfter(GLbitfield mask, GLuint currentBoundFbo, uint64_t frameDraws, GLenum terrainDepthFunc);

    /**
     * Finalize the matrix capture for the frame that is ending.  This is called
     * before the world-state decision so the detection logic can use the
     * perspective/chunk flags.  It does not render anything.
     */
    FrameMatrixStats endMatrixFrame();

    /**
     * Called once per frame after GlTrace has updated the in-world state.
     * Camera tracking, chunk statistics logging, and the test-object telemetry
     * are only performed while inWorld is true.
     */
    void onWorldFrame(bool inWorld, double currentFps, uint64_t frameCount);

    void cleanup();

private:
    PocQuad() = default;
    ~PocQuad() { cleanup(); }

    bool initGL();
    void drawQuad(GLuint targetFbo, GLenum terrainDepthFunc, bool depthInvalidated);

    bool initCubeGL();
    void drawCube(GLuint targetFbo, GLenum terrainDepthFunc);

    struct TranslationKey {
        int64_t x{0};
        int64_t y{0};
        int64_t z{0};

        bool operator<(const TranslationKey &other) const {
            if (x != other.x) return x < other.x;
            if (y != other.y) return y < other.y;
            return z < other.z;
        }
    };

    struct OriginRequest {
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        bool valid{false};
        uint64_t generation{0};
    };

    void applyPendingOrigin();
    void resetAnchor(const char *reason);
    bool detectTranslationSign(float &sign) const;
    void computeCubeModelOffset(float out[3]) const;
    void logChunkStats(uint64_t frameCount);
    void updateCameraTracking(uint64_t frameCount);
    void logCamera(uint64_t frameCount, bool force);
    void logLegacyAnchorWarning();

    std::atomic<bool> mEnabled{false};
    std::atomic<bool> mKilled{false};
    std::atomic<bool> mInWorld{false};
    std::atomic<float> mDepth{0.5f};
    std::atomic<bool> mPoc3d{true};
    std::atomic<float> mPoc3dDist{8.0f};
    std::atomic<float> mMinLodDist{64.0f};
    std::atomic<float> mMaxLodDist{512.0f};

    std::mutex mConfigMutex;
    std::string mColorHex{"ff00ff"};
    std::array<float, 4> mColorRgba{1.0f, 0.0f, 1.0f, 1.0f};
    std::string mTrigger{"before_fbo1"};

    // 2D Quad Render resources (created on render thread)
    bool mInitialized{false};
    GLuint mProgram{0};
    GLint mLocDepth{-1};
    GLint mLocColor{-1};
    GLuint mVao{0};
    GLuint mVbo{0};

    // 3D Matrix & World Anchor state
    GLuint mTerrainProgram{0};
    bool mHasProjMatrix{false};
    float mProjMatrix[16]{0};
    bool mHasViewMatrix{false};
    float mViewMatrix[16]{0};
    bool mHasChunkOffset{false};
    float mLastChunkTranslation[3]{0, 0, 0};

    bool mCubeAnchored{false};
    bool mAbsoluteAnchor{false};
    float mAnchorWorldPos[3]{0, 0, 0};       // Legacy camera-relative anchor
    float mAnchorChunkOffset[3]{0, 0, 0};    // Legacy last-chunk translation at anchor
    float mCubeAbsolutePos[3]{0, 0, 0};      // Absolute world position when origin tracking is active

    // Per-frame matrix capture (render thread only)
    bool mCaptureHasPerspective{false};
    bool mCurrentHasPerspective{false};
    std::set<TranslationKey> mCaptureTranslationKeys;
    std::vector<chunkmath::Vec3f> mCaptureTranslations;
    std::vector<chunkmath::Vec3f> mCurrentTranslations;
    std::vector<chunkmath::Vec3f> mPreviousTranslations;

    // Camera-origin tracking (render thread only once applied)
    std::mutex mOriginMutex;
    OriginRequest mPendingOrigin;
    uint64_t mOriginGenerationCounter{0};
    uint64_t mAppliedOriginGeneration{0};
    bool mOriginRequestInitialized{false};

    bool mOriginValid{false};
    bool mAutoOrigin{true};
    float mOriginX{0.0f};
    float mOriginY{0.0f};
    float mOriginZ{0.0f};
    bool mCameraTracked{false};
    bool mTrackerPrimed{false};
    float mTrackedCameraPos[3]{0, 0, 0};
    float mTranslationSign{-1.0f}; // -1 => t = 16*k - camera; +1 => t = 16*k + camera
    bool mSignDetected{false};
    int mLastCameraMatches{0};
    int mLastCameraPreviousCount{0};

    // 3D Cube Render resources
    bool mCubeInitialized{false};
    GLuint mCubeProgram{0};
    GLint mCubeLocProj{-1};
    GLint mCubeLocView{-1};
    GLint mCubeLocModelOffset{-1};
    GLuint mCubeVao{0};
    GLuint mCubeVbo{0};
    GLuint mCubeEbo{0};

    // LOD Terrain Render resources
    struct LodTileGlBuffer {
        GLuint vao{0};
        GLuint vbo{0};
        GLuint ebo{0};
        GLsizei indexCount{0};
    };

    std::mutex mLodMeshMutex;
    std::vector<LodTileMesh> mPendingLodMeshes;
    bool mHasNewLodMeshes{false};
    std::vector<LodTileGlBuffer> mLodGlBuffers;
    bool mLodGlInitialized{false};
    GLuint mLodProgram{0};
    GLint mLodLocProj{-1};
    GLint mLodLocView{-1};
    GLint mLodLocCameraPos{-1};
    GLint mLodLocMinDist{-1};
    GLint mLodLocMaxDist{-1};

    float mDiscoveredPlayerPos[3]{0.0f, 0.0f, 0.0f};
    bool mHasDiscoveredPlayerPos{false};
    bool mDiscoveredPosPending{false};

    bool initLodGL();
    void uploadPendingLodMeshes();
    void drawLodTerrain(GLuint targetFbo, GLenum terrainDepthFunc);

    // Frame tracking
    std::atomic<bool> mDrawnThisFrame{false};
    uint64_t mTotalDrawCount{0};
    GLenum mLastDepthFuncUsed{GL_LEQUAL};
    bool mLastDepthInvalidated{false};

    // Telemetry / rate limiting
    std::chrono::steady_clock::time_point mLastTelemetryTime{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point mLastStatsLogTime{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point mLastCameraLogTime{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point mLastCameraWarnTime{};
    bool mLegacyAnchorWarningLogged{false};
};

} // namespace lod_poc
