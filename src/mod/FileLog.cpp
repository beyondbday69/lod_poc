#include "mod/FileLog.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <sys/syscall.h>
#include <unistd.h>

namespace lod_poc {
namespace {

inline pid_t getThreadId() {
#if defined(__ANDROID__)
    return gettid();
#elif defined(SYS_gettid)
    return static_cast<pid_t>(syscall(SYS_gettid));
#else
    return 0;
#endif
}

void rotateAndOpen(const std::filesystem::path &logPath,
                   const std::filesystem::path &oldPath,
                   FILE *&outFp, int &outFd, std::string &outPath) {
    outFp = nullptr;
    outFd = -1;
    outPath = logPath.string();

    std::error_code ec;
    try {
        if (std::filesystem::exists(logPath, ec)) {
            std::filesystem::remove(oldPath, ec);
            std::filesystem::rename(logPath, oldPath, ec);
        }
        FILE *fp = std::fopen(logPath.c_str(), "w");
        if (fp != nullptr) {
            outFp = fp;
            outFd = fileno(fp);
        }
    } catch (...) {
        outFp = nullptr;
        outFd = -1;
    }
}

} // namespace

FileLog &FileLog::instance() {
    static FileLog sInstance;
    return sInstance;
}

FileLog::~FileLog() {
    close();
}

void FileLog::open(const std::filesystem::path &primaryDir) {
    std::lock_guard lock(mMutex);
    close();

    mPrimaryBytes = 0;
    mSecondaryBytes = 0;
    mPrimaryCapped = false;
    mSecondaryCapped = false;

    // 1. Primary log file in mod data directory
    try {
        std::error_code ec;
        std::filesystem::create_directories(primaryDir, ec);
        const auto primaryLog = primaryDir / "lod_poc.log";
        const auto primaryOld = primaryDir / "lod_poc.log.old";
        rotateAndOpen(primaryLog, primaryOld, mPrimaryFile, mPrimaryFd, mPrimaryPath);
    } catch (...) {
        mPrimaryFile = nullptr;
        mPrimaryFd = -1;
    }

    // 2. Secondary best-effort log file in /storage/emulated/0/Download
    try {
        const std::filesystem::path dlDir = "/storage/emulated/0/Download";
        std::error_code ec;
        if (std::filesystem::exists(dlDir, ec)) {
            const auto secLog = dlDir / "lod_poc.log";
            const auto secOld = dlDir / "lod_poc.log.old";
            rotateAndOpen(secLog, secOld, mSecondaryFile, mSecondaryFd, mSecondaryPath);
        }
    } catch (...) {
        mSecondaryFile = nullptr;
        mSecondaryFd = -1;
    }
}

void FileLog::close() {
    if (mPrimaryFile != nullptr) {
        std::fflush(mPrimaryFile);
        std::fclose(mPrimaryFile);
        mPrimaryFile = nullptr;
        mPrimaryFd = -1;
    }
    if (mSecondaryFile != nullptr) {
        std::fflush(mSecondaryFile);
        std::fclose(mSecondaryFile);
        mSecondaryFile = nullptr;
        mSecondaryFd = -1;
    }
}

void FileLog::write(char level, std::string_view message) {
    if (mPrimaryFile == nullptr && mSecondaryFile == nullptr) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&t, &tm_buf);
    const pid_t tid = getThreadId();

    char header[64];
    const int headerLen = std::snprintf(
        header, sizeof(header), "%02d:%02d:%02d.%03d [%c] tid=%d ",
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        static_cast<int>(ms.count()), level, static_cast<int>(tid));

    if (headerLen <= 0) {
        return;
    }

    std::lock_guard lock(mMutex);

    // Write to primary file
    if (mPrimaryFile != nullptr) {
        if (!mPrimaryCapped) {
            const size_t lineBytes = static_cast<size_t>(headerLen) + message.size() + 1;
            if (mPrimaryBytes + lineBytes > kMaxFileSize) {
                mPrimaryCapped = true;
                static constexpr char kCapMsg[] = "\n--- [LOG CAP REACHED: 5 MB] ---\n";
                std::fputs(kCapMsg, mPrimaryFile);
                std::fflush(mPrimaryFile);
            } else {
                std::fwrite(header, 1, static_cast<size_t>(headerLen), mPrimaryFile);
                std::fwrite(message.data(), 1, message.size(), mPrimaryFile);
                std::fputc('\n', mPrimaryFile);
                std::fflush(mPrimaryFile);
                mPrimaryBytes += lineBytes;
            }
        }
    }

    // Write to secondary file
    if (mSecondaryFile != nullptr) {
        if (!mSecondaryCapped) {
            const size_t lineBytes = static_cast<size_t>(headerLen) + message.size() + 1;
            if (mSecondaryBytes + lineBytes > kMaxFileSize) {
                mSecondaryCapped = true;
                static constexpr char kCapMsg[] = "\n--- [LOG CAP REACHED: 5 MB] ---\n";
                std::fputs(kCapMsg, mSecondaryFile);
                std::fflush(mSecondaryFile);
            } else {
                std::fwrite(header, 1, static_cast<size_t>(headerLen), mSecondaryFile);
                std::fwrite(message.data(), 1, message.size(), mSecondaryFile);
                std::fputc('\n', mSecondaryFile);
                std::fflush(mSecondaryFile);
                mSecondaryBytes += lineBytes;
            }
        }
    }
}

} // namespace lod_poc
