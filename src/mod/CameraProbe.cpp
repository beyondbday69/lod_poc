#include "mod/CameraProbe.h"
#include "mod/LodPocMod.h"

#include <cmath>
#include <cstring>
#include <sys/uio.h>
#include <unistd.h>

namespace lod_poc {

CameraProbe &CameraProbe::instance() {
    static CameraProbe sInstance;
    return sInstance;
}

CameraProbe::CameraProbe() = default;

void CameraProbe::setConfig(bool enabled, const std::string &userNote) {
    std::lock_guard lock(mMutex);
    mEnabled = enabled;
    mUserNote = userNote;
}

void CameraProbe::reset() {
    std::lock_guard lock(mMutex);
    mStage = Stage::Uninitialized;
    mStartTime = std::chrono::steady_clock::time_point{};
}

bool CameraProbe::isValidPointer(const void *ptr) {
    if (ptr == nullptr) return false;
    uintptr_t u = reinterpret_cast<uintptr_t>(ptr);
    return (u >= 0x10000 && u <= 0x00007fffffffffffull && (u & 0x7) == 0);
}

bool CameraProbe::safeReadMemory(const void *src, void *dst, size_t len) {
    if (src == nullptr || dst == nullptr || len == 0) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(src);
    if (addr < 0x10000 || addr >= 0x00007fffffffffffull) return false;
    if ((addr & 0x3) != 0) return false; // float alignment

    struct iovec local { dst, len };
    struct iovec remote { const_cast<void *>(src), len };
    ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return n == static_cast<ssize_t>(len);
}

void CameraProbe::captureTarget(void *a0, TargetSnapshot &target) {
    if (a0 == nullptr) return;

    // Snapshot a0 (self)
    target.self.valid = safeReadMemory(a0, target.self.data, kSnapshotBytes);

    // Snapshot *(a0 + 0x8)
    void *p08 = nullptr;
    if (safeReadMemory(reinterpret_cast<const char *>(a0) + 0x8, &p08, sizeof(void *)) && isValidPointer(p08)) {
        target.ptr08.valid = safeReadMemory(p08, target.ptr08.data, kSnapshotBytes);
    }

    // Snapshot *(a0 + 0x10)
    void *p10 = nullptr;
    if (safeReadMemory(reinterpret_cast<const char *>(a0) + 0x10, &p10, sizeof(void *)) && isValidPointer(p10)) {
        target.ptr10.valid = safeReadMemory(p10, target.ptr10.data, kSnapshotBytes);
    }

    // Snapshot *(a0 + 0x18)
    void *p18 = nullptr;
    if (safeReadMemory(reinterpret_cast<const char *>(a0) + 0x18, &p18, sizeof(void *)) && isValidPointer(p18)) {
        target.ptr18.valid = safeReadMemory(p18, target.ptr18.data, kSnapshotBytes);
    }
}

void CameraProbe::onCullCall(void *a0) {
    if (!mEnabled || a0 == nullptr) return;

    std::lock_guard lock(mMutex);
    const auto now = std::chrono::steady_clock::now();

    if (mStage == Stage::Uninitialized) {
        mStartTime = now;
        mStage = Stage::WaitingT1;
        LodPocMod::logAll(LogLevel::Info,
                          "[Probe B: CameraProbe] Initialized. Snapshot T1 will capture in 15s at this={:p} (walk/turn during test).",
                          a0);
        return;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - mStartTime).count();

    if (mStage == Stage::WaitingT1 && elapsed >= 15) {
        captureTarget(a0, mSnapT1);
        mStage = Stage::WaitingT2;
        LodPocMod::logAll(LogLevel::Info,
                          "[Probe B: CameraProbe] Captured Snapshot T1 (+15s) at this={:p}. Walk/turn now (T2 in 10s)...",
                          a0);
        return;
    }

    if (mStage == Stage::WaitingT2 && elapsed >= 25) {
        captureTarget(a0, mSnapT2);
        mStage = Stage::WaitingT3;
        LodPocMod::logAll(LogLevel::Info,
                          "[Probe B: CameraProbe] Captured Snapshot T2 (+25s) at this={:p}. Walk/turn again (T3 in 10s)...",
                          a0);
        return;
    }

    if (mStage == Stage::WaitingT3 && elapsed >= 35) {
        captureTarget(a0, mSnapT3);
        mStage = Stage::Done;
        LodPocMod::logAll(LogLevel::Info,
                          "[Probe B: CameraProbe] Captured Snapshot T3 (+35s) at this={:p}. Running diff analysis...",
                          a0);
        runDiffAnalysis();
        return;
    }
}

void CameraProbe::runDiffAnalysis() {
    LodPocMod::logAll(LogLevel::Info, "[Probe B: CameraProbe] ==================== CAMERA DIFF ANALYSIS ====================");

    analyzeBufferDiff("a0 (LevelRendererCamera)", mSnapT1.self, mSnapT2.self, mSnapT3.self);
    analyzeBufferDiff("*(a0 + 0x8)", mSnapT1.ptr08, mSnapT2.ptr08, mSnapT3.ptr08);
    analyzeBufferDiff("*(a0 + 0x10)", mSnapT1.ptr10, mSnapT2.ptr10, mSnapT3.ptr10);
    analyzeBufferDiff("*(a0 + 0x18)", mSnapT1.ptr18, mSnapT2.ptr18, mSnapT3.ptr18);

    LodPocMod::logAll(LogLevel::Info, "[Probe B: CameraProbe] ============================================================");
    LodPocMod::logAll(LogLevel::Info, "[Probe B: CameraProbe] F3-FREE HINT:");
    LodPocMod::logAll(LogLevel::Info, "[Probe B: CameraProbe] User Note: '{}'", mUserNote);
    LodPocMod::logAll(LogLevel::Info, "[Probe B: CameraProbe] Compare your noted player coordinates against the CANDIDATE WORLD COORDS logged above.");
    LodPocMod::logAll(LogLevel::Info, "[Probe B: CameraProbe] ============================================================");
}

void CameraProbe::analyzeBufferDiff(const char *targetName,
                                    const BufferSnapshot &s1,
                                    const BufferSnapshot &s2,
                                    const BufferSnapshot &s3) {
    if (!s1.valid || !s2.valid || !s3.valid) {
        LodPocMod::logAll(LogLevel::Info,
                          "[Probe B: CameraProbe] [{}] not valid or not readable at all 3 snapshots (T1={}, T2={}, T3={}).",
                          targetName, s1.valid, s2.valid, s3.valid);
        return;
    }

    LodPocMod::logAll(LogLevel::Info, "[Probe B: CameraProbe] --- Target: {} ---", targetName);

    size_t changedOffsets = 0;
    for (size_t i = 0; i < kNumFloats; ++i) {
        float v1 = s1.data[i];
        float v2 = s2.data[i];
        float v3 = s3.data[i];

        if (v1 != v2 || v2 != v3) {
            changedOffsets++;
            LodPocMod::logAll(LogLevel::Info,
                              "[Probe B] DIFF [{}] +0x{:03x}: T1={:11.4f}, T2={:11.4f}, T3={:11.4f}",
                              targetName, i * 4, v1, v2, v3);
        }
    }
    LodPocMod::logAll(LogLevel::Info, "[Probe B] [{}] Total changed 4-byte offsets: {} / {}",
                      targetName, changedOffsets, kNumFloats);

    // Search for candidate 3 consecutive floats that look like world coordinates:
    // (abs < 30000000, not NaN, not inf, not all zero, and changed)
    size_t candidateCount = 0;
    for (size_t i = 0; i + 2 < kNumFloats; ++i) {
        float t1_x = s1.data[i], t1_y = s1.data[i + 1], t1_z = s1.data[i + 2];
        float t2_x = s2.data[i], t2_y = s2.data[i + 1], t2_z = s2.data[i + 2];
        float t3_x = s3.data[i], t3_y = s3.data[i + 1], t3_z = s3.data[i + 2];

        bool validT1 = std::isfinite(t1_x) && std::isfinite(t1_y) && std::isfinite(t1_z) &&
                       std::abs(t1_x) < 30000000.0f && std::abs(t1_y) < 30000000.0f && std::abs(t1_z) < 30000000.0f;
        bool validT2 = std::isfinite(t2_x) && std::isfinite(t2_y) && std::isfinite(t2_z) &&
                       std::abs(t2_x) < 30000000.0f && std::abs(t2_y) < 30000000.0f && std::abs(t2_z) < 30000000.0f;
        bool validT3 = std::isfinite(t3_x) && std::isfinite(t3_y) && std::isfinite(t3_z) &&
                       std::abs(t3_x) < 30000000.0f && std::abs(t3_y) < 30000000.0f && std::abs(t3_z) < 30000000.0f;

        if (!validT1 || !validT2 || !validT3) continue;

        // Skip all zeros
        if (t1_x == 0.0f && t1_y == 0.0f && t1_z == 0.0f) continue;

        // Must have changed between snapshots
        bool changed = (t1_x != t2_x || t2_x != t3_x ||
                        t1_y != t2_y || t2_y != t3_y ||
                        t1_z != t2_z || t2_z != t3_z);
        if (!changed) continue;

        candidateCount++;
        LodPocMod::logAll(LogLevel::Info,
                          "[Probe B] CANDIDATE WORLD COORDS [{}] at +0x{:03x}:\n"
                          "  T1: ({:10.3f}, {:10.3f}, {:10.3f})\n"
                          "  T2: ({:10.3f}, {:10.3f}, {:10.3f})\n"
                          "  T3: ({:10.3f}, {:10.3f}, {:10.3f})\n"
                          "  Delta(T2-T1): ({:+.3f}, {:+.3f}, {:+.3f})\n"
                          "  Delta(T3-T2): ({:+.3f}, {:+.3f}, {:+.3f})",
                          targetName, i * 4,
                          t1_x, t1_y, t1_z,
                          t2_x, t2_y, t2_z,
                          t3_x, t3_y, t3_z,
                          t2_x - t1_x, t2_y - t1_y, t2_z - t1_z,
                          t3_x - t2_x, t3_y - t2_y, t3_z - t2_z);
    }

    if (candidateCount == 0) {
        LodPocMod::logAll(LogLevel::Info, "[Probe B] [{}] No candidate coordinate triplets found.", targetName);
    }
}

} // namespace lod_poc
