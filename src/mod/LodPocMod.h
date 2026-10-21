#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

#include "mod/Config.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <pl/Mod.hpp>
#include <pl/memory/Hook.hpp>

namespace lod_poc {

struct Vec3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

/**
 * @brief LOD Proof-of-Concept Diagnostic Mod for LeviLaunchroid.
 *
 * Runs 5 non-intrusive, log-only probes to verify hook points, graphics backend,
 * and chunk/camera pipeline states for distant terrain rendering.
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

  private:
    using ChunkLightingTaskFn = void (*)(void *context);
    using BindFramebufferFn = void (*)(GLenum target, GLuint framebuffer);
    using EglSwapBuffersFn = EGLBoolean (*)(EGLDisplay dpy, EGLSurface surf);
    using CullPointOrChunkFn = void *(*)(void *thisPtr, void *a1, void *a2, void *a3);

    // Lifecycle & Config
    ll::mod::NativeMod &mSelf;
    mutable std::mutex mConfigMutex;
    std::optional<pl::config::ConfigFile<ModConfig>> mConfigFile;

    // Probe 1: ChunkSource::_lightingTask
    pl::memory::HookHandle mLightingHook;
    ChunkLightingTaskFn mOriginalLightingTask{nullptr};
    std::atomic<uint64_t> mLightingCalls{0};
    std::atomic<uint64_t> mLastLightingCalls{0};
    std::chrono::steady_clock::time_point mLastLightingReport{};
    std::mutex mLightingMutex;

    // Probe 2: glBindFramebuffer
    pl::memory::HookHandle mBindFramebufferHook;
    BindFramebufferFn mOriginalBindFramebuffer{nullptr};
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
    static void lightingTaskDetour(void *context);
    static void bindFramebufferDetour(GLenum target, GLuint framebuffer);
    static EGLBoolean swapBuffersDetour(EGLDisplay dpy, EGLSurface surf);
    static void *cullPointOrChunkDetour(void *thisPtr, void *a1, void *a2, void *a3);
};

} // namespace lod_poc
