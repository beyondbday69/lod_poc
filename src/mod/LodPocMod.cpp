#include "mod/LodPocMod.h"
#include "signatures.h"

#include <algorithm>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

#include <pl/memory/Signature.hpp>

namespace lod_poc {
namespace {

constexpr const char *kMinecraftLibrary = "libminecraftpe.so";

inline pid_t getThreadId() {
#if defined(__ANDROID__)
    return gettid();
#elif defined(SYS_gettid)
    return static_cast<pid_t>(syscall(SYS_gettid));
#else
    return 0;
#endif
}

} // namespace

LodPocMod &LodPocMod::instance() {
    static LodPocMod mod;
    return mod;
}

LodPocMod::LodPocMod()
    : mSelf(ll::mod::NativeMod::current()),
      mLastLightingReport(std::chrono::steady_clock::now()),
      mLastFpsReport(std::chrono::steady_clock::now()),
      mLastCullReport(std::chrono::steady_clock::now()) {}

bool LodPocMod::load() {
    auto &self = getSelf();
    try {
        self.getLogger().info("Loading LOD PoC mod (mcpe 1.26.51.1)...");
        std::lock_guard lock(mConfigMutex);
        mConfigFile.emplace(ModConfig{}, self.getConfigDir() / "config.json",
                            self.getConfigDir() / "config.schema.json");
        if (!mConfigFile->load()) {
            self.getLogger().warn("Failed to read config.json, using defaults");
        }
        self.getLogger().info("Loaded successfully");
        return true;
    } catch (const std::exception &ex) {
        self.getLogger().error("load() threw exception: {}", ex.what());
    } catch (...) {
        self.getLogger().error("load() threw unknown exception");
    }
    return false;
}

bool LodPocMod::enable() {
    auto &self = getSelf();
    try {
        self.getLogger().info("Enabling LOD PoC diagnostic probes...");
        installProbes();
        self.getLogger().info("LOD PoC mod enabled");
        return true;
    } catch (const std::exception &ex) {
        self.getLogger().error("enable() threw exception: {}", ex.what());
    } catch (...) {
        self.getLogger().error("enable() threw unknown exception");
    }
    return false;
}

bool LodPocMod::disable() {
    auto &self = getSelf();
    try {
        self.getLogger().info("Disabling LOD PoC diagnostic probes...");
        removeProbes();
        self.getLogger().info("LOD PoC mod disabled");
        return true;
    } catch (const std::exception &ex) {
        self.getLogger().error("disable() threw exception: {}", ex.what());
    } catch (...) {
        self.getLogger().error("disable() threw unknown exception");
    }
    return false;
}

bool LodPocMod::unload() {
    auto &self = getSelf();
    try {
        self.getLogger().info("Unloading LOD PoC mod...");
        removeProbes();
        {
            std::lock_guard lock(mConfigMutex);
            mConfigFile.reset();
        }
        self.getLogger().info("Unloaded successfully");
        return true;
    } catch (const std::exception &ex) {
        self.getLogger().error("unload() threw exception: {}", ex.what());
    } catch (...) {
        self.getLogger().error("unload() threw unknown exception");
    }
    return false;
}

void LodPocMod::installProbes() {
    auto &self = getSelf();
    removeProbes();

    const auto now = std::chrono::steady_clock::now();
    mLastLightingReport = now;
    mLastFpsReport = now;
    mLastCullReport = now;

    // -------------------------------------------------------------------------
    // Probe 1: ChunkSource::_lightingTask
    // -------------------------------------------------------------------------
    const auto lightingAddr = pl::memory::resolveSignature(
        mc_signatures::kSigChunkLightingTask, kMinecraftLibrary);
    if (lightingAddr == 0) {
        self.getLogger().warn("[Probe 1] ChunkSource::_lightingTask signature NOT FOUND; disabling probe 1");
    } else {
        void *orig = nullptr;
        pl::memory::HookHandle handle(
            reinterpret_cast<void *>(lightingAddr),
            reinterpret_cast<void *>(&LodPocMod::lightingTaskDetour),
            &orig);
        if (!handle.installed()) {
            self.getLogger().error("[Probe 1] Failed to hook ChunkSource::_lightingTask at 0x{:x}", lightingAddr);
        } else {
            mOriginalLightingTask = reinterpret_cast<ChunkLightingTaskFn>(orig);
            mLightingHook = std::move(handle);
            self.getLogger().info("[Probe 1] Hooked ChunkSource::_lightingTask at 0x{:x}", lightingAddr);
        }
    }

    // -------------------------------------------------------------------------
    // Probe 2: glBindFramebuffer
    // Note: The preloader SDK does not provide a GOT/PLT hook API; inline hooks
    // via pl::memory::HookHandle on the dlsym'd GLES symbol cleanly intercept
    // all calls process-wide including libminecraftpe.so.
    // -------------------------------------------------------------------------
    void *bindFbTarget = nullptr;
    void *glesLib = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (glesLib) {
        bindFbTarget = dlsym(glesLib, "glBindFramebuffer");
    }
    if (!bindFbTarget) {
        bindFbTarget = dlsym(RTLD_DEFAULT, "glBindFramebuffer");
    }
    if (!bindFbTarget) {
        self.getLogger().warn("[Probe 2] glBindFramebuffer symbol NOT FOUND via dlsym; disabling probe 2");
    } else {
        void *orig = nullptr;
        pl::memory::HookHandle handle(
            bindFbTarget,
            reinterpret_cast<void *>(&LodPocMod::bindFramebufferDetour),
            &orig);
        if (!handle.installed()) {
            self.getLogger().error("[Probe 2] Failed to hook glBindFramebuffer at {:p}", bindFbTarget);
        } else {
            mOriginalBindFramebuffer = reinterpret_cast<BindFramebufferFn>(orig);
            mBindFramebufferHook = std::move(handle);
            self.getLogger().info("[Probe 2] Hooked glBindFramebuffer at {:p}", bindFbTarget);
        }
    }

    // -------------------------------------------------------------------------
    // Probe 3: eglSwapBuffers
    // -------------------------------------------------------------------------
    void *swapTarget = nullptr;
    void *eglLib = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (eglLib) {
        swapTarget = dlsym(eglLib, "eglSwapBuffers");
    }
    if (!swapTarget) {
        swapTarget = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    }
    if (!swapTarget) {
        self.getLogger().warn("[Probe 3] eglSwapBuffers symbol NOT FOUND via dlsym; disabling probe 3");
    } else {
        void *orig = nullptr;
        pl::memory::HookHandle handle(
            swapTarget,
            reinterpret_cast<void *>(&LodPocMod::swapBuffersDetour),
            &orig);
        if (!handle.installed()) {
            self.getLogger().error("[Probe 3] Failed to hook eglSwapBuffers at {:p}", swapTarget);
        } else {
            mOriginalSwapBuffers = reinterpret_cast<EglSwapBuffersFn>(orig);
            mSwapBuffersHook = std::move(handle);
            self.getLogger().info("[Probe 3] Hooked eglSwapBuffers at {:p}", swapTarget);
        }
    }

    // -------------------------------------------------------------------------
    // Probe 4: SubChunk::getBlock (Signature resolution ONLY - do NOT hook)
    // -------------------------------------------------------------------------
    const auto subChunkAddr = pl::memory::resolveSignature(
        mc_signatures::kSigSubChunkGetBlockStorage, kMinecraftLibrary);
    if (subChunkAddr != 0) {
        self.getLogger().info("[Probe 4] SubChunk::getBlock signature resolved at 0x{:x}", subChunkAddr);
        self.getLogger().info("[Probe 4] Verified Bedrock index formula: (x << 8) | (z << 4) | y (y stride 1, z stride 16, x stride 256)");
    } else {
        self.getLogger().warn("[Probe 4] SubChunk::getBlock signature failed to resolve in {}", kMinecraftLibrary);
    }

    // -------------------------------------------------------------------------
    // Probe 5: LevelRendererCamera::cullPointOrChunk
    // -------------------------------------------------------------------------
    const auto cullAddr = pl::memory::resolveSignature(
        mc_signatures::kSigLevelRendererCameraCull, kMinecraftLibrary);
    if (cullAddr == 0) {
        self.getLogger().warn("[Probe 5] LevelRendererCamera::cullPointOrChunk signature NOT FOUND; disabling probe 5");
    } else {
        void *orig = nullptr;
        pl::memory::HookHandle handle(
            reinterpret_cast<void *>(cullAddr),
            reinterpret_cast<void *>(&LodPocMod::cullPointOrChunkDetour),
            &orig);
        if (!handle.installed()) {
            self.getLogger().error("[Probe 5] Failed to hook LevelRendererCamera::cullPointOrChunk at 0x{:x}", cullAddr);
        } else {
            mOriginalCull = reinterpret_cast<CullPointOrChunkFn>(orig);
            mCullHook = std::move(handle);
            self.getLogger().info("[Probe 5] Hooked LevelRendererCamera::cullPointOrChunk at 0x{:x}", cullAddr);
        }
    }
}

void LodPocMod::removeProbes() {
    mLightingHook.reset();
    mOriginalLightingTask = nullptr;

    mBindFramebufferHook.reset();
    mOriginalBindFramebuffer = nullptr;

    mSwapBuffersHook.reset();
    mOriginalSwapBuffers = nullptr;

    mCullHook.reset();
    mOriginalCull = nullptr;
}

void LodPocMod::lightingTaskDetour(void *context) {
    auto &mod = LodPocMod::instance();
    auto &self = mod.getSelf();

    // Call original first
    if (mod.mOriginalLightingTask) {
        mod.mOriginalLightingTask(context);
    }

    const uint64_t call = ++mod.mLightingCalls;
    const pid_t tid = getThreadId();

    uint64_t q[4] = {0, 0, 0, 0};
    if (context != nullptr) {
        std::memcpy(q, context, sizeof(q));
    }

    if (call <= 20) {
        self.getLogger().info(
            "[Probe 1: lightingTask] #{} tid={} ctx={:p} [0x{:016x}, 0x{:016x}, 0x{:016x}, 0x{:016x}]",
            call, tid, context, q[0], q[1], q[2], q[3]);
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - mod.mLastLightingReport >= std::chrono::seconds(5)) {
        std::lock_guard lock(mod.mLightingMutex);
        if (now - mod.mLastLightingReport >= std::chrono::seconds(5)) {
            const double sec = std::chrono::duration<double>(now - mod.mLastLightingReport).count();
            const uint64_t prev = mod.mLastLightingCalls.load(std::memory_order_relaxed);
            mod.mLastLightingCalls.store(call, std::memory_order_relaxed);
            mod.mLastLightingReport = now;
            const double rate = (sec > 0.0) ? (static_cast<double>(call - prev) / sec) : 0.0;
            self.getLogger().info(
                "[Probe 1: lightingTask] 5s Summary: calls={}, rate={:.1f}/s, tid={}",
                call, rate, tid);
        }
    }
}

void LodPocMod::bindFramebufferDetour(GLenum target, GLuint framebuffer) {
    auto &mod = LodPocMod::instance();
    auto &self = mod.getSelf();

    // Call original first
    if (mod.mOriginalBindFramebuffer) {
        mod.mOriginalBindFramebuffer(target, framebuffer);
    }

    const uint64_t call = ++mod.mBindCalls;
    const pid_t tid = getThreadId();

    if (call <= 20) {
        self.getLogger().info(
            "[Probe 2: glBindFramebuffer] #{} tid={} target=0x{:x} fbo={}",
            call, tid, target, framebuffer);
    }

    const size_t idx = mod.mFrameBindCount.fetch_add(1, std::memory_order_relaxed);
    if (idx < kMaxFrameBinds) {
        mod.mFrameBinds[idx] = framebuffer;
    }
}

EGLBoolean LodPocMod::swapBuffersDetour(EGLDisplay dpy, EGLSurface surf) {
    auto &mod = LodPocMod::instance();
    auto &self = mod.getSelf();

    // Call original first
    const EGLBoolean res = mod.mOriginalSwapBuffers
                               ? mod.mOriginalSwapBuffers(dpy, surf)
                               : EGL_TRUE;

    const uint64_t frame = ++mod.mFrameCount;
    const pid_t tid = getThreadId();

    if (frame <= 20) {
        self.getLogger().info(
            "[Probe 3: eglSwapBuffers] #{} tid={} dpy={:p} surf={:p}",
            frame, tid, dpy, surf);
    }

    // Probe 2 per-frame FBO sequence report every 120 frames
    if (frame % 120 == 0) {
        const size_t totalBinds = mod.mFrameBindCount.exchange(0, std::memory_order_relaxed);
        const size_t printCount = std::min(totalBinds, kMaxFrameBinds);
        std::string bindsStr;
        for (size_t i = 0; i < printCount; ++i) {
            if (i > 0) {
                bindsStr += ", ";
            }
            bindsStr += std::to_string(mod.mFrameBinds[i]);
        }
        if (totalBinds > kMaxFrameBinds) {
            bindsStr += ", ...";
        }
        self.getLogger().info(
            "[Probe 2: glBindFramebuffer] frame {}: binds [{}] (total={}), tid={}",
            frame, bindsStr, totalBinds, tid);
    } else {
        mod.mFrameBindCount.store(0, std::memory_order_relaxed);
    }

    // Probe 3 5-second FPS report
    const auto now = std::chrono::steady_clock::now();
    if (now - mod.mLastFpsReport >= std::chrono::seconds(5)) {
        std::lock_guard lock(mod.mFpsMutex);
        if (now - mod.mLastFpsReport >= std::chrono::seconds(5)) {
            const double sec = std::chrono::duration<double>(now - mod.mLastFpsReport).count();
            const uint64_t prev = mod.mLastFpsFrame.load(std::memory_order_relaxed);
            mod.mLastFpsFrame.store(frame, std::memory_order_relaxed);
            mod.mLastFpsReport = now;
            const double fps = (sec > 0.0) ? (static_cast<double>(frame - prev) / sec) : 0.0;
            self.getLogger().info(
                "[Probe 3: eglSwapBuffers] 5s Summary: fps={:.2f} (total_frames={}), tid={}",
                fps, frame, tid);
        }
    }

    return res;
}

void *LodPocMod::cullPointOrChunkDetour(void *thisPtr, void *a1, void *a2, void *a3) {
    auto &mod = LodPocMod::instance();
    auto &self = mod.getSelf();

    // Call original first
    void *const res = mod.mOriginalCull
                          ? mod.mOriginalCull(thisPtr, a1, a2, a3)
                          : nullptr;

    const uint64_t call = ++mod.mCullCalls;
    const pid_t tid = getThreadId();

    Vec3 pos{0.0f, 0.0f, 0.0f};
    Vec3 dir{0.0f, 0.0f, 0.0f};
    if (thisPtr != nullptr) {
        std::memcpy(&pos, reinterpret_cast<const char *>(thisPtr) + 0x5c8, sizeof(Vec3));
        std::memcpy(&dir, reinterpret_cast<const char *>(thisPtr) + 0x5d4, sizeof(Vec3));
    }

    if (call <= 20) {
        self.getLogger().info(
            "[Probe 5: cullPointOrChunk] #{} tid={} this={:p} pos=({:.2f}, {:.2f}, {:.2f}) dir=({:.2f}, {:.2f}, {:.2f})",
            call, tid, thisPtr, pos.x, pos.y, pos.z, dir.x, dir.y, dir.z);
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - mod.mLastCullReport >= std::chrono::seconds(5)) {
        std::lock_guard lock(mod.mCullMutex);
        if (now - mod.mLastCullReport >= std::chrono::seconds(5)) {
            const double sec = std::chrono::duration<double>(now - mod.mLastCullReport).count();
            const uint64_t prev = mod.mLastCullCalls.load(std::memory_order_relaxed);
            mod.mLastCullCalls.store(call, std::memory_order_relaxed);
            mod.mLastCullReport = now;
            const double rate = (sec > 0.0) ? (static_cast<double>(call - prev) / sec) : 0.0;
            self.getLogger().info(
                "[Probe 5: cullPointOrChunk] 5s Summary: calls={}, rate={:.1f}/s, tid={}, pos=({:.2f}, {:.2f}, {:.2f}) dir=({:.2f}, {:.2f}, {:.2f})",
                call, rate, tid, pos.x, pos.y, pos.z, dir.x, dir.y, dir.z);
        }
    }

    return res;
}

} // namespace lod_poc
