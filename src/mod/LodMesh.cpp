#include "mod/LodMesh.h"

#include <algorithm>

namespace lod_poc {
namespace {

int getCellSizeForTier(int tier) {
    switch (tier) {
        case 1: return 2;   // 2x2 blocks (chunks 6-16)
        case 2: return 4;   // 4x4 blocks (chunks 17-32)
        case 3: return 8;   // 8x8 blocks (chunks 33-64)
        case 4: return 16;  // 16x16 blocks (chunks 65+)
        default: return 2;
    }
}

} // namespace

LodTileMesh LodMeshBuilder::buildTileMesh(
    int32_t tileChunkX, int32_t tileChunkZ, int tier,
    const std::unordered_map<uint64_t, ChunkColumnData> &chunks) {

    LodTileMesh mesh;
    mesh.tileChunkX = tileChunkX;
    mesh.tileChunkZ = tileChunkZ;
    mesh.tier = tier;
    mesh.cellSize = getCellSizeForTier(tier);

    // Each tile is 16x16 chunks = 256x256 blocks
    const int tileSizeBlocks = 256;
    const int gridSize = tileSizeBlocks / mesh.cellSize;
    const int vertGridSize = gridSize + 1;

    const float tileBaseX = static_cast<float>(tileChunkX * 16);
    const float tileBaseZ = static_cast<float>(tileChunkZ * 16);

    // 1. Sample downsampled height and color per cell vertex
    std::vector<float> gridHeights(vertGridSize * vertGridSize, 64.0f);
    // Missing/unresolved terrain must never look like valid grass.
    std::vector<RGB> gridColors(vertGridSize * vertGridSize, kUnresolvedColor);

    for (int gz = 0; gz < vertGridSize; ++gz) {
        for (int gx = 0; gx < vertGridSize; ++gx) {
            const int blockX = std::min(gx * mesh.cellSize, tileSizeBlocks - 1);
            const int blockZ = std::min(gz * mesh.cellSize, tileSizeBlocks - 1);

            const int chunkRelX = blockX / 16;
            const int chunkRelZ = blockZ / 16;
            const int32_t targetChunkX = tileChunkX + chunkRelX;
            const int32_t targetChunkZ = tileChunkZ + chunkRelZ;

            const int localBlockX = blockX % 16;
            const int localBlockZ = blockZ % 16;
            // World reader stores columns as (x << 4) | z.
            const int colIdx = (localBlockX << 4) | localBlockZ;

            const uint64_t cKey = makeChunkCoordKey(targetChunkX, targetChunkZ);
            auto it = chunks.find(cKey);
            if (it != chunks.end()) {
                gridHeights[gz * vertGridSize + gx] = static_cast<float>(it->second.heights[colIdx]);
                gridColors[gz * vertGridSize + gx] = it->second.colors[colIdx];
            }
        }
    }

    // 2. Build surface vertices
    mesh.vertices.reserve(vertGridSize * vertGridSize + vertGridSize * 4);
    for (int gz = 0; gz < vertGridSize; ++gz) {
        for (int gx = 0; gx < vertGridSize; ++gx) {
            const size_t idx = gz * vertGridSize + gx;
            LodVertex v{};
            v.x = tileBaseX + static_cast<float>(gx * mesh.cellSize);
            v.y = gridHeights[idx];
            v.z = tileBaseZ + static_cast<float>(gz * mesh.cellSize);
            v.r = gridColors[idx].r;
            v.g = gridColors[idx].g;
            v.b = gridColors[idx].b;
            v.a = 255;
            mesh.vertices.push_back(v);
        }
    }

    // 3. Build surface indices (triangles)
    mesh.indices.reserve(gridSize * gridSize * 6 + gridSize * 24);
    for (int gz = 0; gz < gridSize; ++gz) {
        for (int gx = 0; gx < gridSize; ++gx) {
            const uint32_t v00 = static_cast<uint32_t>(gz * vertGridSize + gx);
            const uint32_t v10 = static_cast<uint32_t>(gz * vertGridSize + (gx + 1));
            const uint32_t v01 = static_cast<uint32_t>((gz + 1) * vertGridSize + gx);
            const uint32_t v11 = static_cast<uint32_t>((gz + 1) * vertGridSize + (gx + 1));

            // Triangle 1
            mesh.indices.push_back(v00);
            mesh.indices.push_back(v10);
            mesh.indices.push_back(v01);

            // Triangle 2
            mesh.indices.push_back(v10);
            mesh.indices.push_back(v11);
            mesh.indices.push_back(v01);
        }
    }

    // 4. Build skirts at the 4 outer tile edges to hide cracks
    static constexpr float kSkirtDrop = 32.0f;

    auto addSkirtEdge = [&](const std::vector<uint32_t> &topEdgeIndices) {
        for (size_t i = 0; i + 1 < topEdgeIndices.size(); ++i) {
            const uint32_t topA = topEdgeIndices[i];
            const uint32_t topB = topEdgeIndices[i + 1];

            LodVertex botA = mesh.vertices[topA];
            botA.y -= kSkirtDrop;
            const uint32_t idxBotA = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(botA);

            LodVertex botB = mesh.vertices[topB];
            botB.y -= kSkirtDrop;
            const uint32_t idxBotB = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(botB);

            // Skirt Quad
            mesh.indices.push_back(topA);
            mesh.indices.push_back(topB);
            mesh.indices.push_back(idxBotA);

            mesh.indices.push_back(topB);
            mesh.indices.push_back(idxBotB);
            mesh.indices.push_back(idxBotA);
        }
    };

    // North edge (gz = 0)
    std::vector<uint32_t> northEdge;
    for (int gx = 0; gx < vertGridSize; ++gx) northEdge.push_back(gx);
    addSkirtEdge(northEdge);

    // South edge (gz = gridSize)
    std::vector<uint32_t> southEdge;
    for (int gx = 0; gx < vertGridSize; ++gx) southEdge.push_back(gridSize * vertGridSize + gx);
    addSkirtEdge(southEdge);

    // West edge (gx = 0)
    std::vector<uint32_t> westEdge;
    for (int gz = 0; gz < vertGridSize; ++gz) westEdge.push_back(gz * vertGridSize);
    addSkirtEdge(westEdge);

    // East edge (gx = gridSize)
    std::vector<uint32_t> eastEdge;
    for (int gz = 0; gz < vertGridSize; ++gz) eastEdge.push_back(gz * vertGridSize + gridSize);
    addSkirtEdge(eastEdge);

    return mesh;
}

} // namespace lod_poc
