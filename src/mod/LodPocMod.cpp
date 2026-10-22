#include "mod/LodPocMod.h"
#include "mod/ElfDiag.h"
#include "mod/PocQuad.h"
#include "mod/WorldReader.h"
#include "signatures.h"

#include <algorithm>
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <string>
#include <sys/syscall.h>
#include <sys/ucontext.h>
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

// -----------------------------------------------------------------------------
// Crash Capture (Async-Signal-Safe)
// -----------------------------------------------------------------------------

struct SavedSigactions {
    struct sigaction segv{};
    struct sigaction bus{};
    struct sigaction abrt{};
    struct sigaction ill{};
    struct sigaction fpe{};
    bool installed{false};
};

static SavedSigactions sSavedSigs;
static std::atomic<uintptr_t> sMinecraftBase{0};
static std::atomic<int> sPrimaryFd{-1};
static std::atomic<int> sSecondaryFd{-1};

static void writeRaw(int fd, const char *str) {
    if (fd < 0 || str == nullptr) {
        return;
    }
    size_t len = 0;
    while (str[len] != '\0') {
        ++len;
    }
    (void)::write(fd, str, len);
}

static void writeHex(int fd, uint64_t val) {
    if (fd < 0) {
        return;
    }
    char buf[19];
    buf[0] = '0';
    buf[1] = 'x';
    for (int i = 15; i >= 0; --i) {
        const uint8_t nibble = val & 0xF;
        buf[2 + i] = (nibble < 10) ? ('0' + nibble) : ('a' + (nibble - 10));
        val >>= 4;
    }
    buf[18] = '\0';
    (void)::write(fd, buf, 18);
}

static void writeDec(int fd, int val) {
    if (fd < 0) {
        return;
    }
    if (val < 0) {
        (void)::write(fd, "-", 1);
        val = -val;
    }
    char buf[12];
    int idx = 0;
    if (val == 0) {
        buf[idx++] = '0';
    } else {
        char temp[12];
        int tlen = 0;
        while (val > 0) {
            temp[tlen++] = '0' + (val % 10);
            val /= 10;
        }
        for (int i = tlen - 1; i >= 0; --i) {
            buf[idx++] = temp[i];
        }
    }
    (void)::write(fd, buf, static_cast<size_t>(idx));
}

static struct sigaction *getOldSigaction(int sig) {
    switch (sig) {
        case SIGSEGV: return &sSavedSigs.segv;
        case SIGBUS:  return &sSavedSigs.bus;
        case SIGABRT: return &sSavedSigs.abrt;
        case SIGILL:  return &sSavedSigs.ill;
        case SIGFPE:  return &sSavedSigs.fpe;
        default:      return nullptr;
    }
}

