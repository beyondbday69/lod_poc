#include "mod/PocQuad.h"
#include "mod/LodPocMod.h"

#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace lod_poc {

namespace {

thread_local bool sInsideQuadRender = false;

using PFN_glGetIntegeri_v = void (*)(GLenum target, GLuint index, GLint *data);
using PFN_glGetInteger64i_v = void (*)(GLenum target, GLuint index, GLint64 *data);
using PFN_glBindBufferRange = void (*)(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
using PFN_glBindBufferBase = void (*)(GLenum target, GLuint index, GLuint buffer);

static PFN_glGetIntegeri_v sGetIntegeri_v = nullptr;
static PFN_glGetInteger64i_v sGetInteger64i_v = nullptr;
static PFN_glBindBufferRange sBindBufferRange = nullptr;
static PFN_glBindBufferBase sBindBufferBase = nullptr;
static bool sUboFunctionsResolved = false;

static void resolveUboFunctions() {
    if (sUboFunctionsResolved) return;
    sUboFunctionsResolved = true;

    void *glesLib = dlopen("libGLESv3.so", RTLD_NOW | RTLD_GLOBAL);
    if (!glesLib) glesLib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);

    auto lookup = [glesLib](const char *name) -> void * {
        void *sym = nullptr;
        if (glesLib) sym = dlsym(glesLib, name);
        if (!sym) sym = dlsym(RTLD_DEFAULT, name);
        return sym;
    };

    sGetIntegeri_v = reinterpret_cast<PFN_glGetIntegeri_v>(lookup("glGetIntegeri_v"));
    sGetInteger64i_v = reinterpret_cast<PFN_glGetInteger64i_v>(lookup("glGetInteger64i_v"));
    sBindBufferRange = reinterpret_cast<PFN_glBindBufferRange>(lookup("glBindBufferRange"));
    sBindBufferBase = reinterpret_cast<PFN_glBindBufferBase>(lookup("glBindBufferBase"));
}

std::array<float, 4> parseHexColor(const std::string &hex) {
    std::string s = hex;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() != 6 && s.size() != 8) {
        return {1.0f, 0.0f, 1.0f, 1.0f}; // default magenta
    }
    auto parseByte = [](const std::string &str, size_t idx) -> float {
        unsigned int val = 0;
        std::stringstream ss;
        ss << std::hex << str.substr(idx, 2);
        if (ss >> val) {
            return static_cast<float>(val) / 255.0f;
        }
        return 0.0f;
    };
    float r = parseByte(s, 0);
    float g = parseByte(s, 2);
    float b = parseByte(s, 4);
    float a = (s.size() == 8) ? parseByte(s, 6) : 1.0f;
    return {r, g, b, a};
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
        default:          return "DEFAULT(LEQUAL)";
    }
}

struct CubeVertex {
    float x, y, z;
    float nx, ny, nz;
    float r, g, b;
};

// 24 vertices for a 2x2x2 cube centered at origin
const float h = 1.0f;
static const CubeVertex sCubeVertices[] = {
    // Top face (+Y, Normal: 0, 1, 0, Green)
    {-h,  h, -h,  0, 1, 0,  0.25f, 0.85f, 0.25f},
    { h,  h, -h,  0, 1, 0,  0.25f, 0.85f, 0.25f},
    { h,  h,  h,  0, 1, 0,  0.25f, 0.85f, 0.25f},
    {-h,  h,  h,  0, 1, 0,  0.25f, 0.85f, 0.25f},

    // Bottom face (-Y, Normal: 0, -1, 0, Brown)
    {-h, -h, -h,  0, -1, 0,  0.50f, 0.35f, 0.20f},
    { h, -h, -h,  0, -1, 0,  0.50f, 0.35f, 0.20f},
    { h, -h,  h,  0, -1, 0,  0.50f, 0.35f, 0.20f},
    {-h, -h,  h,  0, -1, 0,  0.50f, 0.35f, 0.20f},

    // Front face (+Z, Normal: 0, 0, 1, Red)
    {-h, -h,  h,  0, 0, 1,  0.90f, 0.25f, 0.25f},
    { h, -h,  h,  0, 0, 1,  0.90f, 0.25f, 0.25f},
    { h,  h,  h,  0, 0, 1,  0.90f, 0.25f, 0.25f},
    {-h,  h,  h,  0, 0, 1,  0.90f, 0.25f, 0.25f},

    // Back face (-Z, Normal: 0, 0, -1, Blue)
    { h, -h, -h,  0, 0, -1,  0.25f, 0.45f, 0.90f},
    {-h, -h, -h,  0, 0, -1,  0.25f, 0.45f, 0.90f},
    {-h,  h, -h,  0, 0, -1,  0.25f, 0.45f, 0.90f},
    { h,  h, -h,  0, 0, -1,  0.25f, 0.45f, 0.90f},

    // Right face (+X, Normal: 1, 0, 0, Yellow)
    { h, -h,  h,  1, 0, 0,  0.90f, 0.90f, 0.25f},
    { h, -h, -h,  1, 0, 0,  0.90f, 0.90f, 0.25f},
    { h,  h, -h,  1, 0, 0,  0.90f, 0.90f, 0.25f},
    { h,  h,  h,  1, 0, 0,  0.90f, 0.90f, 0.25f},

    // Left face (-X, Normal: -1, 0, 0, Cyan)
    {-h, -h, -h, -1, 0, 0,  0.25f, 0.85f, 0.90f},
    {-h, -h,  h, -1, 0, 0,  0.25f, 0.85f, 0.90f},
    {-h,  h,  h, -1, 0, 0,  0.25f, 0.85f, 0.90f},
    {-h,  h, -h, -1, 0, 0,  0.25f, 0.85f, 0.90f},
};

static const GLushort sCubeIndices[] = {
    // All faces must be CCW when viewed from outside so their geometric
    // normal matches the per-vertex normal above.
    0, 2, 1,  0, 3, 2,        // Top (+Y)
    4, 5, 6,  4, 6, 7,        // Bottom (-Y)
    8, 9, 10,  8, 10, 11,     // Front (+Z)
    12, 13, 14,  12, 14, 15,  // Back (-Z)
    16, 17, 18,  16, 18, 19,  // Right (+X)
    20, 21, 22,  20, 22, 23   // Left (-X)
};

bool isPerspectiveMatrix(const float *m) {
    // Kept identical to the detection used by the original PocQuad and by
    // the GL trace matrix finder.
    return std::abs(m[11] - (-1.0f)) < 0.1f && m[0] > 0.1f && m[5] > 0.1f;
}

bool isChunkTranslationMatrix(const float *m) {
    // Chunk model matrices are identity matrices with a camera-relative
    // translation in the last column.  Compare exactly, as before.
    if (!(m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f && m[15] == 1.0f &&
          m[1] == 0.0f && m[2] == 0.0f && m[3] == 0.0f &&
          m[4] == 0.0f && m[6] == 0.0f && m[7] == 0.0f &&
          m[8] == 0.0f && m[9] == 0.0f && m[11] == 0.0f)) {
        return false;
    }
    return std::abs(m[12]) > 0.1f || std::abs(m[13]) > 0.1f || std::abs(m[14]) > 0.1f;
}

int64_t quantizeTranslation(float value) {
    // 1/1024 block is well below the precision needed for F3 comparison and
    // avoids duplicate near-identical matrices from being counted twice.
    return static_cast<int64_t>(std::llround(static_cast<double>(value) * 1024.0));
}

std::string formatVec3(const float v[3]) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4)
        << "(" << v[0] << ", " << v[1] << ", " << v[2] << ")";
    return out.str();
}

std::string formatVec3(const chunkmath::Vec3f &v) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(4)
        << "(" << v.x << ", " << v.y << ", " << v.z << ")";
    return out.str();
}

std::string formatMod16Values(const std::vector<chunkmath::Vec3f> &translations,
                              int component,
                              size_t limit) {
    std::ostringstream out;
    out << '[';
    const size_t count = std::min(limit, translations.size());
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) out << ", ";
        const chunkmath::Vec3f &t = translations[i];
        const float value = (component == 0) ? t.x : ((component == 1) ? t.y : t.z);
        out << std::fixed << std::setprecision(3) << chunkmath::positiveMod16(value);
    }
    out << ']';
    return out.str();
}

} // namespace

PocQuad &PocQuad::instance() {
    static PocQuad inst;
    return inst;
}

bool PocQuad::isRendering() {
    return sInsideQuadRender;
}

