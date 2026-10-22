#include "mod/WorldReader.h"
#include "mod/LdbReader.h"
#include "mod/LodMesh.h"
#include "mod/LodPocMod.h"
#include "mod/PngWriter.h"

#include <chrono>
#include <climits>
#include <cmath>
#include <dirent.h>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace lod_poc {
namespace {

std::string findActiveWorldDb(std::vector<std::string> &outCandidates) {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
        return "";
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char linkPath[256];
        snprintf(linkPath, sizeof(linkPath), "/proc/self/fd/%s", entry->d_name);
        char target[1024];
        const ssize_t len = readlink(linkPath, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            std::string_view path(target);
            if (path.find("/minecraftWorlds/") != std::string_view::npos ||
                path.find("/db/") != std::string_view::npos) {
                outCandidates.emplace_back(path);
            }
            const auto dbPos = path.find("/db/");
            if (dbPos != std::string_view::npos) {
                const auto lastSlash = path.find_last_of('/');
                const std::string_view filename = path.substr(lastSlash + 1);
                if (filename.ends_with(".ldb") || filename.ends_with(".log") ||
                    filename.starts_with("MANIFEST") || filename == "CURRENT") {
                    closedir(dir);
                    return std::string(path.substr(0, lastSlash));
                }
            }
        }
    }
    closedir(dir);
    return "";
}

} // namespace

void WorldReader::startBackgroundScan(const std::filesystem::path &primaryDir) {
    std::thread([primaryDir]() {
        // Sleep 4 seconds to allow the world to fully load and LevelDB file descriptors to open
        std::this_thread::sleep_for(std::chrono::seconds(4));
        runScan(primaryDir);
    }).detach();
}

