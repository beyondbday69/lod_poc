#include "mod/WorldReader.h"
#include "mod/LdbReader.h"
#include "mod/LodMesh.h"
#include "mod/LodPocMod.h"
#include "mod/PngWriter.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dirent.h>
#include <exception>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lod_poc {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPollInterval = std::chrono::seconds(3);
constexpr auto kWaitLogInterval = std::chrono::seconds(30);
constexpr auto kWaitTimeout = std::chrono::minutes(15);
constexpr auto kRescanSleepSlice = std::chrono::milliseconds(250);

std::atomic<bool> gLaunched{false};
std::atomic<bool> gRunning{false};
std::atomic<bool> gRestartRequested{false};
std::atomic<int> gRescanIntervalSec{0};

bool isWorldDbFileName(std::string_view filename) {
    return filename.ends_with(".ldb") || filename.ends_with(".log") ||
           filename.starts_with("MANIFEST") || filename == "CURRENT";
}

std::string findActiveWorldDb(std::vector<std::string> &outCandidates) {
    DIR *dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        return "";
    }

    std::string result;
    struct dirent *entry = nullptr;

    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        char linkPath[64];
        const int linkLen = std::snprintf(linkPath, sizeof(linkPath),
                                          "/proc/self/fd/%s", entry->d_name);
        if (linkLen <= 0 || static_cast<size_t>(linkLen) >= sizeof(linkPath)) {
            continue;
        }

        char target[4096];
        const ssize_t length =
            readlink(linkPath, target, sizeof(target) - 1);
        if (length <= 0) {
            continue;
        }
        target[length] = '\0';

        std::string_view path(target);
        static constexpr std::string_view kDeletedSuffix = " (deleted)";
        if (path.ends_with(kDeletedSuffix)) {
            path.remove_suffix(kDeletedSuffix.size());
        }

        const auto dbPosition = path.rfind("/db/");
        if (dbPosition == std::string_view::npos) {
            continue;
        }

        const auto lastSlash = path.find_last_of('/');
        if (lastSlash == std::string_view::npos || lastSlash <= dbPosition) {
            continue;
        }

        const std::string_view filename = path.substr(lastSlash + 1);
        if (!isWorldDbFileName(filename)) {
            continue;
        }

        const std::string dbDir(path.substr(0, lastSlash));
        if (dbDir.empty()) {
            continue;
        }

        if (std::find(outCandidates.begin(), outCandidates.end(), dbDir) ==
            outCandidates.end()) {
            outCandidates.push_back(dbDir);
        }
        if (result.empty()) {
            result = dbDir;
        }
    }

    closedir(dir);
    return result;
}

uint64_t parseFileNumber(const std::filesystem::path &path) {
    const std::string stem = path.stem().string();
    uint64_t value = 0;
    const auto *begin = stem.data();
    const auto *end = begin + stem.size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    if (error == std::errc() && ptr == end) {
        return value;
    }
    return std::numeric_limits<uint64_t>::max();
}

std::string formatU8Counts(const std::unordered_map<uint8_t, uint64_t> &counts) {
    if (counts.empty()) {
        return "none";
    }

    std::vector<std::pair<int, uint64_t>> sorted;
    sorted.reserve(counts.size());
    for (const auto &[value, count] : counts) {
        sorted.emplace_back(static_cast<int>(value), count);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });

    std::string out;
    for (const auto &[value, count] : sorted) {
        if (!out.empty()) {
            out += ", ";
        }
        out += "v";
        out += std::to_string(value);
        out += "=";
        out += std::to_string(count);
    }
    return out;
}

bool isParseErrorColor(const RGB &color) {
    return color.r == kParseErrorColor.r && color.g == kParseErrorColor.g &&
           color.b == kParseErrorColor.b;
}

int32_t floorDiv16(int32_t value) {
    int64_t quotient = static_cast<int64_t>(value) / 16;
    if (value < 0 && value % 16 != 0) {
        --quotient;
    }
    return static_cast<int32_t>(quotient * 16);
}