void PocQuad::setConfig(bool enabled, float depth, const std::string &colorHex, const std::string &trigger, bool poc3d, float poc3dDist, float minLodDist, float maxLodDist) {
    bool wasEnabled = mEnabled.load(std::memory_order_relaxed);
    mEnabled.store(enabled, std::memory_order_relaxed);
    mDepth.store(std::clamp(depth, 0.0f, 1.0f), std::memory_order_relaxed);

    const float newDist = poc3dDist > 0.5f ? poc3dDist : 8.0f;
    float oldDist = mPoc3dDist.load(std::memory_order_relaxed);
    bool old3d = mPoc3d.load(std::memory_order_relaxed);
    mPoc3d.store(poc3d, std::memory_order_relaxed);
    mPoc3dDist.store(newDist, std::memory_order_relaxed);
    mMinLodDist.store(std::max(16.0f, minLodDist), std::memory_order_relaxed);
    mMaxLodDist.store(std::max(minLodDist + 32.0f, maxLodDist), std::memory_order_relaxed);

    if (oldDist != newDist || old3d != poc3d) {
        mCubeAnchored = false;
        mAbsoluteAnchor = false;
    }

    {
        std::lock_guard lock(mConfigMutex);
        mColorHex = colorHex;
        mColorRgba = parseHexColor(colorHex);
        mTrigger = trigger;
    }

    if (!wasEnabled && enabled) {
        mKilled.store(false, std::memory_order_relaxed);
        mCubeAnchored = false;
        mAbsoluteAnchor = false;
        LodPocMod::logAll(LogLevel::Info,
            "[PocQuad] Enabled: 3D_Cube={}, dist={:.1f}, minLodDist={:.1f}, maxLodDist={:.1f}, depth={:.3f}, color='#{}', trigger='{}'",
            poc3d ? "true" : "false", poc3dDist, minLodDist, maxLodDist, depth, colorHex, trigger);
    }
}

void PocQuad::setOriginConfig(float x, float y, float z, bool valid) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        LodPocMod::logAll(LogLevel::Warn,
            "[PocQuad: Origin] WARNING: ignoring non-finite origin ({}, {}, {})", x, y, z);
        return;
    }

    std::lock_guard lock(mOriginMutex);
    const bool changed = !mOriginRequestInitialized ||
                         mPendingOrigin.x != x ||
                         mPendingOrigin.y != y ||
                         mPendingOrigin.z != z ||
                         mPendingOrigin.valid != valid;
    if (!changed) {
        return;
    }

    mPendingOrigin.x = x;
    mPendingOrigin.y = y;
    mPendingOrigin.z = z;
    mPendingOrigin.valid = valid;
    mPendingOrigin.generation = ++mOriginGenerationCounter;
    mOriginRequestInitialized = true;
}

