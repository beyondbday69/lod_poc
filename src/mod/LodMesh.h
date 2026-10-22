#pragma once

#include "mod/LdbReader.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lod_poc {

struct LodVertex {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    uint8_t r{128};
    uint8_t g{128};
    uint8_t b{128};
    uint8_t a{255};
};

struct LodTileMesh {
    int32_t tileChunkX{0};
    int32_t tileChunkZ{0};
    int tier{1};
    int cellSize{2};
    std::vector<LodVertex> vertices;
    std::vector<uint32_t> indices;

    [[nodiscard]] size_t memoryBytes() const {
        return sizeof(LodTileMesh) +
               vertices.size() * sizeof(LodVertex) +
               indices.size() * sizeof(uint32_t);
    }
};

class LodMeshBuilder {
  public:
    /**
     * @brief Builds a low-poly heightfield tile mesh for a 16x16-chunk area with edge skirts.
     *
     * @param tileChunkX Starting chunk X (multiple of 16)
     * @param tileChunkZ Starting chunk Z (multiple of 16)
     * @param tier LOD tier (1: 2x2, 2: 4x4, 3: 8x8, 4: 16x16)
     * @param chunks Extracted Overworld chunk database
     */
    static LodTileMesh buildTileMesh(int32_t tileChunkX, int32_t tileChunkZ, int tier,
                                     const std::unordered_map<uint64_t, ChunkColumnData> &chunks);

    /**
     * @brief Builds low-poly heightfield tile meshes around the player.
     *
     * @param tier LOD tier (1: 2x2, 2: 4x4, 3: 8x8, 4: 16x16)
     * @param playerWorldX Player world X coordinate
     * @param playerWorldZ Player world Z coordinate
     * @param chunks Extracted Overworld chunk database
     */
    static std::vector<LodTileMesh> buildAllTiles(
        int tier,
        float playerWorldX,
        float playerWorldZ,
        const std::unordered_map<uint64_t, ChunkColumnData> &chunks);
};

} // namespace lod_poc