static void crashSignalHandler(int sig, siginfo_t *info, void *context) {
    const auto *uc = static_cast<const ucontext_t *>(context);
    uint64_t pc = 0, lr = 0, sp = 0;
    void *faultAddr = (info != nullptr) ? info->si_addr : nullptr;

#if defined(__aarch64__)
    if (uc != nullptr) {
        pc = uc->uc_mcontext.pc;
        lr = uc->uc_mcontext.regs[30];
        sp = uc->uc_mcontext.sp;
    }
#endif

    const uint64_t mcBase = sMinecraftBase.load(std::memory_order_relaxed);
    const int fds[2] = {sPrimaryFd.load(std::memory_order_relaxed),
                        sSecondaryFd.load(std::memory_order_relaxed)};

    for (int fd : fds) {
        if (fd < 0) {
            continue;
        }
        writeRaw(fd, "\n================================================================\n");
        writeRaw(fd, "FATAL CRASH CAUGHT BY LOD_POC:\n");
        writeRaw(fd, "  Signal:     ");
        writeDec(fd, sig);
        writeRaw(fd, " (");
        if (sig == SIGSEGV) writeRaw(fd, "SIGSEGV");
        else if (sig == SIGBUS) writeRaw(fd, "SIGBUS");
        else if (sig == SIGABRT) writeRaw(fd, "SIGABRT");
        else if (sig == SIGILL) writeRaw(fd, "SIGILL");
        else if (sig == SIGFPE) writeRaw(fd, "SIGFPE");
        else writeRaw(fd, "UNKNOWN");
        writeRaw(fd, ")\n  Fault Addr: ");
        writeHex(fd, reinterpret_cast<uintptr_t>(faultAddr));
        writeRaw(fd, "\n  PC:         ");
        writeHex(fd, pc);
        writeRaw(fd, "\n  LR:         ");
        writeHex(fd, lr);
        writeRaw(fd, "\n  SP:         ");
        writeHex(fd, sp);
        writeRaw(fd, "\n  MC Base:    ");
        writeHex(fd, mcBase);
        if (mcBase != 0) {
            writeRaw(fd, "\n  PC - Base:  ");
            writeHex(fd, pc - mcBase);
            writeRaw(fd, "\n  LR - Base:  ");
            writeHex(fd, lr - mcBase);
        }
        writeRaw(fd, "\n================================================================\n");
        (void)::fsync(fd);
    }

    // Chain to previously installed signal handler
    struct sigaction *oldSa = getOldSigaction(sig);
    if (oldSa != nullptr) {
        if (oldSa->sa_flags & SA_SIGINFO) {
            if (oldSa->sa_sigaction != nullptr) {
                oldSa->sa_sigaction(sig, info, context);
                return;
            }
        } else {
            if (oldSa->sa_handler != SIG_DFL && oldSa->sa_handler != SIG_IGN &&
                oldSa->sa_handler != nullptr) {
                oldSa->sa_handler(sig);
                return;
            }
        }
    }

    // Fallback to default disposition
    signal(sig, SIG_DFL);
    raise(sig);
}

static void installCrashHandlers() {
    if (sSavedSigs.installed) {
        return;
    }
    sPrimaryFd.store(FileLog::instance().getPrimaryFd(), std::memory_order_relaxed);
    sSecondaryFd.store(FileLog::instance().getSecondaryFd(), std::memory_order_relaxed);

    struct sigaction sa{};
    sa.sa_sigaction = crashSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &sSavedSigs.segv);
    sigaction(SIGBUS, &sa, &sSavedSigs.bus);
    sigaction(SIGABRT, &sa, &sSavedSigs.abrt);
    sigaction(SIGILL, &sa, &sSavedSigs.ill);
    sigaction(SIGFPE, &sa, &sSavedSigs.fpe);
    sSavedSigs.installed = true;
}

static void restoreCrashHandlers() {
    if (!sSavedSigs.installed) {
        return;
    }
    sigaction(SIGSEGV, &sSavedSigs.segv, nullptr);
    sigaction(SIGBUS, &sSavedSigs.bus, nullptr);
    sigaction(SIGABRT, &sSavedSigs.abrt, nullptr);
    sigaction(SIGILL, &sSavedSigs.ill, nullptr);
    sigaction(SIGFPE, &sSavedSigs.fpe, nullptr);
    sSavedSigs.installed = false;
}

// -----------------------------------------------------------------------------
// Module Base Resolver
// -----------------------------------------------------------------------------

[[maybe_unused]] static uintptr_t getModuleBase(const char *moduleName) {
    struct Match {
        const char *name;
        uintptr_t base;
    } match{moduleName, 0};

    dl_iterate_phdr([](struct dl_phdr_info *info, size_t, void *data) -> int {
        auto *m = static_cast<Match *>(data);
        if (info->dlpi_name != nullptr && std::strstr(info->dlpi_name, m->name) != nullptr) {
            m->base = info->dlpi_addr;
            return 1;
        }
        return 0;
    }, &match);

    if (match.base == 0) {
        FILE *fp = std::fopen("/proc/self/maps", "r");
        if (fp != nullptr) {
            char line[512];
            while (std::fgets(line, sizeof(line), fp) != nullptr) {
                if (std::strstr(line, moduleName) != nullptr) {
                    uintptr_t start = 0;
                    if (std::sscanf(line, "%lx-", &start) == 1) {
                        match.base = start;
                        break;
                    }
                }
            }
            std::fclose(fp);
        }
    }
    return match.base;
}

} // namespace