void PocQuad::onUniformMatrix4fv(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {
    if (value == nullptr || count < 1 || transpose) return;

    for (GLsizei i = 0; i < count; ++i) {
        const float *m = value + (i * 16);

        // Detect Chunk Model Translation:
        // M[0]==1, M[5]==1, M[10]==1, M[15]==1, M[1..11] == 0, M[12..14] != 0
        if (isChunkTranslationMatrix(m)) {
            mTerrainProgram = program; // Confirm this program draws 3D terrain chunks!
            const chunkmath::Vec3f translation{m[12], m[13], m[14]};
            const TranslationKey key{
                quantizeTranslation(translation.x),
                quantizeTranslation(translation.y),
                quantizeTranslation(translation.z),
            };
            if (mCaptureTranslationKeys.insert(key).second) {
                mCaptureTranslations.push_back(translation);
            }

            // Preserve the legacy "last matrix seen" value for originValid=false.
            mLastChunkTranslation[0] = translation.x;
            mLastChunkTranslation[1] = translation.y;
            mLastChunkTranslation[2] = translation.z;
            mHasChunkOffset = true;
        }

        // Detect Projection Matrix
        // Standard perspective: M[11] == -1.0f (or close) and M[0] > 0.1f, M[5] > 0.1f
        if (isPerspectiveMatrix(m)) {
            std::copy(m, m + 16, mProjMatrix);
            mHasProjMatrix = true;
            mCaptureHasPerspective = true;
        }

        // Detect View (Rotation) Matrix:
        // Pure 3x3 rotation: M[12]==0, M[13]==0, M[14]==0, M[15]==1.0
        // and row lengths == 1.0.
        // CRITICAL: Reject pure identity matrices from UI/HUD/post-processing shaders
        // (like Prog 3 at end of frame). Otherwise Identity overwrites the real view
        // matrix, causing the 3D cube to pin to the screen and follow camera rotation!
        if (std::abs(m[12]) < 0.001f && std::abs(m[13]) < 0.001f && std::abs(m[14]) < 0.001f &&
            std::abs(m[15] - 1.0f) < 0.001f && std::abs(m[3]) < 0.001f && std::abs(m[7]) < 0.001f) {
            float r0 = m[0]*m[0] + m[1]*m[1] + m[2]*m[2];
            float r1 = m[4]*m[4] + m[5]*m[5] + m[6]*m[6];
            float r2 = m[8]*m[8] + m[9]*m[9] + m[10]*m[10];
            if (std::abs(r0 - 1.0f) < 0.08f && std::abs(r1 - 1.0f) < 0.08f && std::abs(r2 - 1.0f) < 0.08f) {
                bool isIdent = std::abs(m[0] - 1.0f) < 0.001f && std::abs(m[5] - 1.0f) < 0.001f &&
                               std::abs(m[10] - 1.0f) < 0.001f && std::abs(m[1]) < 0.001f &&
                               std::abs(m[2]) < 0.001f && std::abs(m[4]) < 0.001f &&
                               std::abs(m[6]) < 0.001f && std::abs(m[8]) < 0.001f && std::abs(m[9]) < 0.001f;
                if (!isIdent || (mTerrainProgram != 0 && program == mTerrainProgram)) {
                    std::copy(m, m + 16, mViewMatrix);
                    mHasViewMatrix = true;
                }
            }
        }
    }
}

PocQuad::FrameMatrixStats PocQuad::endMatrixFrame() {
    FrameMatrixStats stats;
    stats.hasPerspective = mCaptureHasPerspective;
    stats.chunkTranslationCount = static_cast<uint32_t>(mCaptureTranslations.size());

    mCurrentHasPerspective = mCaptureHasPerspective;
    mCurrentTranslations = std::move(mCaptureTranslations);
    mCaptureTranslations.clear();
    mCaptureTranslationKeys.clear();
    mCaptureHasPerspective = false;

    return stats;
}

void PocQuad::setLodMeshes(std::vector<LodTileMesh> &&meshes) {
    std::lock_guard lock(mLodMeshMutex);
    mPendingLodMeshes = std::move(meshes);
    mHasNewLodMeshes = true;
    LodPocMod::logAll(LogLevel::Info,
        "[PocQuad: LOD] Queued {} LOD terrain tiles for GPU upload",
        mPendingLodMeshes.size());
}

void PocQuad::setDiscoveredPlayerPos(float x, float y, float z) {
    std::lock_guard lock(mConfigMutex);
    mDiscoveredPlayerPos[0] = x;
    mDiscoveredPlayerPos[1] = y;
    mDiscoveredPlayerPos[2] = z;
    mHasDiscoveredPlayerPos = true;
    mDiscoveredPosPending = true;
    LodPocMod::logAll(LogLevel::Info,
        "[PocQuad: Origin] Received discovered player world position: ({:.2f}, {:.2f}, {:.2f})",
        x, y, z);
}

void PocQuad::applyPendingOrigin() {
    if (!mOriginValid && mDiscoveredPosPending) {
        mDiscoveredPosPending = false;
        mOriginX = mDiscoveredPlayerPos[0];
        mOriginY = mDiscoveredPlayerPos[1];
        mOriginZ = mDiscoveredPlayerPos[2];
        mTrackedCameraPos[0] = mOriginX;
        mTrackedCameraPos[1] = mOriginY + 1.620f;
        mTrackedCameraPos[2] = mOriginZ;
        mAutoOrigin = false;
        mCameraTracked = true;
        mSignDetected = true;
        mTranslationSign = -1.0f;
        mTrackerPrimed = false;
        mPreviousTranslations.clear();
        mCubeAnchored = false;
        LodPocMod::logAll(LogLevel::Info,
            "[PocQuad: Origin] Auto-calibrated camera to discovered world origin: ({:.4f}, {:.4f}, {:.4f}) with eyeHeight=+1.62",
            mTrackedCameraPos[0], mTrackedCameraPos[1], mTrackedCameraPos[2]);
    }

    OriginRequest request;
    {
        std::lock_guard lock(mOriginMutex);
        if (mPendingOrigin.generation == mAppliedOriginGeneration) {
            return;
        }
        request = mPendingOrigin;
        mAppliedOriginGeneration = request.generation;
    }

    const bool wasValid = mOriginValid;
    const bool changed = (!wasValid && request.valid) ||
                         (wasValid && !request.valid) ||
                         mOriginX != request.x ||
                         mOriginY != request.y ||
                         mOriginZ != request.z;

    mOriginValid = request.valid;
    mOriginX = request.x;
    mOriginY = request.y;
    mOriginZ = request.z;

    if (!changed) {
        return;
    }

    // The absolute cube position belongs to the old origin.  Re-anchor on the
    // next rendered world frame.
    mCubeAnchored = false;
    mAbsoluteAnchor = false;
    mTrackerPrimed = false;
    mPreviousTranslations.clear();
    mSignDetected = false;
    mTranslationSign = -1.0f;
    mLegacyAnchorWarningLogged = false;

    if (!mOriginValid) {
        mAutoOrigin = true;
        mCameraTracked = true;
        mTrackedCameraPos[0] = 0.0f;
        mTrackedCameraPos[1] = 1.620f;
        mTrackedCameraPos[2] = 0.0f;
        LodPocMod::logAll(LogLevel::Info,
            "[PocQuad: Origin] originValid=false; automatic relative camera tracking active (eyeHeight=+1.62; zero-config)");
        return;
    }

    mAutoOrigin = false;
    mCameraTracked = true;
    mTrackedCameraPos[0] = mOriginX;
    mTrackedCameraPos[1] = mOriginY + 1.620f;
    mTrackedCameraPos[2] = mOriginZ;

    float sign = -1.0f;
    const bool signDetected = detectTranslationSign(sign);
    if (signDetected) {
        mTranslationSign = sign;
        mSignDetected = true;
    }

    std::string detail;
    if (!mCurrentTranslations.empty()) {
        const auto &t = mCurrentTranslations.front();
        std::ostringstream out;
        out << std::fixed << std::setprecision(4)
            << " sampleTranslation=" << formatVec3(t)
            << " tmod16=(" << chunkmath::positiveMod16(t.x) << ", "
            << chunkmath::positiveMod16(t.y) << ", "
            << chunkmath::positiveMod16(t.z) << ")"
            << " cameraFrac(-t)=(" << chunkmath::positiveMod16(-t.x) << ", "
            << chunkmath::positiveMod16(-t.y) << ", "
            << chunkmath::positiveMod16(-t.z) << ")";
        detail = out.str();
    }

    LodPocMod::logAll(LogLevel::Info,
        "[PocQuad: Origin] {} origin=({:.4f}, {:.4f}, {:.4f}) with eye-height (+1.62 -> cameraY={:.4f}); camera initialized; translationSign={:+.0f} ({}){}",
        changed ? "applied" : "unchanged",
        mOriginX, mOriginY, mOriginZ, mTrackedCameraPos[1],
        mTranslationSign,
        mSignDetected ? "auto-detected" : "default hypothesis t=16*k-camera",
        detail);
}

void PocQuad::resetAnchor(const char * /*reason*/) {
    mCubeAnchored = false;
    mAbsoluteAnchor = false;
}

bool PocQuad::detectTranslationSign(float &sign) const {
    if (mCurrentTranslations.empty()) {
        return false;
    }

    float minusError = 0.0f;
    float plusError = 0.0f;
    const float origin[3] = {mOriginX, mOriginY, mOriginZ};
    const size_t sampleCount = std::min<size_t>(mCurrentTranslations.size(), 10);
    for (size_t sample = 0; sample < sampleCount; ++sample) {
        const auto &t = mCurrentTranslations[sample];
        const float translation[3] = {t.x, t.y, t.z};
        for (int i = 0; i < 3; ++i) {
            minusError += chunkmath::circularDistance16(origin[i], -translation[i]);
            plusError += chunkmath::circularDistance16(origin[i], translation[i]);
        }
    }

    // Do not flip the sign on an ambiguous fractional match.  The default is
    // the hypothesis stated in the task: t = 16*k - cameraPos.
    if (std::fabs(minusError - plusError) <= 0.02f) {
        return false;
    }

    sign = (minusError < plusError) ? -1.0f : 1.0f;
    return true;
}

void PocQuad::logChunkStats(uint64_t frameCount) {
    const auto now = std::chrono::steady_clock::now();
    if (now - mLastStatsLogTime < std::chrono::seconds(5)) {
        return;
    }
    mLastStatsLogTime = now;

    const size_t count = mCurrentTranslations.size();
    if (count == 0) {
        LodPocMod::logAll(LogLevel::Info,
            "[PocQuad: ChunkStats] frame={} distinct=0 perspective={} min=n/a max=n/a tmod16 x=[] y=[] z=[] hypothesis=NO",
            frameCount, mCurrentHasPerspective ? "yes" : "no");
        return;
    }

    float minValue[3] = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    float maxValue[3] = {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };

    float maxModDeviation = 0.0f;
    const float firstMod[3] = {
        chunkmath::positiveMod16(mCurrentTranslations.front().x),
        chunkmath::positiveMod16(mCurrentTranslations.front().y),
        chunkmath::positiveMod16(mCurrentTranslations.front().z),
    };

    for (const auto &t : mCurrentTranslations) {
        const float v[3] = {t.x, t.y, t.z};
        for (int i = 0; i < 3; ++i) {
            minValue[i] = std::min(minValue[i], v[i]);
            maxValue[i] = std::max(maxValue[i], v[i]);
            maxModDeviation = std::max(maxModDeviation,
                chunkmath::circularDistance16(firstMod[i], v[i]));
        }
    }

    const bool hypothesisHolds = maxModDeviation <= 0.02f;
    const auto &first = mCurrentTranslations.front();
    const float cameraFraction[3] = {
        chunkmath::positiveMod16(mTranslationSign < 0.0f ? -first.x : first.x),
        chunkmath::positiveMod16(mTranslationSign < 0.0f ? -first.y : first.y),
        chunkmath::positiveMod16(mTranslationSign < 0.0f ? -first.z : first.z),
    };

    std::ostringstream minStr;
    std::ostringstream maxStr;
    minStr << std::fixed << std::setprecision(4)
           << '(' << minValue[0] << ", " << minValue[1] << ", " << minValue[2] << ')';
    maxStr << std::fixed << std::setprecision(4)
           << '(' << maxValue[0] << ", " << maxValue[1] << ", " << maxValue[2] << ')';

    LodPocMod::logAll(LogLevel::Info,
        "[PocQuad: ChunkStats] frame={} distinct={} perspective={} min={} max={} tmod16 x={} y={} z={} hypothesis={} maxModDeviation={:.4f} inferredCameraFrac={} sign={:+.0f}",
        frameCount,
        count,
        mCurrentHasPerspective ? "yes" : "no",
        minStr.str(), maxStr.str(),
        formatMod16Values(mCurrentTranslations, 0, 10),
        formatMod16Values(mCurrentTranslations, 1, 10),
        formatMod16Values(mCurrentTranslations, 2, 10),
        hypothesisHolds ? "YES" : "NO",
        maxModDeviation,
        formatVec3(cameraFraction),
        mTranslationSign);
}

void PocQuad::updateCameraTracking(uint64_t frameCount) {
    if (!mCameraTracked) {
        return;
    }

    if (mOriginValid && !mSignDetected && !mCurrentTranslations.empty()) {
        float detectedSign = mTranslationSign;
        if (detectTranslationSign(detectedSign)) {
            mTranslationSign = detectedSign;
            mSignDetected = true;
            LodPocMod::logAll(LogLevel::Info,
                "[PocQuad: Origin] translation sign auto-detected from origin fraction: {:.0f} ({})",
                mTranslationSign,
                mTranslationSign < 0.0f ? "t = 16*k - cameraPos" : "t = 16*k + cameraPos");
        }
    }

    if (!mTrackerPrimed) {
        if (!mCurrentTranslations.empty()) {
            mPreviousTranslations = mCurrentTranslations;
            mTrackerPrimed = true;
            LodPocMod::logAll(LogLevel::Info,
                "[PocQuad: Origin] camera tracker primed at frame {} with {} chunk translations",
                frameCount, mCurrentTranslations.size());
        }
        return;
    }

    if (mCurrentTranslations.empty()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - mLastCameraWarnTime >= std::chrono::seconds(1)) {
            mLastCameraWarnTime = now;
            LodPocMod::logAll(LogLevel::Warn,
                "[PocQuad: Camera] WARNING: translation matching lost at frame {} (current frame has no chunk translations); keeping camera at ({:.4f}, {:.4f}, {:.4f})",
                frameCount,
                mTrackedCameraPos[0], mTrackedCameraPos[1], mTrackedCameraPos[2]);
        }
        return;
    }

    const auto match = chunkmath::findBestShift(mPreviousTranslations, mCurrentTranslations);
    mLastCameraMatches = match.matches;
    mLastCameraPreviousCount = match.previousCount;

    if (!match.found) {
        const auto now = std::chrono::steady_clock::now();
        if (now - mLastCameraWarnTime >= std::chrono::seconds(1)) {
            mLastCameraWarnTime = now;
            LodPocMod::logAll(LogLevel::Warn,
                "[PocQuad: Camera] WARNING: translation matching lost at frame {} (bestMatches={}, previous={}, current={}); keeping camera at ({:.4f}, {:.4f}, {:.4f})",
                frameCount,
                match.matches, match.previousCount, match.currentCount,
                mTrackedCameraPos[0], mTrackedCameraPos[1], mTrackedCameraPos[2]);
        }
        mPreviousTranslations = mCurrentTranslations;
        return;
    }

    // If t = 16*k - cameraPos, a positive translation shift means the camera
    // moved negatively.  The auto-detected sign handles the opposite
    // convention if this build uses it.
    const float deltaX = mTranslationSign * match.shift.x;
    const float deltaY = mTranslationSign * match.shift.y;
    const float deltaZ = mTranslationSign * match.shift.z;
    const float jump = std::sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);

    mTrackedCameraPos[0] += deltaX;
    mTrackedCameraPos[1] += deltaY;
    mTrackedCameraPos[2] += deltaZ;

    if (jump > 1.0f) {
        const auto now = std::chrono::steady_clock::now();
        if (now - mLastCameraWarnTime >= std::chrono::seconds(1)) {
            mLastCameraWarnTime = now;
            LodPocMod::logAll(LogLevel::Warn,
                "[PocQuad: Camera] WARNING: camera jumped {:.4f} blocks in one frame at frame {} (shift=({:.4f}, {:.4f}, {:.4f}), matches={}/{}); new camera=({:.4f}, {:.4f}, {:.4f})",
                jump, frameCount,
                deltaX, deltaY, deltaZ,
                match.matches, match.previousCount,
                mTrackedCameraPos[0], mTrackedCameraPos[1], mTrackedCameraPos[2]);
        }
    }

    mPreviousTranslations = mCurrentTranslations;
}