std::vector<const ChunkColumnData *>
orderedChunks(const std::unordered_map<uint64_t, ChunkColumnData> &chunks) {
    std::vector<const ChunkColumnData *> ordered;
    ordered.reserve(chunks.size());
    for (const auto &[key, chunk] : chunks) {
        (void)key;
        ordered.push_back(&chunk);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const ChunkColumnData *a, const ChunkColumnData *b) {
                  if (a->chunkX != b->chunkX) {
                      return a->chunkX < b->chunkX;
                  }
                  return a->chunkZ < b->chunkZ;
              });
    return ordered;
}

void logWorldScanStats(const ScanState &state) {
    const auto &stats = state.stats;

    LodPocMod::logAll(
        LogLevel::Info,
        "[LOD Reader] LevelDB records: total={}, overworld={}, values={}, "
        "deletions={}, superseded={}, ignoredDimension={}, ignoredOther={}, "
        "malformedBlocks={}",
        stats.totalRecords, stats.overworldRecords, stats.valueRecords,
        stats.deletionRecords, stats.supersededRecords,
        stats.ignoredDimensionRecords, stats.ignoredOtherRecords,
        stats.malformedDataBlocks);

    LodPocMod::logAll(
        LogLevel::Info,
        "[LOD Reader] Chunk tags: Data3D={}, SubChunk={}, "
        "Data2D(unsupported)={}",
        stats.data3dRecords, stats.subChunkRecords, stats.data2dRecords);

    LodPocMod::logAll(
        LogLevel::Info,
        "[LOD Reader] SubChunk versions: v8={}, v9={}; skipped: {}",
        stats.subChunkVersions.count(8) ? stats.subChunkVersions.at(8) : 0,
        stats.subChunkVersions.count(9) ? stats.subChunkVersions.at(9) : 0,
        formatU8Counts(stats.skippedSubChunkVersions));

    LodPocMod::logAll(
        LogLevel::Info,
        "[LOD Reader] SubChunk parse errors={}, Y-index mismatches={}, "
        "out-of-range skipped={}",
        stats.subChunkParseErrors, stats.yIndexMismatches,
        stats.outOfRangeSubChunks);

    LodPocMod::logAll(
        LogLevel::Info,
        "[LOD Reader] Finalized {} chunks ({} columns): resolved={}, "
        "unresolved={}, parse-error={}",
        stats.chunksFinalized, stats.chunksFinalized * 256,
        stats.resolvedColumns, stats.unresolvedColumns,
        stats.parseErrorColumns);
}

void logData3DCrossChecks(
    const std::vector<const ChunkColumnData *> &ordered) {
    int checked = 0;
    for (const ChunkColumnData *chunk : ordered) {
        if (checked >= 10) {
            break;
        }
        if (!chunk->hasData3D) {
            continue;
        }

        int resolved = 0;
        int64_t diffSum = 0;
        int minDiff = INT_MAX;
        int maxDiff = INT_MIN;
        int sampleColumn = -1;

        for (int column = 0; column < 256; ++column) {
            if (chunk->resolved[column] == 0) {
                continue;
            }
            const int raw = static_cast<int>(chunk->data3dHeights[column]);
            const int top = static_cast<int>(chunk->heights[column]);
            const int diff = top - raw;
            diffSum += diff;
            minDiff = std::min(minDiff, diff);
            maxDiff = std::max(maxDiff, diff);
            if (sampleColumn < 0) {
                sampleColumn = column;
            }
            resolved++;
        }

        if (resolved == 0) {
            LodPocMod::logAll(
                LogLevel::Info,
                "[LOD Reader] Data3D cross-check chunk ({}, {}): no resolved "
                "columns",
                chunk->chunkX, chunk->chunkZ);
            checked++;
            continue;
        }

        const int localX = (sampleColumn >> 4) & 0x0F;
        const int localZ = sampleColumn & 0x0F;
        const int raw = static_cast<int>(chunk->data3dHeights[sampleColumn]);
        const int top = static_cast<int>(chunk->heights[sampleColumn]);
        const double average =
            static_cast<double>(diffSum) / static_cast<double>(resolved);

        LodPocMod::logAll(
            LogLevel::Info,
            "[LOD Reader] Data3D cross-check chunk ({}, {}): sample "
            "col=({}, {}) raw={} topY={} diff(top-raw)={} "
            "diffAfterMinus64={}; resolved={} avgDiff={:.2f} minDiff={} "
            "maxDiff={}",
            chunk->chunkX, chunk->chunkZ, localX, localZ, raw, top, top - raw,
            top - (raw - 64), resolved, average, minDiff, maxDiff);
        checked++;
    }

    if (checked == 0) {
        LodPocMod::logAll(LogLevel::Info,
                          "[LOD Reader] Data3D cross-check: no matching chunks");
    }
}