LodPocMod &LodPocMod::instance() {
    static LodPocMod mod;
    return mod;
}

LodPocMod::LodPocMod()
    : mSelf(*ll::mod::NativeMod::current()),
      mLastLightingReport(std::chrono::steady_clock::now()),
      mLastFpsReport(std::chrono::steady_clock::now()),
      mLastCullReport(std::chrono::steady_clock::now()) {}

bool LodPocMod::load() {
    auto &self = getSelf();
    try {
        // Initialize file logger in mod data directory
        FileLog::instance().open(self.getDataDir());

        // Immediately inspect libminecraftpe.so and log build-id as the very first lines
        mElfInfo = ElfDiag::inspectMinecraftBinary();
        sMinecraftBase.store(mElfInfo.baseAddress, std::memory_order_relaxed);

        logAll(LogLevel::Info, "================================================================");
        logAll(LogLevel::Info, "[Init] libminecraftpe.so Build-ID: {}", mElfInfo.buildId.empty() ? "UNKNOWN" : mElfInfo.buildId);
        logAll(LogLevel::Info, "[Init] libminecraftpe.so Path:     {}", mElfInfo.path.empty() ? "UNKNOWN" : mElfInfo.path);
        logAll(LogLevel::Info, "[Init] libminecraftpe.so Size:     {} bytes ({:.2f} MB)", mElfInfo.fileSize, mElfInfo.fileSize / (1024.0 * 1024.0));
        logAll(LogLevel::Info, "[Init] Build Match (712509dc14ccc233e91f267937dfb46ecdcc4b68): {}",
               mElfInfo.matchesKnownReBuild ? "YES (EXACT MATCH)" : "NO (BUILD MISMATCH)");
        logAll(LogLevel::Info, "================================================================");

        logAll(LogLevel::Info, "Loading LOD PoC mod v{} (author: {})...",
               self.getVersion(), self.getAuthor());
        logAll(LogLevel::Info, "Target Minecraft Bedrock: 1.26.51.1 (arm64-v8a)");

        if (FileLog::instance().isPrimaryOpen()) {
            logAll(LogLevel::Info, "Primary log file opened: {}",
                   FileLog::instance().getPrimaryPath());
        } else {
            logAll(LogLevel::Warn, "Primary log file failed to open at: {}",
                   FileLog::instance().getPrimaryPath());
        }

        if (FileLog::instance().isSecondaryOpen()) {
            logAll(LogLevel::Info, "Secondary log file opened: {}",
                   FileLog::instance().getSecondaryPath());
        } else {
            logAll(LogLevel::Info, "Secondary log file not available at: {}",
                   FileLog::instance().getSecondaryPath());
        }

        std::lock_guard lock(mConfigMutex);
        mConfigFile.emplace(ModConfig{}, self.getConfigDir() / "config.json",
                            self.getConfigDir() / "config.schema.json");
        if (!mConfigFile->load()) {
            logAll(LogLevel::Warn, "Failed to read config.json, using defaults");
        } else {
            const auto &cfg = mConfigFile->value();
            GlTrace::instance().setConfig(cfg.traceFrame, cfg.traceRaw, cfg.traceLabel, cfg.traceDelayAfterWorldSec);
            CameraProbe::instance().setConfig(cfg.probeCameraDiff, cfg.userNote);
            PocQuad::instance().setConfig(cfg.pocQuad, cfg.pocDepth, cfg.pocColor, cfg.pocTrigger, cfg.poc3d, cfg.poc3dDist);
        }
        logAll(LogLevel::Info, "Loaded successfully");
        return true;
    } catch (const std::exception &ex) {
        logAll(LogLevel::Error, "load() threw exception: {}", ex.what());
    } catch (...) {
        logAll(LogLevel::Error, "load() threw unknown exception");
    }
    return false;
}