void PocQuad::logCamera(uint64_t frameCount, bool force) {
    if (!mCameraTracked) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!force && now - mLastCameraLogTime < std::chrono::seconds(1)) {
        return;
    }
    mLastCameraLogTime = now;

    float fractionalError = 0.0f;
    if (mOriginValid && !mCurrentTranslations.empty()) {
        const auto &t = mCurrentTranslations.front();
        const float observed[3] = {
            chunkmath::positiveMod16(mTranslationSign < 0.0f ? -t.x : t.x),
            chunkmath::positiveMod16(mTranslationSign < 0.0f ? -t.y : t.y),
            chunkmath::positiveMod16(mTranslationSign < 0.0f ? -t.z : t.z),
        };
        for (int i = 0; i < 3; ++i) {
            fractionalError = std::max(fractionalError,
                chunkmath::circularDistance16(observed[i], mTrackedCameraPos[i]));
        }
    }

    LodPocMod::logAll(LogLevel::Info,
        "[PocQuad: Camera] frame={} camera=({:.4f}, {:.4f}, {:.4f}) mode={} tracker={} matches={}/{} fractionalError={:.4f} sign={:+.0f}",
        frameCount,
        mTrackedCameraPos[0], mTrackedCameraPos[1], mTrackedCameraPos[2],
        mAutoOrigin ? "auto-relative (zero-config, eyeHeight=+1.62)" : "absolute",
        mTrackerPrimed ? "primed" : "priming",
        mLastCameraMatches, mLastCameraPreviousCount,
        fractionalError,
        mTranslationSign);
}

void PocQuad::logLegacyAnchorWarning() {
    if (mLegacyAnchorWarningLogged) {
        return;
    }
    mLegacyAnchorWarningLogged = true;
    LodPocMod::logAll(LogLevel::Warn,
        "[PocQuad: 3D] WARNING: originValid=false; using legacy last-chunk anchor. "
        "The cube may drift when multiple chunk translations are visible. "
        "Set originX/originY/originZ and originValid=true for absolute world anchoring.");
}

bool PocQuad::initGL() {
    const char *vsSource =
        "#version 300 es\n"
        "layout(location = 0) in vec2 aPos;\n"
        "uniform float uDepth;\n"
        "void main() {\n"
        "    gl_Position = vec4(aPos, uDepth, 1.0);\n"
        "}\n";

    const char *fsSource =
        "#version 300 es\n"
        "precision mediump float;\n"
        "uniform vec4 uColor;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    fragColor = uColor;\n"
        "}\n";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vsSource, nullptr);
    glCompileShader(vs);
    GLint status = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(vs, sizeof(log), nullptr, log);
        LodPocMod::logAll(LogLevel::Error, "[PocQuad] Failed to compile quad VS: {}", log);
        glDeleteShader(vs);
        mKilled.store(true, std::memory_order_relaxed);
        return false;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fsSource, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(fs, sizeof(log), nullptr, log);
        LodPocMod::logAll(LogLevel::Error, "[PocQuad] Failed to compile quad FS: {}", log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        mKilled.store(true, std::memory_order_relaxed);
        return false;
    }

    mProgram = glCreateProgram();
    glAttachShader(mProgram, vs);
    glAttachShader(mProgram, fs);
    glLinkProgram(mProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGetProgramiv(mProgram, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(mProgram, sizeof(log), nullptr, log);
        LodPocMod::logAll(LogLevel::Error, "[PocQuad] Failed to link quad program: {}", log);
        glDeleteProgram(mProgram);
        mProgram = 0;
        mKilled.store(true, std::memory_order_relaxed);
        return false;
    }

    mLocDepth = glGetUniformLocation(mProgram, "uDepth");
    mLocColor = glGetUniformLocation(mProgram, "uColor");

    static const float sQuadVertices[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };

    glGenVertexArrays(1, &mVao);
    glBindVertexArray(mVao);

    glGenBuffers(1, &mVbo);
    glBindBuffer(GL_ARRAY_BUFFER, mVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(sQuadVertices), sQuadVertices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), reinterpret_cast<const void *>(0));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LodPocMod::logAll(LogLevel::Error, "[PocQuad] GL error 0x{:x} during quad setup", err);
        mKilled.store(true, std::memory_order_relaxed);
        return false;
    }

    mInitialized = true;
    LodPocMod::logAll(LogLevel::Info, "[PocQuad] Initialized 2D quad program ({}) successfully", mProgram);
    return true;
}

bool PocQuad::initCubeGL() {
    const char *vsSource =
        "#version 300 es\n"
        "layout(location = 0) in vec3 aPos;\n"
        "layout(location = 1) in vec3 aNormal;\n"
        "layout(location = 2) in vec3 aColor;\n"
        "uniform mat4 uProj;\n"
        "uniform mat4 uView;\n"
        "uniform vec3 uModelOffset;\n"
        "out vec3 vColor;\n"
        "void main() {\n"
        "    vec3 worldPos = aPos + uModelOffset;\n"
        "    vec4 eyePos = uView * vec4(worldPos, 1.0);\n"
        "    gl_Position = uProj * eyePos;\n"
        "    vec3 lightDir = normalize(vec3(0.4, 0.9, 0.3));\n"
        "    float diff = max(dot(aNormal, lightDir), 0.50);\n"
        "    vColor = aColor * diff;\n"
        "}\n";

    const char *fsSource =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec3 vColor;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    fragColor = vec4(vColor, 1.0);\n"
        "}\n";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vsSource, nullptr);
    glCompileShader(vs);
    GLint status = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(vs, sizeof(log), nullptr, log);
        LodPocMod::logAll(LogLevel::Error, "[PocQuad: 3D] Failed to compile cube VS: {}", log);
        glDeleteShader(vs);
        mKilled.store(true, std::memory_order_relaxed);
        return false;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fsSource, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(fs, sizeof(log), nullptr, log);
        LodPocMod::logAll(LogLevel::Error, "[PocQuad: 3D] Failed to compile cube FS: {}", log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        mKilled.store(true, std::memory_order_relaxed);
        return false;
    }

    mCubeProgram = glCreateProgram();
    glAttachShader(mCubeProgram, vs);
    glAttachShader(mCubeProgram, fs);
    glLinkProgram(mCubeProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGetProgramiv(mCubeProgram, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(mCubeProgram, sizeof(log), nullptr, log);
        LodPocMod::logAll(LogLevel::Error, "[PocQuad: 3D] Failed to link cube program: {}", log);
        glDeleteProgram(mCubeProgram);
        mCubeProgram = 0;
        mKilled.store(true, std::memory_order_relaxed);
        return false;
    }

    mCubeLocProj = glGetUniformLocation(mCubeProgram, "uProj");
    mCubeLocView = glGetUniformLocation(mCubeProgram, "uView");
    mCubeLocModelOffset = glGetUniformLocation(mCubeProgram, "uModelOffset");

    glGenVertexArrays(1, &mCubeVao);
    glBindVertexArray(mCubeVao);

    glGenBuffers(1, &mCubeVbo);
    glBindBuffer(GL_ARRAY_BUFFER, mCubeVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(sCubeVertices), sCubeVertices, GL_STATIC_DRAW);

    glGenBuffers(1, &mCubeEbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mCubeEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(sCubeIndices), sCubeIndices, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex), reinterpret_cast<const void *>(offsetof(CubeVertex, x)));

    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex), reinterpret_cast<const void *>(offsetof(CubeVertex, nx)));

    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(CubeVertex), reinterpret_cast<const void *>(offsetof(CubeVertex, r)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LodPocMod::logAll(LogLevel::Error, "[PocQuad: 3D] GL error 0x{:x} during cube setup", err);
        mKilled.store(true, std::memory_order_relaxed);
        return false;
    }

    mCubeInitialized = true;
    LodPocMod::logAll(LogLevel::Info, "[PocQuad: 3D] Initialized 3D cube program ({}) and VAO ({}) successfully", mCubeProgram, mCubeVao);
    return true;
}