void logSampleChunks(const std::vector<const ChunkColumnData *> &ordered) {
    int samples = 0;
    for (const ChunkColumnData *chunk : ordered) {
        if (samples >= 5) {
            break;
        }

        static constexpr int kColumn = (8 << 4) | 8;
        std::string name;
        if (chunk->resolved[kColumn] != 0) {
            name = LdbReader::blockNameById(chunk->topBlockIds[kColumn]);
        } else if (isParseErrorColor(chunk->colors[kColumn])) {
            name = "<parse_error>";
        } else {
            name = "<unresolved>";
        }

        LodPocMod::logAll(
            LogLevel::Info,
            "[LOD Reader] Sample chunk ({}, {}): top block at column (8,8) = "
            "{} y={}",
            chunk->chunkX, chunk->chunkZ, name,
            static_cast<int>(chunk->heights[kColumn]));
        samples++;
    }

    if (samples == 0) {
        LodPocMod::logAll(LogLevel::Info,
                          "[LOD Reader] Sample chunks: none");
    }
}

void logTopBlockNames(
    const std::unordered_map<uint64_t, ChunkColumnData> &chunks) {
    std::unordered_map<uint16_t, uint64_t> counts;
    for (const auto &[key, chunk] : chunks) {
        (void)key;
        for (int column = 0; column < 256; ++column) {
            if (chunk.resolved[column] != 0) {
                counts[chunk.topBlockIds[column]]++;
            }
        }
    }

    if (counts.empty()) {
        LodPocMod::logAll(LogLevel::Info,
                          "[LOD Reader] Top top-block names: none");
        return;
    }

    std::vector<std::pair<uint16_t, uint64_t>> sorted;
    sorted.reserve(counts.size());
    for (const auto &[id, count] : counts) {
        sorted.emplace_back(id, count);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const auto &a, const auto &b) {
                  if (a.second != b.second) {
                      return a.second > b.second;
                  }
                  return LdbReader::blockNameById(a.first) <
                         LdbReader::blockNameById(b.first);
              });

    const size_t limit = std::min<size_t>(sorted.size(), 20);
    for (size_t i = 0; i < limit; ++i) {
        LodPocMod::logAll(
            LogLevel::Info, "[LOD Reader] Top block #{}: {} = {} columns",
            i + 1, LdbReader::blockNameById(sorted[i].first),
            sorted[i].second);
    }
}

} // namespace

void WorldReader::setRescanIntervalSec(int seconds) {
    const int normalized = std::max(0, seconds);
    const int previous = gRescanIntervalSec.exchange(normalized);
    if (normalized <= 0) {
        gRestartRequested.store(false);
    } else if (previous == 0) {
        // If the one-shot worker already finished, let the next
        // startBackgroundScan() call start a rescan-capable worker.
        gRestartRequested.store(true);
    }
    if (previous != normalized) {
        LodPocMod::logAll(
            LogLevel::Info,
            "[LOD Reader] rescanIntervalSec={}s (0 = scan once)",
            normalized);
    }
}