bool LodPocMod::enable() {
    try {
        logAll(LogLevel::Info, "Enabling LOD PoC diagnostic probes...");

        // Ensure Minecraft base address is known
        if (sMinecraftBase.load(std::memory_order_relaxed) == 0 && mElfInfo.baseAddress != 0) {
            sMinecraftBase.store(mElfInfo.baseAddress, std::memory_order_relaxed);
        }

        // Check Build-ID safety gate
        bool requireKnown = true;
        {
            std::lock_guard lock(mConfigMutex);
            if (mConfigFile) {
                requireKnown = mConfigFile->value().requireKnownBuild;
            }
        }

        if (!mElfInfo.matchesKnownReBuild && requireKnown) {
            logAll(LogLevel::Warn, "================================================================");
            logAll(LogLevel::Warn, "BUILD MISMATCH: signature-based probes disabled");
            logAll(LogLevel::Warn, "  Loaded Build-ID:   {}", mElfInfo.buildId);
            logAll(LogLevel::Warn, "  Expected Build-ID: 712509dc14ccc233e91f267937dfb46ecdcc4b68");
            logAll(LogLevel::Warn, "  GL/EGL probes, world reader, file log, and crash handlers remain active.");
            logAll(LogLevel::Warn, "  (Set 'requireKnownBuild': false in config.json to override)");
            logAll(LogLevel::Warn, "================================================================");
        } else if (!mElfInfo.matchesKnownReBuild) {
            logAll(LogLevel::Warn, "BUILD MISMATCH OVERRIDDEN: requireKnownBuild is false, attempting signature probes...");
        }

        // Install crash capture handlers
        try {
            installCrashHandlers();
        } catch (const std::exception &ex) {
            logAll(LogLevel::Error, "installCrashHandlers() failed: {}", ex.what());
        } catch (...) {
            logAll(LogLevel::Error, "installCrashHandlers() failed with unknown error");
        }

        // Install probe hooks defensively
        installProbes();

        // Trigger background LevelDB world scan if enabled
        bool shouldScanWorld = true;
        int rescanIntervalSec = 0;
        {
            std::lock_guard lock(mConfigMutex);
            if (mConfigFile) {
                shouldScanWorld = mConfigFile->value().buildLodMap;
                rescanIntervalSec = mConfigFile->value().rescanIntervalSec;
            }
        }
        if (shouldScanWorld) {
            try {
                WorldReader::startBackgroundScan(getSelf().getDataDir(), rescanIntervalSec);
            } catch (const std::exception &ex) {
                logAll(LogLevel::Error, "WorldReader::startBackgroundScan failed: {}", ex.what());
            } catch (...) {
                logAll(LogLevel::Error, "WorldReader::startBackgroundScan failed with unknown error");
            }
        }

        logAll(LogLevel::Info, "LOD PoC mod enabled");
        return true;
    } catch (const std::exception &ex) {
        logAll(LogLevel::Error, "enable() threw exception: {}", ex.what());
    } catch (...) {
        logAll(LogLevel::Error, "enable() threw unknown exception");
    }
    return false;
}

bool LodPocMod::disable() {
    try {
        logAll(LogLevel::Info, "Disabling LOD PoC diagnostic probes...");
        removeProbes();
        restoreCrashHandlers();
        GlTrace::instance().remove();
        PocQuad::instance().cleanup();
        CameraProbe::instance().reset();
        logAll(LogLevel::Info, "LOD PoC mod disabled");
        return true;
    } catch (const std::exception &ex) {
        logAll(LogLevel::Error, "disable() threw exception: {}", ex.what());
    } catch (...) {
        logAll(LogLevel::Error, "disable() threw unknown exception");
    }
    return false;
}

bool LodPocMod::unload() {
    try {
        logAll(LogLevel::Info, "Unloading LOD PoC mod...");
        removeProbes();
        restoreCrashHandlers();
        GlTrace::instance().remove();
        PocQuad::instance().cleanup();
        CameraProbe::instance().reset();
        {
            std::lock_guard lock(mConfigMutex);
            mConfigFile.reset();
        }
        logAll(LogLevel::Info, "Unloaded successfully");
        FileLog::instance().close();
        return true;
    } catch (const std::exception &ex) {
        logAll(LogLevel::Error, "unload() threw exception: {}", ex.what());
    } catch (...) {
        logAll(LogLevel::Error, "unload() threw unknown exception");
    }
    return false;
}