void PocQuad::computeCubeModelOffset(float out[3]) const {
    if (mAbsoluteAnchor && mCameraTracked) {
        // The shader works in camera-relative world space.  Subtract the
        // tracked camera from the fixed absolute cube position every frame.
        out[0] = mCubeAbsolutePos[0] - mTrackedCameraPos[0];
        out[1] = mCubeAbsolutePos[1] - mTrackedCameraPos[1];
        out[2] = mCubeAbsolutePos[2] - mTrackedCameraPos[2];
        return;
    }

    // Legacy camera-relative anchor (originValid=false fallback).
    if (mHasChunkOffset) {
        const float deltaX = mAnchorChunkOffset[0] - mLastChunkTranslation[0];
        const float deltaY = mAnchorChunkOffset[1] - mLastChunkTranslation[1];
        const float deltaZ = mAnchorChunkOffset[2] - mLastChunkTranslation[2];
        out[0] = mAnchorWorldPos[0] - deltaX;
        out[1] = mAnchorWorldPos[1] - deltaY;
        out[2] = mAnchorWorldPos[2] - deltaZ;
    } else {
        out[0] = mAnchorWorldPos[0];
        out[1] = mAnchorWorldPos[1];
        out[2] = mAnchorWorldPos[2];
    }
}

bool PocQuad::initLodGL() {
    const char *vsSource =
        "#version 300 es\n"
        "layout(location = 0) in vec3 aPos;\n"
        "layout(location = 1) in vec4 aColor;\n"
        "uniform mat4 uProj;\n"
        "uniform mat4 uView;\n"
        "uniform vec3 uCameraPos;\n"
        "out vec3 vWorldPos;\n"
        "out vec3 vRelPos;\n"
        "out vec4 vColor;\n"
        "void main() {\n"
        "    vec3 relPos = aPos - uCameraPos;\n"
        "    vec4 eyePos = uView * vec4(relPos, 1.0);\n"
        "    gl_Position = uProj * eyePos;\n"
        "    vWorldPos = aPos;\n"
        "    vRelPos = relPos;\n"
        "    vColor = aColor;\n"
        "}\n";

    const char *fsSource =
        "#version 300 es\n"
        "precision mediump float;\n"
        "in vec3 vWorldPos;\n"
        "in vec3 vRelPos;\n"
        "in vec4 vColor;\n"
        "uniform float uMinLodDist;\n"
        "uniform float uMaxLodDist;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "    float distXZ = length(vRelPos.xz);\n"
        "    if (distXZ < uMinLodDist || distXZ > uMaxLodDist) {\n"
        "        discard;\n"
        "    }\n"
        "    vec3 dX = dFdx(vWorldPos);\n"
        "    vec3 dY = dFdy(vWorldPos);\n"
        "    vec3 normal = normalize(cross(dX, dY));\n"
        "    if (normal.y < 0.0) normal = -normal;\n"
        "    vec3 lightDir = normalize(vec3(0.4, 0.9, 0.3));\n"
        "    float diff = max(dot(normal, lightDir), 0.55);\n"
        "    float fogFactor = clamp((distXZ - (uMaxLodDist - 128.0)) / 128.0, 0.0, 1.0);\n"
        "    vec3 fogColor = vec3(0.70, 0.80, 0.92);\n"
        "    vec3 finalRgb = mix(vColor.rgb * diff, fogColor, fogFactor);\n"
        "    fragColor = vec4(finalRgb, 1.0);\n"
        "}\n";

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vsSource, nullptr);
    glCompileShader(vs);
    GLint status = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(vs, sizeof(log), nullptr, log);
        LodPocMod::logAll(LogLevel::Error, "[PocQuad: LOD] Failed to compile LOD VS: {}", log);
        glDeleteShader(vs);
        return false;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fsSource, nullptr);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512];
        glGetShaderInfoLog(fs, sizeof(log), nullptr, log);
        LodPocMod::logAll(LogLevel::Error, "[PocQuad: LOD] Failed to compile LOD FS: {}", log);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    mLodProgram = glCreateProgram();
    glAttachShader(mLodProgram, vs);
    glAttachShader(mLodProgram, fs);
    glLinkProgram(mLodProgram);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGetProgramiv(mLodProgram, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512];
        glGetProgramInfoLog(mLodProgram, sizeof(log), nullptr, log);
        LodPocMod::logAll(LogLevel::Error, "[PocQuad: LOD] Failed to link LOD program: {}", log);
        glDeleteProgram(mLodProgram);
        mLodProgram = 0;
        return false;
    }

    mLodLocProj = glGetUniformLocation(mLodProgram, "uProj");
    mLodLocView = glGetUniformLocation(mLodProgram, "uView");
    mLodLocCameraPos = glGetUniformLocation(mLodProgram, "uCameraPos");
    mLodLocMinDist = glGetUniformLocation(mLodProgram, "uMinLodDist");
    mLodLocMaxDist = glGetUniformLocation(mLodProgram, "uMaxLodDist");

    mLodGlInitialized = true;
    LodPocMod::logAll(LogLevel::Info,
        "[PocQuad: LOD] Initialized LOD terrain program ({}) successfully",
        mLodProgram);
    return true;
}

void PocQuad::uploadPendingLodMeshes() {
    std::vector<LodTileMesh> pending;
    {
        std::lock_guard lock(mLodMeshMutex);
        if (!mHasNewLodMeshes) return;
        pending = std::move(mPendingLodMeshes);
        mHasNewLodMeshes = false;
    }

    if (pending.empty()) return;

    for (auto &buf : mLodGlBuffers) {
        if (buf.vao != 0) glDeleteVertexArrays(1, &buf.vao);
        if (buf.vbo != 0) glDeleteBuffers(1, &buf.vbo);
        if (buf.ebo != 0) glDeleteBuffers(1, &buf.ebo);
    }
    mLodGlBuffers.clear();

    size_t totalVerts = 0;
    size_t totalIndices = 0;

    for (const auto &tile : pending) {
        if (tile.indices.empty()) continue;

        LodTileGlBuffer buf;
        glGenVertexArrays(1, &buf.vao);
        glBindVertexArray(buf.vao);

        glGenBuffers(1, &buf.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, buf.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(tile.vertices.size() * sizeof(LodVertex)),
                     tile.vertices.data(), GL_STATIC_DRAW);

        glGenBuffers(1, &buf.ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(tile.indices.size() * sizeof(uint32_t)),
                     tile.indices.data(), GL_STATIC_DRAW);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LodVertex),
                              reinterpret_cast<const void *>(offsetof(LodVertex, x)));

        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(LodVertex),
                              reinterpret_cast<const void *>(offsetof(LodVertex, r)));

        buf.indexCount = static_cast<GLsizei>(tile.indices.size());
        mLodGlBuffers.push_back(buf);

        totalVerts += tile.vertices.size();
        totalIndices += tile.indices.size();
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    LodPocMod::logAll(LogLevel::Info,
        "[PocQuad: LOD] Uploaded {} LOD terrain tiles to GPU (total verts: {}, indices: {}, memory: {:.2f} KB)",
        mLodGlBuffers.size(), totalVerts, totalIndices,
        static_cast<double>(totalVerts * sizeof(LodVertex) + totalIndices * sizeof(uint32_t)) / 1024.0);
}