void WorldReader::startBackgroundScan(const std::filesystem::path &primaryDir,
                                      int rescanIntervalSec) {
    setRescanIntervalSec(rescanIntervalSec);

    if (gLaunched.load()) {
        if (gRunning.load() || !gRestartRequested.exchange(false)) {
            return;
        }
        gLaunched.store(false);
    }

    bool expected = false;
    if (!gLaunched.compare_exchange_strong(expected, true)) {
        return;
    }
    gRestartRequested.store(false);
    gRunning.store(true);

    try {
        std::thread([primaryDir]() {
            try {
                std::vector<std::string> candidates;
                std::string dbDir;

                const auto startedAt = Clock::now();
                auto lastWaitLog = startedAt;

                LodPocMod::logAll(
                    LogLevel::Info,
                    "[LOD Reader] waiting for world... polling /proc/self/fd "
                    "every 3s (timeout 15 min)");

                while (true) {
                    candidates.clear();
                    dbDir = findActiveWorldDb(candidates);
                    if (!dbDir.empty()) {
                        break;
                    }

                    const auto now = Clock::now();
                    if (now - startedAt >= kWaitTimeout) {
                        LodPocMod::logAll(
                            LogLevel::Warn,
                            "[LOD Reader] Timed out after 15 minutes waiting for "
                            "world... (candidate DBs={})",
                            candidates.size());
                        gRunning.store(false);
                        return;
                    }

                    if (now - lastWaitLog >= kWaitLogInterval) {
                        const auto elapsedSeconds =
                            std::chrono::duration_cast<std::chrono::seconds>(
                                now - startedAt)
                                .count();
                        LodPocMod::logAll(
                            LogLevel::Info,
                            "[LOD Reader] waiting for world... elapsed={}s "
                            "candidate DBs={}",
                            elapsedSeconds, candidates.size());
                        lastWaitLog = now;
                    }

                    std::this_thread::sleep_for(kPollInterval);
                }

                LodPocMod::logAll(
                    LogLevel::Info,
                    "[LOD Reader] Discovered active world DB directory: {}",
                    dbDir);

                WorldReader::runScan(primaryDir, dbDir);

                while (true) {
                    int interval = gRescanIntervalSec.load();
                    if (interval <= 0) {
                        break;
                    }

                    auto nextRescan = Clock::now() + std::chrono::seconds(interval);
                    while (Clock::now() < nextRescan) {
                        std::this_thread::sleep_for(kRescanSleepSlice);
                        const int updated = gRescanIntervalSec.load();
                        if (updated <= 0) {
                            interval = 0;
                            break;
                        }
                        if (updated != interval) {
                            interval = updated;
                            nextRescan = Clock::now() + std::chrono::seconds(interval);
                        }
                    }

                    if (interval <= 0 || gRescanIntervalSec.load() <= 0) {
                        break;
                    }

                    std::vector<std::string> rescannedCandidates;
                    const std::string newDbDir =
                        findActiveWorldDb(rescannedCandidates);
                    if (newDbDir.empty()) {
                        LodPocMod::logAll(
                            LogLevel::Warn,
                            "[LOD Reader] World DB is no longer open; stopping "
                            "rescans");
                        break;
                    }

                    if (newDbDir != dbDir) {
                        LodPocMod::logAll(
                            LogLevel::Info,
                            "[LOD Reader] Active world DB changed: {} -> {}",
                            dbDir, newDbDir);
                        dbDir = newDbDir;
                    }

                    WorldReader::runScan(primaryDir, dbDir);
                }

                LodPocMod::logAll(LogLevel::Info,
                                  "[LOD Reader] Background poller finished");
            } catch (const std::exception &ex) {
                LodPocMod::logAll(
                    LogLevel::Error,
                    "[LOD Reader] Background scan failed: {}", ex.what());
            } catch (...) {
                LodPocMod::logAll(
                    LogLevel::Error,
                    "[LOD Reader] Background scan failed with unknown error");
            }
            gRunning.store(false);
        }).detach();
    } catch (const std::exception &ex) {
        gRunning.store(false);
        gLaunched.store(false);
        LodPocMod::logAll(LogLevel::Error,
                          "[LOD Reader] Failed to start poller thread: {}",
                          ex.what());
    } catch (...) {
        gRunning.store(false);
        gLaunched.store(false);
        LodPocMod::logAll(
            LogLevel::Error,
            "[LOD Reader] Failed to start poller thread: unknown error");
    }
}