void WorldReader::runScan(const std::filesystem::path &primaryDir) {
    LodPocMod::logAll(LogLevel::Info, "[LOD Reader] Starting background Bedrock world scan...");

    std::vector<std::string> candidates;
    const std::string dbDir = findActiveWorldDb(candidates);

    if (dbDir.empty()) {
        LodPocMod::logAll(LogLevel::Warn,
            "[LOD Reader] No active world DB found in open file descriptors. Checked {} candidate files.",
            candidates.size());
        for (size_t i = 0; i < std::min<size_t>(candidates.size(), 8); ++i) {
            LodPocMod::logAll(LogLevel::Warn, "[LOD Reader] Candidate [{}]: {}", i, candidates[i]);
        }
        return;
    }

    LodPocMod::logAll(LogLevel::Info, "[LOD Reader] Discovered active world DB directory: {}", dbDir);
    LodPocMod::logAll(LogLevel::Info,
        "[LOD Reader] LevelDB Strategy: Strategy A - Direct immutable .ldb read (lockless, zero-copy, read-only)");

    // Gather all .ldb files
    std::vector<std::filesystem::path> ldbFiles;
    uint64_t totalBytes = 0;
    std::error_code ec;

    for (const auto &entry : std::filesystem::directory_iterator(dbDir, ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".ldb") {
            ldbFiles.push_back(entry.path());
            totalBytes += entry.file_size(ec);
        }
    }

    if (ldbFiles.empty()) {
        LodPocMod::logAll(LogLevel::Warn,
            "[LOD Reader] No .ldb files found in {} (world may be brand new in WAL memtable)", dbDir);
        return;
    }

    const double totalMb = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
    LodPocMod::logAll(LogLevel::Info,
        "[LOD Reader] Found {} .ldb files (total size: {:.2f} MB)", ldbFiles.size(), totalMb);

    const auto startTime = std::chrono::steady_clock::now();
    std::unordered_map<uint64_t, ChunkColumnData> chunks;

    for (const auto &ldbPath : ldbFiles) {
        LdbReader::readLdbFile(ldbPath.string(), chunks);
    }

    const auto endTime = std::chrono::steady_clock::now();
    const double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();
    const double chunksPerSec = (elapsedSec > 0.0) ? (static_cast<double>(chunks.size()) / elapsedSec) : 0.0;

    LodPocMod::logAll(LogLevel::Info,
        "[LOD Reader] Successfully scanned {} Overworld chunks in {:.2f} s ({:.1f} chunks/sec)",
        chunks.size(), elapsedSec, chunksPerSec);

    if (chunks.empty()) {
        LodPocMod::logAll(LogLevel::Warn, "[LOD Reader] Zero Overworld chunks with terrain data parsed.");
        return;
    }

    // Determine chunk bounding box
    int32_t minX = INT32_MAX, maxX = INT32_MIN;
    int32_t minZ = INT32_MAX, maxZ = INT32_MIN;

    for (const auto &[coordKey, data] : chunks) {
        minX = std::min(minX, data.chunkX);
        maxX = std::max(maxX, data.chunkX);
        minZ = std::min(minZ, data.chunkZ);
        maxZ = std::max(maxZ, data.chunkZ);
    }

    LodPocMod::logAll(LogLevel::Info,
        "[LOD Reader] World Bounds: Chunk X=[{} .. {}], Chunk Z=[{} .. {}] (span: {}x{} chunks)",
        minX, maxX, minZ, maxZ, (maxX - minX + 1), (maxZ - minZ + 1));

    // -------------------------------------------------------------------------
    // Build Sample LOD Tier Meshes
    // -------------------------------------------------------------------------
    const int32_t sampleTileX = (minX / 16) * 16;
    const int32_t sampleTileZ = (minZ / 16) * 16;

    for (int tier = 1; tier <= 4; ++tier) {
        const auto tileMesh = LodMeshBuilder::buildTileMesh(sampleTileX, sampleTileZ, tier, chunks);
        const double memKb = static_cast<double>(tileMesh.memoryBytes()) / 1024.0;
        LodPocMod::logAll(LogLevel::Info,
            "[LOD Mesh] Sample Tile (chunk {}, {}) Tier {} (cell {}x{}): {} verts, {} indices, memory: {:.2f} KB",
            sampleTileX, sampleTileZ, tier, tileMesh.cellSize, tileMesh.cellSize,
            tileMesh.vertices.size(), tileMesh.indices.size(), memKb);
    }

    // -------------------------------------------------------------------------
    // Generate Debug Top-Down Map PNG
    // -------------------------------------------------------------------------
    const int chunksW = maxX - minX + 1;
    const int chunksH = maxZ - minZ + 1;
    const int rawWidth = chunksW * 16;
    const int rawHeight = chunksH * 16;

    // Scale down if map exceeds 1024x1024
    int scale = 1;
    while ((rawWidth / scale) > 1024 || (rawHeight / scale) > 1024) {
        scale *= 2;
    }

    const int imgWidth = std::max(1, rawWidth / scale);
    const int imgHeight = std::max(1, rawHeight / scale);

    LodPocMod::logAll(LogLevel::Info,
        "[LOD Map] Generating top-down PNG: {}x{} blocks (scale 1:{}, image: {}x{} px)",
        rawWidth, rawHeight, scale, imgWidth, imgHeight);

    std::vector<uint8_t> pixels(static_cast<size_t>(imgWidth * imgHeight * 3), 40); // dark background

    for (int iy = 0; iy < imgHeight; ++iy) {
        for (int ix = 0; ix < imgWidth; ++ix) {
            const int blockRelX = ix * scale;
            const int blockRelZ = iy * scale;

            const int32_t cX = minX + (blockRelX / 16);
            const int32_t cZ = minZ + (blockRelZ / 16);

            const int localX = blockRelX % 16;
            const int localZ = blockRelZ % 16;
            const int colIdx = (localZ << 4) | localX;

            const uint64_t cKey = makeChunkCoordKey(cX, cZ);
            auto it = chunks.find(cKey);
            if (it != chunks.end()) {
                const auto col = it->second.colors[colIdx];
                const size_t pOffset = static_cast<size_t>(iy * imgWidth + ix) * 3;
                pixels[pOffset + 0] = col.r;
                pixels[pOffset + 1] = col.g;
                pixels[pOffset + 2] = col.b;
            }
        }
    }

    const auto primaryPngPath = (primaryDir / "lod_map.png").string();
    const std::string secondaryPngPath = "/storage/emulated/0/Download/lod_map.png";

    if (PngWriter::writeRgbPng(primaryPngPath, imgWidth, imgHeight, pixels)) {
        LodPocMod::logAll(LogLevel::Info, "[LOD Map] Saved primary PNG map to: {}", primaryPngPath);
    } else {
        LodPocMod::logAll(LogLevel::Warn, "[LOD Map] Failed to write primary PNG map to: {}", primaryPngPath);
    }

    if (std::filesystem::exists("/storage/emulated/0/Download", ec)) {
        if (PngWriter::writeRgbPng(secondaryPngPath, imgWidth, imgHeight, pixels)) {
            LodPocMod::logAll(LogLevel::Info, "[LOD Map] Copied PNG map to Download: {}", secondaryPngPath);
        }
    }

    // Estimate peak memory
    const size_t chunkMemoryBytes = chunks.size() * sizeof(ChunkColumnData);
    const size_t pixelMemoryBytes = pixels.size();
    const double peakMemMb = static_cast<double>(chunkMemoryBytes + pixelMemoryBytes) / (1024.0 * 1024.0);

    LodPocMod::logAll(LogLevel::Info,
        "[LOD Reader] Complete: {} chunks parsed, peak memory: {:.2f} MB",
        chunks.size(), peakMemMb);
}

} // namespace lod_poc
