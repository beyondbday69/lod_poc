#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace lod_poc {

class CameraProbe {
public:
    static CameraProbe &instance();

    void setConfig(bool enabled, const std::string &userNote);
    void onCullCall(void *a0);
    void reset();

private:
    CameraProbe();

    static constexpr size_t kSnapshotBytes = 0x1000;
    static constexpr size_t kNumFloats = kSnapshotBytes / sizeof(float);

    struct BufferSnapshot {
        bool valid{false};
        float data[kNumFloats]{0.0f};
    };

    struct TargetSnapshot {
        BufferSnapshot self;   // a0
        BufferSnapshot ptr08;  // *(a0 + 0x8)
        BufferSnapshot ptr10;  // *(a0 + 0x10)
        BufferSnapshot ptr18;  // *(a0 + 0x18)
    };

    enum class Stage {
        Uninitialized,
        WaitingT1,
        WaitingT2,
        WaitingT3,
        Done
    };

    bool mEnabled{true};
    std::string mUserNote;
    std::mutex mMutex;

    Stage mStage{Stage::Uninitialized};
    std::chrono::steady_clock::time_point mStartTime{};

    TargetSnapshot mSnapT1{};
    TargetSnapshot mSnapT2{};
    TargetSnapshot mSnapT3{};

    static bool safeReadMemory(const void *src, void *dst, size_t len);
    static bool isValidPointer(const void *ptr);

    void captureTarget(void *a0, TargetSnapshot &target);
    void runDiffAnalysis();
    void analyzeBufferDiff(const char *targetName,
                           const BufferSnapshot &s1,
                           const BufferSnapshot &s2,
                           const BufferSnapshot &s3);
};

} // namespace lod_poc