void PocQuad::drawLodTerrain(GLuint targetFbo, GLenum terrainDepthFunc) {
    (void)targetFbo;
    (void)terrainDepthFunc;

    uploadPendingLodMeshes();
    if (mLodGlBuffers.empty()) return;

    if (!mLodGlInitialized) {
        if (!initLodGL()) return;
    }

    glDisable(GL_CULL_FACE);
    glUseProgram(mLodProgram);
    glUniformMatrix4fv(mLodLocProj, 1, GL_FALSE, mProjMatrix);
    glUniformMatrix4fv(mLodLocView, 1, GL_FALSE, mViewMatrix);
    glUniform3f(mLodLocCameraPos, mTrackedCameraPos[0], mTrackedCameraPos[1], mTrackedCameraPos[2]);
    glUniform1f(mLodLocMinDist, mMinLodDist.load(std::memory_order_relaxed));
    glUniform1f(mLodLocMaxDist, mMaxLodDist.load(std::memory_order_relaxed));

    for (const auto &buf : mLodGlBuffers) {
        glBindVertexArray(buf.vao);
        glDrawElements(GL_TRIANGLES, buf.indexCount, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
}

void PocQuad::drawCube(GLuint targetFbo, GLenum terrainDepthFunc) {
    if (!mInWorld.load(std::memory_order_relaxed)) return;
    if (!mEnabled.load(std::memory_order_relaxed) || mKilled.load(std::memory_order_relaxed)) return;
    if (sInsideQuadRender) return;

    sInsideQuadRender = true;

    if (!mCubeInitialized) {
        if (!initCubeGL()) {
            sInsideQuadRender = false;
            return;
        }
    }

    mLastDepthFuncUsed = (terrainDepthFunc != 0 && terrainDepthFunc != GL_NEVER) ? terrainDepthFunc : GL_LEQUAL;

    // Anchor the cube in world space if not yet anchored.
    if (!mCubeAnchored) {
        // Forward vector in world space from View matrix:
        // Eye forward is (0, 0, -1), so world forward = -Row 2 of View matrix = -(View[2], View[6], View[10])
        float fwdX = -mViewMatrix[2];
        float fwdY = -mViewMatrix[6];
        float fwdZ = -mViewMatrix[10];
        float len = std::sqrt(fwdX * fwdX + fwdY * fwdY + fwdZ * fwdZ);
        if (len > 0.001f) {
            fwdX /= len; fwdY /= len; fwdZ /= len;
        }
        const float dist = mPoc3dDist.load(std::memory_order_relaxed);

        if (mCameraTracked) {
            mAbsoluteAnchor = true;
            mCubeAbsolutePos[0] = mTrackedCameraPos[0] + fwdX * dist;
            mCubeAbsolutePos[1] = mTrackedCameraPos[1] + fwdY * dist;
            mCubeAbsolutePos[2] = mTrackedCameraPos[2] + fwdZ * dist;
            LodPocMod::logAll(LogLevel::Info,
                "[PocQuad: 3D] Anchored world cube at ({:.4f}, {:.4f}, {:.4f}) from camera=({:.4f}, {:.4f}, {:.4f}) dist={:.2f} (mode={})",
                mCubeAbsolutePos[0], mCubeAbsolutePos[1], mCubeAbsolutePos[2],
                mTrackedCameraPos[0], mTrackedCameraPos[1], mTrackedCameraPos[2], dist,
                mAutoOrigin ? "auto-relative (zero-config, eyeHeight=+1.62)" : "absolute");
        } else {
            mAbsoluteAnchor = false;
            mAnchorWorldPos[0] = fwdX * dist;
            mAnchorWorldPos[1] = fwdY * dist;
            mAnchorWorldPos[2] = fwdZ * dist;
            if (mHasChunkOffset) {
                mAnchorChunkOffset[0] = mLastChunkTranslation[0];
                mAnchorChunkOffset[1] = mLastChunkTranslation[1];
                mAnchorChunkOffset[2] = mLastChunkTranslation[2];
            } else {
                mAnchorChunkOffset[0] = 0.0f;
                mAnchorChunkOffset[1] = 0.0f;
                mAnchorChunkOffset[2] = 0.0f;
            }
            LodPocMod::logAll(LogLevel::Info,
                "[PocQuad: 3D] Anchored legacy cube at camera-relative pos=({:.4f}, {:.4f}, {:.4f}) dist={:.2f}",
                mAnchorWorldPos[0], mAnchorWorldPos[1], mAnchorWorldPos[2], dist);
            if (!mOriginValid) {
                logLegacyAnchorWarning();
            } else {
                LodPocMod::logAll(LogLevel::Warn,
                    "[PocQuad: 3D] WARNING: originValid=true but camera is not initialized; using legacy anchor until tracking is ready");
            }
        }
        mCubeAnchored = true;
    }

    float modelOffset[3];
    computeCubeModelOffset(modelOffset);

    // --- SAVE ALL TOUCHED GL STATE ---
    GLint origProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &origProgram);
    GLint origVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &origVao);
    GLint origVbo = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &origVbo);
    GLint origEbo = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &origEbo);
    GLint origActiveTexture = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &origActiveTexture);
    GLint origTex2D = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &origTex2D);

    GLint origUbos[4] = {0};
    GLint64 origUboOffsets[4] = {0};
    GLint64 origUboSizes[4] = {0};
    resolveUboFunctions();
    if (sGetIntegeri_v != nullptr && sGetInteger64i_v != nullptr) {
        for (GLuint i = 0; i < 4; ++i) {
            sGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, i, &origUbos[i]);
            sGetInteger64i_v(GL_UNIFORM_BUFFER_START, i, &origUboOffsets[i]);
            sGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, i, &origUboSizes[i]);
        }
    }

    GLint origViewport[4] = {0};
    glGetIntegerv(GL_VIEWPORT, origViewport);
    GLboolean origScissorTest = glIsEnabled(GL_SCISSOR_TEST);
    GLint origScissor[4] = {0};
    glGetIntegerv(GL_SCISSOR_BOX, origScissor);

    GLboolean origDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint origDepthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &origDepthFunc);
    GLboolean origDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &origDepthMask);
    GLfloat origDepthRange[2] = {0.0f, 1.0f};
    glGetFloatv(GL_DEPTH_RANGE, origDepthRange);

    GLboolean origBlend = glIsEnabled(GL_BLEND);
    GLint origBlendSrcRgb = GL_ONE, origBlendDstRgb = GL_ZERO;
    GLint origBlendSrcAlpha = GL_ONE, origBlendDstAlpha = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC_RGB, &origBlendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &origBlendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &origBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &origBlendDstAlpha);
    GLint origBlendEqRgb = GL_FUNC_ADD, origBlendEqAlpha = GL_FUNC_ADD;
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &origBlendEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &origBlendEqAlpha);

    GLboolean origCullFace = glIsEnabled(GL_CULL_FACE);
    GLint origCullMode = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &origCullMode);
    GLint origFrontFace = GL_CCW;
    glGetIntegerv(GL_FRONT_FACE, &origFrontFace);

    GLboolean origStencilTest = glIsEnabled(GL_STENCIL_TEST);
    GLboolean origColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetBooleanv(GL_COLOR_WRITEMASK, origColorMask);

    GLint origDrawFbo = 0, origReadFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &origDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &origReadFbo);

    // --- APPLY 3D CUBE STATE ---
    if (static_cast<GLuint>(origDrawFbo) != targetFbo) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targetFbo);
    }

    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(mLastDepthFuncUsed);
    // The cube is a solid world object: let it write depth so its own front
    // and back faces resolve against each other in the depth buffer.
    glDepthMask(GL_TRUE);
    glDepthRangef(0.0f, 1.0f);

    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // --- DRAW LOD TERRAIN MESH FIRST ---
    drawLodTerrain(targetFbo, mLastDepthFuncUsed);

    // --- DRAW 3D TEST CUBE ---
    if (mPoc3d.load(std::memory_order_relaxed)) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glUseProgram(mCubeProgram);
        glUniformMatrix4fv(mCubeLocProj, 1, GL_FALSE, mProjMatrix);
        glUniformMatrix4fv(mCubeLocView, 1, GL_FALSE, mViewMatrix);
        glUniform3f(mCubeLocModelOffset, modelOffset[0], modelOffset[1], modelOffset[2]);

        glBindVertexArray(mCubeVao);
        glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, nullptr);
    }

    // --- RESTORE ALL TOUCHED GL STATE ---
    glBindVertexArray(static_cast<GLuint>(origVao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(origVbo));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(origEbo));

    if (sBindBufferRange != nullptr && sBindBufferBase != nullptr) {
        for (GLuint i = 0; i < 4; ++i) {
            if (origUbos[i] != 0 && origUboSizes[i] > 0) {
                sBindBufferRange(GL_UNIFORM_BUFFER, i, static_cast<GLuint>(origUbos[i]),
                                 static_cast<GLintptr>(origUboOffsets[i]),
                                 static_cast<GLsizeiptr>(origUboSizes[i]));
            } else {
                sBindBufferBase(GL_UNIFORM_BUFFER, i, static_cast<GLuint>(origUbos[i]));
            }
        }
    }

    glActiveTexture(static_cast<GLenum>(origActiveTexture));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(origTex2D));
    glUseProgram(static_cast<GLuint>(origProgram));

    glViewport(origViewport[0], origViewport[1], origViewport[2], origViewport[3]);
    glScissor(origScissor[0], origScissor[1], origScissor[2], origScissor[3]);
    if (origScissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);

    if (origDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(static_cast<GLenum>(origDepthFunc));
    glDepthMask(origDepthMask);
    glDepthRangef(origDepthRange[0], origDepthRange[1]);

    glBlendFuncSeparate(static_cast<GLenum>(origBlendSrcRgb), static_cast<GLenum>(origBlendDstRgb),
                        static_cast<GLenum>(origBlendSrcAlpha), static_cast<GLenum>(origBlendDstAlpha));
    glBlendEquationSeparate(static_cast<GLenum>(origBlendEqRgb), static_cast<GLenum>(origBlendEqAlpha));
    if (origBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);

    glCullFace(static_cast<GLenum>(origCullMode));
    if (origCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glFrontFace(static_cast<GLenum>(origFrontFace));

    if (origStencilTest) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glColorMask(origColorMask[0], origColorMask[1], origColorMask[2], origColorMask[3]);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(origDrawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(origReadFbo));

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LodPocMod::logAll(LogLevel::Error, "[PocQuad: 3D] GL error 0x{:x} detected during cube render! Killing PocQuad.", err);
        mKilled.store(true, std::memory_order_relaxed);
    } else {
        mTotalDrawCount++;
    }

    sInsideQuadRender = false;
}

