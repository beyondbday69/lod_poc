#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace lod_poc {

/**
 * @brief Thread-safe file logger for device telemetry without adb.
 *
 * Writes to primary mod data directory and a best-effort copy in /sdcard/Download.
 * Automatically rotates the previous log to .old on launch and caps file size at 5 MB.
 */
class FileLog {
  public:
    static FileLog &instance();

    FileLog() = default;
    ~FileLog();

    FileLog(const FileLog &) = delete;
    FileLog &operator=(const FileLog &) = delete;

    void open(const std::filesystem::path &primaryDir);
    void close();

    void write(char level, std::string_view message);

    [[nodiscard]] const std::string &getPrimaryPath() const { return mPrimaryPath; }
    [[nodiscard]] const std::string &getSecondaryPath() const { return mSecondaryPath; }

    [[nodiscard]] bool isPrimaryOpen() const { return mPrimaryFile != nullptr; }
    [[nodiscard]] bool isSecondaryOpen() const { return mSecondaryFile != nullptr; }

    [[nodiscard]] int getPrimaryFd() const { return mPrimaryFd; }
    [[nodiscard]] int getSecondaryFd() const { return mSecondaryFd; }

  private:
    std::mutex mMutex;
    FILE *mPrimaryFile{nullptr};
    FILE *mSecondaryFile{nullptr};
    int mPrimaryFd{-1};
    int mSecondaryFd{-1};

    std::string mPrimaryPath;
    std::string mSecondaryPath;

    size_t mPrimaryBytes{0};
    size_t mSecondaryBytes{0};

    bool mPrimaryCapped{false};
    bool mSecondaryCapped{false};

    static constexpr size_t kMaxFileSize = 5 * 1024 * 1024; // 5 MB
};

} // namespace lod_poc
