#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

#include "mod/CameraProbe.h"
#include "mod/Config.h"
#include "mod/FileLog.h"
#include "mod/GlTrace.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <fmt/format.h>
#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

namespace lod_poc {

enum class LogLevel {
    Info,
    Warn,
    Error
};

struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

/**
 * @brief LOD Proof-of-Concept Diagnostic Mod for LeviLaunchroid.
 *
 * Runs non-intrusive, log-only probes to verify hook points, graphics backend,
 * camera coordinates, and LevelDB world/mesh state.
 */
class LodPocMod {
  public:
    static LodPocMod &instance();

    LodPocMod();

    [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

    void onBindFramebuffer(GLenum target, GLuint framebuffer);
    void checkConfigReload();

    template <typename... Args>
    static void logAll(LogLevel level, fmt::format_string<Args...> fmtStr, Args &&...args) {
        try {
            std::string msg = fmt::format(fmtStr, std::forward<Args>(args)...);
            char levelChar = 'I';
            auto &logger = LodPocMod::instance().getSelf().getLogger();
            switch (level) {
                case LogLevel::Info:
                    logger.info("{}", msg);
                    levelChar = 'I';
                    break;
                case LogLevel::Warn:
                    logger.warn("{}", msg);
                    levelChar = 'W';
                    break;
                case LogLevel::Error:
                    logger.error("{}", msg);
                    levelChar = 'E';
                    break;
            }
            FileLog::instance().write(levelChar, msg);
        } catch (...) {}
    }

  private:
    using ChunkLightingTaskFn = void (*)(void *a0, void *a1, void *a2, void *a3,
                                         void *a4, void *a5, void *a6, void *a7);
    using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay dpy, EGLSurface surf);
    using CullPointOrChunkFn = void *(*)(void *a0, void *a1, void *a2, void *a3,
                                         void *a4, void *a5, void *a6, void *a7);

    // Lifecycle & Config
    ll::mod::NativeMod &mSelf;
    mutable std::mutex mConfigMutex;
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile;
    std::chrono::steady_clock::time_point mLastConfigCheck{};

    // Probe 1: ChunkSource::_lightingTask
    pl::memory::HookHandle mLightingHook;
    ChunkLightingTaskFn mOriginalLightingTask{nullptr};
    std::atomic<uint64_t> mLightingCalls{0};
    std::atomic<uint64_t> mLastLightingCalls{0};
    std::chrono::steady_clock::time_point mLastLightingReport{};
    std::mutex mLightingMutex;

    // Probe 2: glBindFramebuffer tracking
    std::atomic<uint64_t> mBindCalls{0};
    static constexpr size_t kMaxFrameBinds = 64;
    std::array<GLuint, kMaxFrameBinds> mFrameBinds{};
    std::atomic<size_t> mFrameBindCount{0};

    // Probe 3: eglSwapBuffers
    pl::memory::HookHandle mSwapBuffersHook;
    EglSwapBuffersFn mOriginalSwapBuffers{nullptr};
    std::atomic<uint64_t> mFrameCount{0};
    std::atomic<uint64_t> mLastFpsFrame{0};
    std::chrono::steady_clock::time_point mLastFpsReport{};
    std::mutex mFpsMutex;

    // Probe 5: LevelRendererCamera::cullPointOrChunk
    pl::memory::HookHandle mCullHook;
    CullPointOrChunkFn mOriginalCull{nullptr};
    std::atomic<uint64_t> mCullCalls{0};
    std::atomic<uint64_t> mLastCullCalls{0};
    std::chrono::steady_clock::time_point mLastCullReport{};
    std::mutex mCullMutex;

    void installProbes();
    void removeProbes();

    // Detours
    static void lightingTaskDetour(void *a0, void *a1, void *a2, void *a3,
                                   void *a4, void *a5, void *a6, void *a7);
    static EGLBoolean swapBuffersDetour(EGLDisplay dpy, EGLSurface surf);
    static void *cullPointOrChunkDetour(void *a0, void *a1, void *a2, void *a3,
                                        void *a4, void *a5, void *a6, void *a7);
};

} // namespace lod_poc