void PocQuad::drawQuad(GLuint targetFbo, GLenum terrainDepthFunc, bool depthInvalidated) {
    if (!mInWorld.load(std::memory_order_relaxed)) return;
    if (!mEnabled.load(std::memory_order_relaxed) || mKilled.load(std::memory_order_relaxed)) return;
    if (sInsideQuadRender) return;

    sInsideQuadRender = true;

    if (!mInitialized) {
        if (!initGL()) {
            sInsideQuadRender = false;
            return;
        }
    }

    mLastDepthFuncUsed = (terrainDepthFunc != 0 && terrainDepthFunc != GL_NEVER) ? terrainDepthFunc : GL_LEQUAL;
    mLastDepthInvalidated = depthInvalidated;

    // --- SAVE ALL TOUCHED GL STATE ---
    GLint origProgram = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &origProgram);
    GLint origVao = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &origVao);
    GLint origVbo = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &origVbo);
    GLint origEbo = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &origEbo);
    GLint origActiveTexture = GL_TEXTURE0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &origActiveTexture);
    GLint origTex2D = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &origTex2D);

    GLint origUbos[4] = {0};
    GLint64 origUboOffsets[4] = {0};
    GLint64 origUboSizes[4] = {0};
    resolveUboFunctions();
    if (sGetIntegeri_v != nullptr && sGetInteger64i_v != nullptr) {
        for (GLuint i = 0; i < 4; ++i) {
            sGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, i, &origUbos[i]);
            sGetInteger64i_v(GL_UNIFORM_BUFFER_START, i, &origUboOffsets[i]);
            sGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, i, &origUboSizes[i]);
        }
    }

    GLint origViewport[4] = {0};
    glGetIntegerv(GL_VIEWPORT, origViewport);
    GLboolean origScissorTest = glIsEnabled(GL_SCISSOR_TEST);
    GLint origScissor[4] = {0};
    glGetIntegerv(GL_SCISSOR_BOX, origScissor);

    GLboolean origDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLint origDepthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &origDepthFunc);
    GLboolean origDepthMask = GL_TRUE;
    glGetBooleanv(GL_DEPTH_WRITEMASK, &origDepthMask);
    GLfloat origDepthRange[2] = {0.0f, 1.0f};
    glGetFloatv(GL_DEPTH_RANGE, origDepthRange);

    GLboolean origBlend = glIsEnabled(GL_BLEND);
    GLint origBlendSrcRgb = GL_ONE, origBlendDstRgb = GL_ZERO;
    GLint origBlendSrcAlpha = GL_ONE, origBlendDstAlpha = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC_RGB, &origBlendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &origBlendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &origBlendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &origBlendDstAlpha);
    GLint origBlendEqRgb = GL_FUNC_ADD, origBlendEqAlpha = GL_FUNC_ADD;
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &origBlendEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &origBlendEqAlpha);

    GLboolean origCullFace = glIsEnabled(GL_CULL_FACE);
    GLint origCullMode = GL_BACK;
    glGetIntegerv(GL_CULL_FACE_MODE, &origCullMode);
    GLint origFrontFace = GL_CCW;
    glGetIntegerv(GL_FRONT_FACE, &origFrontFace);

    GLboolean origStencilTest = glIsEnabled(GL_STENCIL_TEST);
    GLboolean origColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    glGetBooleanv(GL_COLOR_WRITEMASK, origColorMask);

    GLint origDrawFbo = 0, origReadFbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &origDrawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &origReadFbo);

    // --- APPLY TEST QUAD STATE ---
    if (static_cast<GLuint>(origDrawFbo) != targetFbo) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targetFbo);
    }

    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(mLastDepthFuncUsed);
    glDepthMask(GL_FALSE);
    glDepthRangef(0.0f, 1.0f);

    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glUseProgram(mProgram);
    float depthVal = mDepth.load(std::memory_order_relaxed);
    float ndcZ = depthVal * 2.0f - 1.0f; // OpenGL NDC depth in [-1, 1]
    glUniform1f(mLocDepth, ndcZ);

    std::array<float, 4> color;
    {
        std::lock_guard lock(mConfigMutex);
        color = mColorRgba;
    }
    glUniform4f(mLocColor, color[0], color[1], color[2], color[3]);

    glBindVertexArray(mVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // --- RESTORE ALL TOUCHED GL STATE ---
    glBindVertexArray(static_cast<GLuint>(origVao));
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(origVbo));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(origEbo));

    if (sBindBufferRange != nullptr && sBindBufferBase != nullptr) {
        for (GLuint i = 0; i < 4; ++i) {
            if (origUbos[i] != 0 && origUboSizes[i] > 0) {
                sBindBufferRange(GL_UNIFORM_BUFFER, i, static_cast<GLuint>(origUbos[i]),
                                 static_cast<GLintptr>(origUboOffsets[i]),
                                 static_cast<GLsizeiptr>(origUboSizes[i]));
            } else {
                sBindBufferBase(GL_UNIFORM_BUFFER, i, static_cast<GLuint>(origUbos[i]));
            }
        }
    }

    glActiveTexture(static_cast<GLenum>(origActiveTexture));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(origTex2D));
    glUseProgram(static_cast<GLuint>(origProgram));

    glViewport(origViewport[0], origViewport[1], origViewport[2], origViewport[3]);
    glScissor(origScissor[0], origScissor[1], origScissor[2], origScissor[3]);
    if (origScissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);

    if (origDepthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(static_cast<GLenum>(origDepthFunc));
    glDepthMask(origDepthMask);
    glDepthRangef(origDepthRange[0], origDepthRange[1]);

    glBlendFuncSeparate(static_cast<GLenum>(origBlendSrcRgb), static_cast<GLenum>(origBlendDstRgb),
                        static_cast<GLenum>(origBlendSrcAlpha), static_cast<GLenum>(origBlendDstAlpha));
    glBlendEquationSeparate(static_cast<GLenum>(origBlendEqRgb), static_cast<GLenum>(origBlendEqAlpha));
    if (origBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);

    glCullFace(static_cast<GLenum>(origCullMode));
    if (origCullFace) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glFrontFace(static_cast<GLenum>(origFrontFace));

    if (origStencilTest) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glColorMask(origColorMask[0], origColorMask[1], origColorMask[2], origColorMask[3]);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(origDrawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(origReadFbo));

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        LodPocMod::logAll(LogLevel::Error, "[PocQuad] GL error 0x{:x} detected during quad render! Killing PocQuad.", err);
        mKilled.store(true, std::memory_order_relaxed);
    } else {
        mTotalDrawCount++;
    }

    sInsideQuadRender = false;
}

