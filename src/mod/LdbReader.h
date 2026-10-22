#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace lod_poc {

struct RGB {
    uint8_t r{128};
    uint8_t g{128};
    uint8_t b{128};

    bool operator==(const RGB &o) const {
        return r == o.r && g == o.g && b == o.b;
    }
    bool operator!=(const RGB &o) const {
        return !(*this == o);
    }
};

constexpr RGB kUnresolvedColor{64, 64, 64};
constexpr RGB kParseErrorColor{255, 0, 255};

constexpr uint16_t kNoBlockId = 0;
constexpr uint16_t kUnknownBlockId = 1;

/**
 * @brief Per-column top candidate for one SubChunk.
 *
 * A zero name id means no non-air, non-decoration block was found in this
 * SubChunk for that column. topLocalYs is the local 0..15 Y inside the
 * SubChunk. The global Y is subY * 16 + topLocalYs.
 */
struct SubChunkTopData {
    int8_t subY{0};
    uint8_t version{0};

    /**
     * True when this SubChunk existed but could not be parsed. Its top block
     * is unknown, so columns at or below this Y must be shown as parse errors
     * instead of silently falling through to a lower SubChunk.
     */
    bool parseError{false};

    std::array<uint16_t, 256> topNameIds{};
    std::array<int8_t, 256> topLocalYs{};
};

struct ChunkColumnData {
    int32_t chunkX{0};
    int32_t chunkZ{0};

    /**
     * Per-column data. Index = (localX << 4) | localZ, matching Bedrock's
     * SubChunk block order (x << 8) | (z << 4) | y and Data3D ZX order.
     */
    /** Absolute top block Y for each column, or a Data3D fallback. */
    std::array<int16_t, 256> heights{};

    /** Final map color for each column. */
    std::array<RGB, 256> colors{};

    /** 1 when a real top block was resolved from SubChunk data. */
    std::array<uint8_t, 256> resolved{};

    /** Interned top block name id, 0 when unresolved. */
    std::array<uint16_t, 256> topBlockIds{};

    bool hasData3D{false};
    std::array<int16_t, 256> data3dHeights{};

    /** True when any relevant record for this chunk failed to parse. */
    bool parseError{false};

    std::vector<SubChunkTopData> subChunks;

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

struct Data3DPayload {
    std::array<int16_t, 256> heights{};
};

struct SubChunkPayload {
    SubChunkTopData data{};
};

using RecordPayload = std::variant<std::monostate, Data3DPayload, SubChunkPayload>;

/**
 * @brief Winning LevelDB record for one user key.
 *
 * The value is decoded while files are read, but it is not applied to a chunk
 * until every file has been processed. That keeps the build independent of
 * file order and lets a newer sequence or a deletion replace an older value.
 */
struct PendingRecord {
    uint64_t sequence{0};
    uint64_t fileNumber{0};
    uint8_t valueType{1}; // 1 = value, 0 = deletion
    uint8_t tag{0};
    int32_t chunkX{0};
    int32_t chunkZ{0};
    int8_t subY{0};
    bool hasSubY{false};
    bool parseError{false};
    RecordPayload payload;
};

struct ScanStats {
    uint64_t filesRead{0};
    uint64_t filesFailed{0};

    uint64_t totalRecords{0};
    uint64_t overworldRecords{0};
    uint64_t ignoredDimensionRecords{0};
    uint64_t ignoredOtherRecords{0};
    uint64_t valueRecords{0};
    uint64_t deletionRecords{0};
    uint64_t supersededRecords{0};
    uint64_t malformedDataBlocks{0};

    uint64_t data3dRecords{0};
    uint64_t data2dRecords{0};
    uint64_t subChunkRecords{0};
    uint64_t subChunkParseErrors{0};
    uint64_t yIndexMismatches{0};
    uint64_t outOfRangeSubChunks{0};

    uint64_t chunksFinalized{0};
    uint64_t resolvedColumns{0};
    uint64_t unresolvedColumns{0};
    uint64_t parseErrorColumns{0};

    std::unordered_map<uint8_t, uint64_t> subChunkVersions;
    std::unordered_map<uint8_t, uint64_t> skippedSubChunkVersions;
    std::unordered_map<uint8_t, uint64_t> ignoredTags;
};

struct ScanState {
    std::unordered_map<std::string, PendingRecord> records;
    std::unordered_map<uint64_t, ChunkColumnData> chunks;
    ScanStats stats;
    bool hasPlayerPos{false};
    float playerPos[3]{0.0f, 0.0f, 0.0f};
};

/**
 * @brief Attempts to extract 3D world player coordinates from an uncompressed NBT buffer
 * (e.g. Bedrock ~local_player or level.dat).
 */
bool extractPlayerPos(const uint8_t *val, size_t len, float pos[3]);

/**
 * @brief Standalone LevelDB SSTable (.ldb) reader for Minecraft Bedrock.
 *
 * Directly reads immutable .ldb files without requiring database locks or
 * external LevelDB binaries. LevelDB internal keys are stripped of their
 * 8-byte trailer and only the highest sequence per user key survives.
 */
class LdbReader {
  public:
    static bool readLdbFile(const std::string &filePath, uint64_t fileNumber,
                            ScanState &state);

    /**
     * @brief Applies surviving records and computes top blocks/colors.
     *
     * Must be called after all .ldb files have been read. It is safe to call
     * with an empty state.
     */
    static void finalizeChunks(ScanState &state);

    [[nodiscard]] static const std::string &blockNameById(uint16_t id);
};

} // namespace lod_poc
