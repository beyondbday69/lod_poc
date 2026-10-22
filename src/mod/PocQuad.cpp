#include "mod/PocQuad.h"
#include "mod/LodPocMod.h"

#include <dlfcn.h>
#include <algorithm>
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

} // namespace

PocQuad &PocQuad::instance() {
    static PocQuad inst;
    return inst;
}

bool PocQuad::isRendering() {
    return sInsideQuadRender;
}

void PocQuad::setConfig(bool enabled, float depth, const std::string &colorHex, const std::string &trigger) {
    bool wasEnabled = mEnabled.load(std::memory_order_relaxed);
    mEnabled.store(enabled, std::memory_order_relaxed);
    mDepth.store(std::clamp(depth, 0.0f, 1.0f), std::memory_order_relaxed);

    {
        std::lock_guard lock(mConfigMutex);
        mColorHex = colorHex;
        mColorRgba = parseHexColor(colorHex);
        mTrigger = trigger;
    }

    if (!wasEnabled && enabled) {
        mKilled.store(false, std::memory_order_relaxed);
        LodPocMod::logAll(LogLevel::Info,
            "[PocQuad] Enabled: depth={:.3f}, color='#{}' (RGBA: [{:.2f},{:.2f},{:.2f},{:.2f}]), trigger='{}'",
            depth, colorHex, mColorRgba[0], mColorRgba[1], mColorRgba[2], mColorRgba[3], trigger);
    }
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
        LodPocMod::logAll(LogLevel::Error, "[PocQuad] Failed to compile vertex shader: {}", log);
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
        LodPocMod::logAll(LogLevel::Error, "[PocQuad] Failed to compile fragment shader: {}", log);
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
        LodPocMod::logAll(LogLevel::Error, "[PocQuad] Failed to link shader program: {}", log);
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
        LodPocMod::logAll(LogLevel::Error, "[PocQuad] GL error 0x{:x} during setup", err);
        mKilled.store(true, std::memory_order_relaxed);
        return false;
    }

    mInitialized = true;
    LodPocMod::logAll(LogLevel::Info, "[PocQuad] Initialized shader (prog={}) and VAO ({}) successfully", mProgram, mVao);
    return true;
}

void PocQuad::drawQuad(GLuint targetFbo, GLenum terrainDepthFunc, bool depthInvalidated) {
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
    if (!isEnabled()) return;
    if (mDrawnThisFrame.load(std::memory_order_relaxed)) return;

    std::string trigger;
    {
        std::lock_guard lock(mConfigMutex);
        trigger = mTrigger;
    }
    if (trigger != "before_fbo1") return;

    // Trigger condition:
    // Bound FBO is 0, new FBO is non-zero, frame draws >= 30 (world pass complete across all render distances)
    if (currentBoundFbo == 0 && newFbo != 0 && frameDraws >= 30) {
        drawQuad(0, terrainDepthFunc, depthInvalidated);
        mDrawnThisFrame.store(true, std::memory_order_relaxed);
    }
}

void PocQuad::onClearAfter(GLbitfield mask, GLuint currentBoundFbo, uint64_t frameDraws, GLenum terrainDepthFunc) {
    if (!isEnabled()) return;
    if (mDrawnThisFrame.load(std::memory_order_relaxed)) return;

    std::string trigger;
    {
        std::lock_guard lock(mConfigMutex);
        trigger = mTrigger;
    }
    if (trigger != "after_depth_clear") return;

    // Trigger condition:
    // Depth clear on FBO 0 and in-world (draw counter > 0)
    if ((mask & GL_DEPTH_BUFFER_BIT) && currentBoundFbo == 0 && frameDraws > 0) {
        drawQuad(0, terrainDepthFunc, false);
        mDrawnThisFrame.store(true, std::memory_order_relaxed);
    }
}

void PocQuad::onSwapBuffers(double currentFps) {
    mDrawnThisFrame.store(false, std::memory_order_relaxed);

    if (mEnabled.load(std::memory_order_relaxed) && !mKilled.load(std::memory_order_relaxed)) {
        auto now = std::chrono::steady_clock::now();
        if (now - mLastTelemetryTime >= std::chrono::seconds(1)) {
            mLastTelemetryTime = now;
            float depth = mDepth.load(std::memory_order_relaxed);
            float ndcZ = depth * 2.0f - 1.0f;
            LodPocMod::logAll(LogLevel::Info,
                "[PocQuad] Telemetry: draws={}, depth={:.3f} (NDC z={:.3f}), func={}, invalidated={}, fps={:.1f}{}",
                mTotalDrawCount, depth, ndcZ,
                depthFuncToString(mLastDepthFuncUsed),
                mLastDepthInvalidated ? "TRUE (WARNING: depth buffer was invalidated before draw!)" : "false",
                currentFps,
                mTotalDrawCount == 0 ? " (WAITING FOR TRIGGER)" : "");
        }
    }
}

void PocQuad::cleanup() {
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
}

} // namespace lod_poc