void WorldReader::runScan(const std::filesystem::path &primaryDir,
                          const std::string &dbDir) {
    const auto scanStarted = Clock::now();

    LodPocMod::logAll(
        LogLevel::Info,
        "[LOD Reader] Starting Bedrock world scan in {} (background thread)",
        dbDir);

    struct LdbFile {
        std::filesystem::path path;
        uint64_t number{0};
    };

    std::vector<LdbFile> ldbFiles;
    uint64_t totalBytes = 0;
    std::error_code ec;

    for (const auto &entry :
         std::filesystem::directory_iterator(dbDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        if (entry.path().extension() != ".ldb") {
            continue;
        }

        LdbFile file;
        file.path = entry.path();
        file.number = parseFileNumber(file.path);
        ldbFiles.push_back(file);

        const auto size = entry.file_size(ec);
        if (!ec) {
            totalBytes += size;
        }
        ec.clear();
    }

    if (ec) {
        LodPocMod::logAll(LogLevel::Warn,
                          "[LOD Reader] Failed while listing {}: {}",
                          dbDir, ec.message());
    }

    if (ldbFiles.empty()) {
        LodPocMod::logAll(
            LogLevel::Warn,
            "[LOD Reader] No .ldb files found in {} (world may still be in "
            "the WAL memtable)",
            dbDir);
        return;
    }

    std::sort(ldbFiles.begin(), ldbFiles.end(),
              [](const LdbFile &a, const LdbFile &b) {
                  if (a.number != b.number) {
                      return a.number < b.number;
                  }
                  return a.path.string() < b.path.string();
              });

    const double totalMb =
        static_cast<double>(totalBytes) / (1024.0 * 1024.0);
    LodPocMod::logAll(
        LogLevel::Info,
        "[LOD Reader] Found {} .ldb files (total size: {:.2f} MB), scanning "
        "in ascending file-number order",
        ldbFiles.size(), totalMb);

    ScanState state;
    const auto readStarted = Clock::now();

    for (const LdbFile &file : ldbFiles) {
        const bool ok =
            LdbReader::readLdbFile(file.path.string(), file.number, state);
        if (!ok) {
            LodPocMod::logAll(LogLevel::Warn,
                              "[LOD Reader] Failed to read .ldb file: {}",
                              file.path.string());
        }
    }

    const auto readFinished = Clock::now();
    const double readSeconds =
        std::chrono::duration<double>(readFinished - readStarted).count();

    LodPocMod::logAll(
        LogLevel::Info,
        "[LOD Reader] Read {} files ({} ok, {} failed) in {:.2f}s; applying "
        "surviving records",
        ldbFiles.size(), state.stats.filesRead, state.stats.filesFailed,
        readSeconds);

    LdbReader::finalizeChunks(state);

    const auto scanFinished = Clock::now();
    const double totalSeconds =
        std::chrono::duration<double>(scanFinished - scanStarted).count();

    logWorldScanStats(state);

    if (state.chunks.empty()) {
        LodPocMod::logAll(
            LogLevel::Warn,
            "[LOD Reader] Zero Overworld chunks with terrain data parsed.");
        return;
    }

    const auto ordered = orderedChunks(state.chunks);
    logData3DCrossChecks(ordered);
    logSampleChunks(ordered);
    logTopBlockNames(state.chunks);

    int32_t minX = INT32_MAX;
    int32_t maxX = INT32_MIN;
    int32_t minZ = INT32_MAX;
    int32_t maxZ = INT32_MIN;

    for (const ChunkColumnData *chunk : ordered) {
        minX = std::min(minX, chunk->chunkX);
        maxX = std::max(maxX, chunk->chunkX);
        minZ = std::min(minZ, chunk->chunkZ);
        maxZ = std::max(maxZ, chunk->chunkZ);
    }

    LodPocMod::logAll(
        LogLevel::Info,
        "[LOD Reader] World Bounds: Chunk X=[{} .. {}], Chunk Z=[{} .. {}] "
        "(span: {}x{} chunks)",
        minX, maxX, minZ, maxZ, (maxX - minX + 1), (maxZ - minZ + 1));

    const int32_t sampleTileX = floorDiv16(minX);
    const int32_t sampleTileZ = floorDiv16(minZ);

    for (int tier = 1; tier <= 4; ++tier) {
        const auto tileMesh =
            LodMeshBuilder::buildTileMesh(sampleTileX, sampleTileZ, tier,
                                          state.chunks);
        const double memKb =
            static_cast<double>(tileMesh.memoryBytes()) / 1024.0;
        LodPocMod::logAll(
            LogLevel::Info,
            "[LOD Mesh] Sample Tile (chunk {}, {}) Tier {} (cell {}x{}): {} "
            "verts, {} indices, memory: {:.2f} KB",
            sampleTileX, sampleTileZ, tier, tileMesh.cellSize,
            tileMesh.cellSize, tileMesh.vertices.size(),
            tileMesh.indices.size(), memKb);
    }

    const int64_t chunksW = static_cast<int64_t>(maxX) - minX + 1;
    const int64_t chunksH = static_cast<int64_t>(maxZ) - minZ + 1;
    const int64_t rawWidth = chunksW * 16;
    const int64_t rawHeight = chunksH * 16;

    int scale = 1;
    while ((rawWidth / scale) > 1024 || (rawHeight / scale) > 1024) {
        scale *= 2;
    }

    const int imgWidth =
        static_cast<int>(std::max<int64_t>(1, rawWidth / scale));
    const int imgHeight =
        static_cast<int>(std::max<int64_t>(1, rawHeight / scale));

    LodPocMod::logAll(
        LogLevel::Info,
        "[LOD Map] Generating top-down PNG: {}x{} blocks (scale 1:{}, image: "
        "{}x{} px)",
        rawWidth, rawHeight, scale, imgWidth, imgHeight);

    std::vector<uint8_t> pixels(
        static_cast<size_t>(imgWidth) * static_cast<size_t>(imgHeight) * 3u,
        40);

    for (int iy = 0; iy < imgHeight; ++iy) {
        for (int ix = 0; ix < imgWidth; ++ix) {
            const int64_t blockRelX = static_cast<int64_t>(ix) * scale;
            const int64_t blockRelZ = static_cast<int64_t>(iy) * scale;

            const int32_t chunkX =
                minX + static_cast<int32_t>(blockRelX / 16);
            const int32_t chunkZ =
                minZ + static_cast<int32_t>(blockRelZ / 16);

            const int localX = static_cast<int>(blockRelX % 16);
            const int localZ = static_cast<int>(blockRelZ % 16);
            const int column = (localX << 4) | localZ;

            const uint64_t key = makeChunkCoordKey(chunkX, chunkZ);
            const auto found = state.chunks.find(key);
            if (found == state.chunks.end()) {
                continue;
            }

            const RGB color = found->second.colors[column];
            const size_t offset =
                static_cast<size_t>(iy * imgWidth + ix) * 3u;
            pixels[offset + 0] = color.r;
            pixels[offset + 1] = color.g;
            pixels[offset + 2] = color.b;
        }
    }

    const std::string primaryPngPath = (primaryDir / "lod_map.png").string();
    const std::string secondaryPngPath =
        "/storage/emulated/0/Download/lod_map.png";

    if (PngWriter::writeRgbPng(primaryPngPath, imgWidth, imgHeight, pixels)) {
        LodPocMod::logAll(LogLevel::Info,
                          "[LOD Map] Saved primary PNG map next to log: {}",
                          primaryPngPath);
    } else {
        LodPocMod::logAll(LogLevel::Warn,
                          "[LOD Map] Failed to write primary PNG map: {}",
                          primaryPngPath);
    }

    if (std::filesystem::exists("/storage/emulated/0/Download", ec)) {
        if (PngWriter::writeRgbPng(secondaryPngPath, imgWidth, imgHeight,
                                   pixels)) {
            LodPocMod::logAll(LogLevel::Info,
                              "[LOD Map] Copied PNG map to Download: {}",
                              secondaryPngPath);
        } else {
            LodPocMod::logAll(LogLevel::Warn,
                              "[LOD Map] Failed to write Download PNG: {}",
                              secondaryPngPath);
        }
    } else {
        LodPocMod::logAll(
            LogLevel::Warn,
            "[LOD Map] Download directory unavailable; skipped {}",
            secondaryPngPath);
    }

    size_t chunkMemoryBytes = 0;
    for (const auto &[key, chunk] : state.chunks) {
        (void)key;
        chunkMemoryBytes += sizeof(ChunkColumnData);
        chunkMemoryBytes +=
            chunk.subChunks.capacity() * sizeof(SubChunkTopData);
    }
    const size_t pixelMemoryBytes = pixels.size();
    const double peakMemMb =
        static_cast<double>(chunkMemoryBytes + pixelMemoryBytes) /
        (1024.0 * 1024.0);

    LodPocMod::logAll(
        LogLevel::Info,
        "[LOD Reader] Complete: {} chunks parsed in {:.2f}s, approximate peak "
        "memory: {:.2f} MB",
        state.chunks.size(), totalSeconds, peakMemMb);
}

} // namespace lod_poc
