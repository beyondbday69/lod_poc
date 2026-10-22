#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lod_poc {

struct RGB {
    uint8_t r{128};
    uint8_t g{128};
    uint8_t b{128};
};

struct ChunkColumnData {
    int32_t chunkX{0};
    int32_t chunkZ{0};
    int16_t heights[256]{};
    RGB colors[256]{};
    bool hasData3D{false};

    ChunkColumnData();
};

inline uint64_t makeChunkCoordKey(int32_t x, int32_t z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
           static_cast<uint64_t>(static_cast<uint32_t>(z));
}

inline void unpackChunkCoordKey(uint64_t key, int32_t &x, int32_t &z) {
    x = static_cast<int32_t>(key >> 32);
    z = static_cast<int32_t>(key & 0xFFFFFFFF);
}

RGB getBlockColor(std::string_view blockName);

/**
 * @brief Standalone LevelDB SSTable (.ldb) reader for Minecraft Bedrock.
 *
 * Directly reads immutable .ldb files without requiring database locks or external LevelDB binaries.
 */
class LdbReader {
  public:
    static bool readLdbFile(const std::string &filePath,
                            std::unordered_map<uint64_t, ChunkColumnData> &outChunks);
};

} // namespace lod_poc