void LodPocMod::installProbes() {
    removeProbes();

    const auto now = std::chrono::steady_clock::now();
    mLastLightingReport = now;
    mLastFpsReport = now;
    mLastCullReport = now;

    bool requireKnown = true;
    {
        std::lock_guard lock(mConfigMutex);
        if (mConfigFile) {
            requireKnown = mConfigFile->value().requireKnownBuild;
        }
    }
    const bool skipSigProbes = requireKnown && !mElfInfo.matchesKnownReBuild;

    // -------------------------------------------------------------------------
    // Probe 1: ChunkSource::_lightingTask (Signature-based)
    // -------------------------------------------------------------------------
    if (skipSigProbes) {
        logAll(LogLevel::Info, "[Probe 1: lightingTask] SKIPPED (build mismatch)");
    } else {
        try {
            const auto lightingAddr = pl::memory::resolveSignature(
                mc_signatures::kSigChunkLightingTask, kMinecraftLibrary);
            if (lightingAddr == 0) {
                logAll(LogLevel::Warn, "[Probe 1: lightingTask] NOT FOUND (sig failed)");
                ElfDiag::runSignatureDiagnostics(mc_signatures::kSigChunkLightingTask, "ChunkSource::_lightingTask");
            } else {
                void *orig = nullptr;
                pl::memory::HookHandle handle(
                    reinterpret_cast<void *>(lightingAddr),
                    reinterpret_cast<void *>(&LodPocMod::lightingTaskDetour),
                    &orig);
                if (!handle.installed()) {
                    logAll(LogLevel::Error, "[Probe 1: lightingTask] FAILED at 0x{:x}", lightingAddr);
                } else {
                    mOriginalLightingTask = reinterpret_cast<ChunkLightingTaskFn>(orig);
                    mLightingHook = std::move(handle);
                    logAll(LogLevel::Info, "[Probe 1: lightingTask] Hooked at 0x{:x}", lightingAddr);
                }
            }
        } catch (const std::exception &ex) {
            logAll(LogLevel::Error, "[Probe 1: lightingTask] Exception: {}", ex.what());
        } catch (...) {
            logAll(LogLevel::Error, "[Probe 1: lightingTask] Unknown exception");
        }
    }

    // -------------------------------------------------------------------------
    // Probe 2 & Probe A: OpenGL ES trace hooks (dlsym-based, independent of build)
    // -------------------------------------------------------------------------
    try {
        GlTrace::instance().install();
    } catch (const std::exception &ex) {
        logAll(LogLevel::Error, "[GLTrace] install() exception: {}", ex.what());
    } catch (...) {
        logAll(LogLevel::Error, "[GLTrace] install() unknown exception");
    }

    // -------------------------------------------------------------------------
    // Probe 3: eglSwapBuffers (dlsym-based, independent of build)
    // -------------------------------------------------------------------------
    try {
        void *swapTarget = nullptr;
        void *eglLib = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
        if (eglLib != nullptr) {
            swapTarget = dlsym(eglLib, "eglSwapBuffers");
        }
        if (swapTarget == nullptr) {
            swapTarget = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
        }
        if (swapTarget == nullptr) {
            logAll(LogLevel::Warn, "[Probe 3: eglSwapBuffers] NOT FOUND");
        } else {
            void *orig = nullptr;
            pl::memory::HookHandle handle(
                swapTarget,
                reinterpret_cast<void *>(&LodPocMod::swapBuffersDetour),
                &orig);
            if (!handle.installed()) {
                logAll(LogLevel::Error, "[Probe 3: eglSwapBuffers] FAILED at {:p}", swapTarget);
            } else {
                mOriginalSwapBuffers = reinterpret_cast<EglSwapBuffersFn>(orig);
                mSwapBuffersHook = std::move(handle);
                logAll(LogLevel::Info, "[Probe 3: eglSwapBuffers] Hooked at {:p}", swapTarget);
            }
        }
    } catch (const std::exception &ex) {
        logAll(LogLevel::Error, "[Probe 3: eglSwapBuffers] Exception: {}", ex.what());
    } catch (...) {
        logAll(LogLevel::Error, "[Probe 3: eglSwapBuffers] Unknown exception");
    }

    // -------------------------------------------------------------------------
    // Probe 4: SubChunk::getBlock (Signature resolution ONLY - do NOT hook)
    // -------------------------------------------------------------------------
    if (skipSigProbes) {
        logAll(LogLevel::Info, "[Probe 4: SubChunk::getBlock] SKIPPED (build mismatch)");
    } else {
        try {
            const auto subChunkAddr = pl::memory::resolveSignature(
                mc_signatures::kSigSubChunkGetBlockStorage, kMinecraftLibrary);
            if (subChunkAddr != 0) {
                logAll(LogLevel::Info, "[Probe 4: SubChunk::getBlock] Resolved at 0x{:x} (verified formula: (x << 8) | (z << 4) | y)",
                       subChunkAddr);
            } else {
                logAll(LogLevel::Warn, "[Probe 4: SubChunk::getBlock] NOT FOUND (sig failed)");
                ElfDiag::runSignatureDiagnostics(mc_signatures::kSigSubChunkGetBlockStorage, "SubChunk::getBlock");
            }
        } catch (const std::exception &ex) {
            logAll(LogLevel::Error, "[Probe 4: SubChunk::getBlock] Exception: {}", ex.what());
        } catch (...) {
            logAll(LogLevel::Error, "[Probe 4: SubChunk::getBlock] Unknown exception");
        }
    }

    // -------------------------------------------------------------------------
    // Probe 5: LevelRendererCamera::cullPointOrChunk (Signature-based)
    // -------------------------------------------------------------------------
    if (skipSigProbes) {
        logAll(LogLevel::Info, "[Probe 5: cullPointOrChunk] SKIPPED (build mismatch)");
    } else {
        try {
            const auto cullAddr = pl::memory::resolveSignature(
                mc_signatures::kSigLevelRendererCameraCull, kMinecraftLibrary);
            if (cullAddr == 0) {
                logAll(LogLevel::Warn, "[Probe 5: cullPointOrChunk] NOT FOUND (sig failed)");
                ElfDiag::runSignatureDiagnostics(mc_signatures::kSigLevelRendererCameraCull, "LevelRendererCamera::cullPointOrChunk");
            } else {
                void *orig = nullptr;
                pl::memory::HookHandle handle(
                    reinterpret_cast<void *>(cullAddr),
                    reinterpret_cast<void *>(&LodPocMod::cullPointOrChunkDetour),
                    &orig);
                if (!handle.installed()) {
                    logAll(LogLevel::Error, "[Probe 5: cullPointOrChunk] FAILED at 0x{:x}", cullAddr);
                } else {
                    mOriginalCull = reinterpret_cast<CullPointOrChunkFn>(orig);
                    mCullHook = std::move(handle);
                    logAll(LogLevel::Info, "[Probe 5: cullPointOrChunk] Hooked at 0x{:x}", cullAddr);
                }
            }
        } catch (const std::exception &ex) {
            logAll(LogLevel::Error, "[Probe 5: cullPointOrChunk] Exception: {}", ex.what());
        } catch (...) {
            logAll(LogLevel::Error, "[Probe 5: cullPointOrChunk] Unknown exception");
        }
    }
}