void PocQuad::onBindFramebufferBefore(GLuint newFbo, GLuint currentBoundFbo, uint64_t frameDraws, GLenum terrainDepthFunc, bool depthInvalidated) {
    if (!mInWorld.load(std::memory_order_relaxed)) return;
    if (!isEnabled()) return;
    if (mDrawnThisFrame.load(std::memory_order_relaxed)) return;

    std::string trigger;
    {
        std::lock_guard lock(mConfigMutex);
        trigger = mTrigger;
    }
    if (trigger != "before_fbo1") return;

    // World-pass trigger.  The >30 threshold matches the world detector; the
    // in-world gate is the primary protection against drawing in menus.
    if (currentBoundFbo == 0 && newFbo != 0 && frameDraws > 30) {
        if (mPoc3d.load(std::memory_order_relaxed) && mHasProjMatrix && mHasViewMatrix) {
            drawCube(0, terrainDepthFunc);
        } else {
            drawQuad(0, terrainDepthFunc, depthInvalidated);
        }
        mDrawnThisFrame.store(true, std::memory_order_relaxed);
    }
}

void PocQuad::onClearAfter(GLbitfield mask, GLuint currentBoundFbo, uint64_t frameDraws, GLenum terrainDepthFunc) {
    if (!mInWorld.load(std::memory_order_relaxed)) return;
    if (!isEnabled()) return;
    if (mDrawnThisFrame.load(std::memory_order_relaxed)) return;

    std::string trigger;
    {
        std::lock_guard lock(mConfigMutex);
        trigger = mTrigger;
    }
    if (trigger != "after_depth_clear") return;

    // Trigger condition:
    // Depth clear on FBO 0 and in-world (draw counter > 30)
    if ((mask & GL_DEPTH_BUFFER_BIT) && currentBoundFbo == 0 && frameDraws > 30) {
        if (mPoc3d.load(std::memory_order_relaxed) && mHasProjMatrix && mHasViewMatrix) {
            drawCube(0, terrainDepthFunc);
        } else {
            drawQuad(0, terrainDepthFunc, false);
        }
        mDrawnThisFrame.store(true, std::memory_order_relaxed);
    }
}

void PocQuad::onWorldFrame(bool inWorld, double currentFps, uint64_t frameCount) {
    mDrawnThisFrame.store(false, std::memory_order_relaxed);

    const auto now = std::chrono::steady_clock::now();
    const bool wasInWorld = mInWorld.exchange(inWorld, std::memory_order_relaxed);

    // Apply config reloads on the render thread, even while outside a world.
    applyPendingOrigin();

    if (!inWorld) {
        if (wasInWorld) {
            resetAnchor("left world");
            mTrackerPrimed = false;
            mPreviousTranslations.clear();
        }
        return;
    }

    if (!wasInWorld) {
        resetAnchor("entered world");
        mTrackerPrimed = false;
        mPreviousTranslations.clear();
        mLastStatsLogTime = now;
        mLastTelemetryTime = now;
        mLastCameraLogTime = now;

        if (mOriginValid && !mCameraTracked) {
            mCameraTracked = true;
            mTrackedCameraPos[0] = mOriginX;
            mTrackedCameraPos[1] = mOriginY + 1.620f;
            mTrackedCameraPos[2] = mOriginZ;
            mSignDetected = false;
            mTranslationSign = -1.0f;
            mAutoOrigin = false;
        } else if (!mCameraTracked) {
            mCameraTracked = true;
            mTrackedCameraPos[0] = 0.0f;
            mTrackedCameraPos[1] = 1.620f;
            mTrackedCameraPos[2] = 0.0f;
            mSignDetected = false;
            mTranslationSign = -1.0f;
            mAutoOrigin = true;
        }

        LodPocMod::logAll(LogLevel::Info,
            "[PocQuad] world entry at frame {}: originValid={}, autoOrigin={}, cameraTracked={}, currentTranslations={}, perspective={}",
            frameCount,
            mOriginValid ? "true" : "false",
            mAutoOrigin ? "true" : "false",
            mCameraTracked ? "true" : "false",
            mCurrentTranslations.size(),
            mCurrentHasPerspective ? "yes" : "no");

        if (mCameraTracked) {
            logCamera(frameCount, true);
        }
    }

    // Required chunk-translation statistics every 5 seconds while in world.
    logChunkStats(frameCount);

    // Camera-origin tracking is independent of whether the test quad/cube is
    // enabled, but only runs where the world detector says we are in world.
    updateCameraTracking(frameCount);
    logCamera(frameCount, false);

    if (mEnabled.load(std::memory_order_relaxed) && !mKilled.load(std::memory_order_relaxed) &&
        now - mLastTelemetryTime >= std::chrono::seconds(1)) {
        mLastTelemetryTime = now;
        if (mPoc3d.load(std::memory_order_relaxed) && mHasProjMatrix && mHasViewMatrix) {
            float offset[3];
            computeCubeModelOffset(offset);
            float fwdX = -mViewMatrix[2];
            float fwdY = -mViewMatrix[6];
            float fwdZ = -mViewMatrix[10];
            LodPocMod::logAll(LogLevel::Info,
                "[PocQuad: 3D] Telemetry: draws={}, anchored={}, absoluteAnchor={}, originMode={}, camera=({:.2f},{:.2f},{:.2f}), fwd=({:.2f},{:.2f},{:.2f}), cubeOffset=({:.2f},{:.2f},{:.2f}), fps={:.1f}{}",
                mTotalDrawCount,
                mCubeAnchored ? "true" : "false",
                mAbsoluteAnchor ? "true" : "false",
                mAutoOrigin ? "auto-relative (zero-config, eyeHeight=+1.62)" : "absolute",
                mTrackedCameraPos[0], mTrackedCameraPos[1], mTrackedCameraPos[2],
                fwdX, fwdY, fwdZ,
                offset[0], offset[1], offset[2],
                currentFps,
                mTotalDrawCount == 0 ? " (WAITING FOR TRIGGER)" : "");
        } else {
            float depth = mDepth.load(std::memory_order_relaxed);
            float ndcZ = depth * 2.0f - 1.0f;
            LodPocMod::logAll(LogLevel::Info,
                "[PocQuad: 2D] Telemetry: draws={}, depth={:.3f} (NDC z={:.3f}), func={}, invalidated={}, fps={:.1f}{}",
                mTotalDrawCount, depth, ndcZ,
                depthFuncToString(mLastDepthFuncUsed),
                mLastDepthInvalidated ? "TRUE (WARNING: depth buffer was invalidated before draw!)" : "false",
                currentFps,
                mTotalDrawCount == 0 ? " (WAITING FOR TRIGGER)" : "");
        }
    }
}

void PocQuad::cleanup() {
    if (mLodProgram != 0) {
        glDeleteProgram(mLodProgram);
        mLodProgram = 0;
    }
    for (auto &buf : mLodGlBuffers) {
        if (buf.vao != 0) glDeleteVertexArrays(1, &buf.vao);
        if (buf.vbo != 0) glDeleteBuffers(1, &buf.vbo);
        if (buf.ebo != 0) glDeleteBuffers(1, &buf.ebo);
    }
    mLodGlBuffers.clear();
    mLodGlInitialized = false;

    if (mCubeInitialized) {
        if (mCubeEbo != 0) {
            glDeleteBuffers(1, &mCubeEbo);
            mCubeEbo = 0;
        }
        if (mCubeVbo != 0) {
            glDeleteBuffers(1, &mCubeVbo);
            mCubeVbo = 0;
        }
        if (mCubeVao != 0) {
            glDeleteVertexArrays(1, &mCubeVao);
            mCubeVao = 0;
        }
        if (mCubeProgram != 0) {
            glDeleteProgram(mCubeProgram);
            mCubeProgram = 0;
        }
        mCubeInitialized = false;
    }
    if (mInitialized) {
        if (mVbo != 0) {
            glDeleteBuffers(1, &mVbo);
            mVbo = 0;
        }
        if (mVao != 0) {
            glDeleteVertexArrays(1, &mVao);
            mVao = 0;
        }
        if (mProgram != 0) {
            glDeleteProgram(mProgram);
            mProgram = 0;
        }
        mInitialized = false;
    }

    mInWorld.store(false, std::memory_order_relaxed);
    mCubeAnchored = false;
    mAbsoluteAnchor = false;
    mCameraTracked = false;
    mTrackerPrimed = false;
    mOriginValid = false;
    mCaptureTranslations.clear();
    mCaptureTranslationKeys.clear();
    mCaptureHasPerspective = false;
    mCurrentTranslations.clear();
    mPreviousTranslations.clear();

    // Allow the next setOriginConfig() (including one with identical values)
    // to be applied after disable/re-enable, when the singleton is reused.
    {
        std::lock_guard lock(mOriginMutex);
        mPendingOrigin = OriginRequest{};
        mOriginRequestInitialized = false;
        mOriginGenerationCounter = 0;
        mAppliedOriginGeneration = 0;
    }
}

} // namespace lod_poc