void LodPocMod::removeProbes() {
    mLightingHook.reset();
    mOriginalLightingTask = nullptr;

    GlTrace::instance().remove();

    mSwapBuffersHook.reset();
    mOriginalSwapBuffers = nullptr;

    mCullHook.reset();
    mOriginalCull = nullptr;
}

void LodPocMod::onBindFramebuffer(GLenum target, GLuint framebuffer) {
    const uint64_t call = ++mBindCalls;
    const pid_t tid = getThreadId();

    if (call <= 20) {
        logAll(LogLevel::Info,
               "[Probe 2: glBindFramebuffer] #{} tid={} target=0x{:x} fbo={}",
               call, tid, target, framebuffer);
    }

    const size_t idx = mFrameBindCount.fetch_add(1, std::memory_order_relaxed);
    if (idx < kMaxFrameBinds) {
        mFrameBinds[idx] = framebuffer;
    }
}

void LodPocMod::checkConfigReload() {
    const auto now = std::chrono::steady_clock::now();
    if (now - mLastConfigCheck >= std::chrono::seconds(2)) {
        mLastConfigCheck = now;
        std::lock_guard lock(mConfigMutex);
        if (mConfigFile) {
            if (mConfigFile->load()) {
                const auto &cfg = mConfigFile->value();
                GlTrace::instance().setConfig(cfg.traceFrame, cfg.traceRaw, cfg.traceLabel, cfg.traceDelayAfterWorldSec);
                CameraProbe::instance().setConfig(cfg.probeCameraDiff, cfg.userNote);
                PocQuad::instance().setConfig(cfg.pocQuad, cfg.pocDepth, cfg.pocColor, cfg.pocTrigger, cfg.poc3d, cfg.poc3dDist);

                if (cfg.buildLodMap) {
                    WorldReader::startBackgroundScan(getSelf().getDataDir(), cfg.rescanIntervalSec);
                } else {
                    WorldReader::setRescanIntervalSec(0);
                }
            }
        }
    }
}

void LodPocMod::lightingTaskDetour(void *a0, void *a1, void *a2, void *a3,
                                   void *a4, void *a5, void *a6, void *a7) {
    auto &mod = LodPocMod::instance();

    // Read the 4 qwords from context (a0) BEFORE calling original
    uint64_t q[4] = {0, 0, 0, 0};
    if (a0 != nullptr) {
        std::memcpy(q, a0, sizeof(q));
    }

    // Call original forwarding all 8 args (never skip if non-null)
    const auto orig = mod.mOriginalLightingTask;
    if (orig != nullptr) {
        orig(a0, a1, a2, a3, a4, a5, a6, a7);
    }

    const uint64_t call = ++mod.mLightingCalls;
    const pid_t tid = getThreadId();

    if (call <= 20) {
        logAll(LogLevel::Info,
               "[Probe 1: lightingTask] #{} tid={} ctx={:p} [0x{:016x}, 0x{:016x}, 0x{:016x}, 0x{:016x}]",
               call, tid, a0, q[0], q[1], q[2], q[3]);
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
            logAll(LogLevel::Info,
                   "[Probe 1: lightingTask] 5s Summary: calls={}, rate={:.1f}/s, tid={}",
                   call, rate, tid);
        }
    }
}

EGLBoolean LodPocMod::swapBuffersDetour(EGLDisplay dpy, EGLSurface surf) {
    auto &mod = LodPocMod::instance();

    // Call original first (never skip if non-null)
    const auto orig = mod.mOriginalSwapBuffers;
    const EGLBoolean res = orig ? orig(dpy, surf) : EGL_TRUE;

    const uint64_t frame = ++mod.mFrameCount;
    const pid_t tid = getThreadId();

    static std::atomic<bool> sLoggedGlVersion{false};
    if (!sLoggedGlVersion.exchange(true)) {
        const GLubyte *renderer = glGetString(GL_RENDERER);
        const GLubyte *version = glGetString(GL_VERSION);
        const GLubyte *vendor = glGetString(GL_VENDOR);
        logAll(LogLevel::Info, "[GL Context] Renderer: {}", renderer ? reinterpret_cast<const char *>(renderer) : "unknown");
        logAll(LogLevel::Info, "[GL Context] Version:  {}", version ? reinterpret_cast<const char *>(version) : "unknown");
        logAll(LogLevel::Info, "[GL Context] Vendor:   {}", vendor ? reinterpret_cast<const char *>(vendor) : "unknown");
    }

    // Probe A: GL frame trace & render thread tracking
    GlTrace::instance().onSwapBuffers(frame, mod.mCurrentFps.load(std::memory_order_relaxed));

    // Refresh config periodically
    mod.checkConfigReload();

    if (frame <= 20) {
        logAll(LogLevel::Info,
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
        logAll(LogLevel::Info,
               "[Probe 2: glBindFramebuffer] frame {}: binds [{}] (total={}), tid={}",
               frame, bindsStr, totalBinds, tid);
    } else {
        mod.mFrameBindCount.store(0, std::memory_order_relaxed);
    }

    // Probe 3 5-second FPS report + alive heartbeat
    const auto now = std::chrono::steady_clock::now();
    if (now - mod.mLastFpsReport >= std::chrono::seconds(5)) {
        std::lock_guard lock(mod.mFpsMutex);
        if (now - mod.mLastFpsReport >= std::chrono::seconds(5)) {
            const double sec = std::chrono::duration<double>(now - mod.mLastFpsReport).count();
            const uint64_t prev = mod.mLastFpsFrame.load(std::memory_order_relaxed);
            mod.mLastFpsFrame.store(frame, std::memory_order_relaxed);
            mod.mLastFpsReport = now;
            const double fps = (sec > 0.0) ? (static_cast<double>(frame - prev) / sec) : 0.0;
            mod.mCurrentFps.store(fps, std::memory_order_relaxed);
            logAll(LogLevel::Info,
                   "[Probe 3: eglSwapBuffers] alive frame={} fps={:.2f} (total_frames={}), tid={}",
                   frame, fps, frame, tid);
        }
    }

    return res;
}

void *LodPocMod::cullPointOrChunkDetour(void *a0, void *a1, void *a2, void *a3,
                                        void *a4, void *a5, void *a6, void *a7) {
    auto &mod = LodPocMod::instance();

    // Call original first forwarding all 8 args (never skip if non-null)
    const auto orig = mod.mOriginalCull;
    void *const res = orig
                          ? orig(a0, a1, a2, a3, a4, a5, a6, a7)
                          : nullptr;

    const uint64_t call = ++mod.mCullCalls;

    // Probe B: Camera Position Discovery snapshot
    CameraProbe::instance().onCullCall(a0);

    // Optimization: only process telemetry on first 20 calls or every 4096 calls
    if (call <= 20 || (call % 4096 == 0)) {
        const void *thisPtr = a0;
        Vec3 pos{0.0f, 0.0f, 0.0f};
        Vec3 dir{0.0f, 0.0f, 0.0f};
        if (thisPtr != nullptr) {
            std::memcpy(&pos, reinterpret_cast<const char *>(thisPtr) + 0x5c8, sizeof(Vec3));
            std::memcpy(&dir, reinterpret_cast<const char *>(thisPtr) + 0x5d4, sizeof(Vec3));
        }

        if (call <= 20) {
            const pid_t tid = getThreadId();
            logAll(LogLevel::Info,
                   "[Probe 5: cullPointOrChunk] #{} tid={} this={:p} pos=({:.2f}, {:.2f}, {:.2f}) dir=({:.2f}, {:.2f}, {:.2f})",
                   call, tid, thisPtr, pos.x, pos.y, pos.z, dir.x, dir.y, dir.z);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - mod.mLastCullReport >= std::chrono::seconds(5)) {
            std::lock_guard lock(mod.mCullMutex);
            if (now - mod.mLastCullReport >= std::chrono::seconds(5)) {
                const pid_t tid = getThreadId();
                const double sec = std::chrono::duration<double>(now - mod.mLastCullReport).count();
                const uint64_t prev = mod.mLastCullCalls.load(std::memory_order_relaxed);
                mod.mLastCullCalls.store(call, std::memory_order_relaxed);
                mod.mLastCullReport = now;
                const double rate = (sec > 0.0) ? (static_cast<double>(call - prev) / sec) : 0.0;
                logAll(LogLevel::Info,
                       "[Probe 5: cullPointOrChunk] 5s Summary: calls={}, rate={:.1f}/s, tid={}, pos=({:.2f}, {:.2f}, {:.2f}) dir=({:.2f}, {:.2f}, {:.2f})",
                       call, rate, tid, pos.x, pos.y, pos.z, dir.x, dir.y, dir.z);
            }
        }
    }

    return res;
}

} // namespace lod_poc
