#include "mod/LdbReader.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <zlib.h>

namespace lod_poc {
namespace {

constexpr size_t kMaxDecompressedBlockSize = 64ull * 1024ull * 1024ull;
constexpr int kMaxNbtDepth = 64;
constexpr uint32_t kMaxPaletteEntries = 65535;

inline uint16_t readLe16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t readLe32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline int32_t readLeSigned32(const uint8_t *p) {
    return static_cast<int32_t>(readLe32(p));
}

inline uint64_t readLe64(const uint8_t *p) {
    return static_cast<uint64_t>(readLe32(p)) |
           (static_cast<uint64_t>(readLe32(p + 4)) << 32);
}

inline bool readVarint32(const uint8_t *&ptr, const uint8_t *limit, uint32_t &val) {
    val = 0;
    int shift = 0;
    while (ptr < limit && shift <= 28) {
        const uint8_t b = *ptr++;
        val |= static_cast<uint32_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            return true;
        }
        shift += 7;
    }
    return false;
}

inline bool readVarint64(const uint8_t *&ptr, const uint8_t *limit, uint64_t &val) {
    val = 0;
    int shift = 0;
    while (ptr < limit && shift <= 63) {
        const uint8_t b = *ptr++;
        val |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            return true;
        }
        shift += 7;
    }
    return false;
}

inline bool safeAdd64(uint64_t a, uint64_t b, uint64_t &out) {
    if (a > std::numeric_limits<uint64_t>::max() - b) {
        return false;
    }
    out = a + b;
    return true;
}

struct BlockHandle {
    uint64_t offset{0};
    uint64_t size{0};

    bool decode(const uint8_t *&ptr, const uint8_t *limit) {
        return readVarint64(ptr, limit, offset) && readVarint64(ptr, limit, size);
    }
};

// -----------------------------------------------------------------------------
// Bounds-checked byte reader
// -----------------------------------------------------------------------------

class ByteReader {
  public:
    ByteReader(const uint8_t *data, size_t size)
        : mPtr(data), mEnd(data + size) {}

    [[nodiscard]] const uint8_t *ptr() const { return mPtr; }
    [[nodiscard]] size_t remaining() const {
        return static_cast<size_t>(mEnd - mPtr);
    }

    bool readU8(uint8_t &value) {
        if (mPtr >= mEnd) {
            return false;
        }
        value = *mPtr++;
        return true;
    }

    bool readU16(uint16_t &value) {
        if (remaining() < 2) {
            return false;
        }
        value = readLe16(mPtr);
        mPtr += 2;
        return true;
    }

    bool readU32(uint32_t &value) {
        if (remaining() < 4) {
            return false;
        }
        value = readLe32(mPtr);
        mPtr += 4;
        return true;
    }

    bool readI32(int32_t &value) {
        uint32_t raw = 0;
        if (!readU32(raw)) {
            return false;
        }
        value = static_cast<int32_t>(raw);
        return true;
    }

    bool readU64(uint64_t &value) {
        if (remaining() < 8) {
            return false;
        }
        value = readLe64(mPtr);
        mPtr += 8;
        return true;
    }

    bool skip(size_t count) {
        if (count > remaining()) {
            return false;
        }
        mPtr += count;
        return true;
    }

    bool readString(std::string &out, size_t maxLength = 1024 * 1024) {
        uint16_t length = 0;
        if (!readU16(length)) {
            return false;
        }
        if (static_cast<size_t>(length) > maxLength ||
            static_cast<size_t>(length) > remaining()) {
            return false;
        }
        out.assign(reinterpret_cast<const char *>(mPtr), length);
        mPtr += length;
        return true;
    }

    bool readVarint32(uint32_t &value) {
        value = 0;
        int shift = 0;
        while (shift <= 28) {
            uint8_t b = 0;
            if (!readU8(b)) {
                return false;
            }
            value |= static_cast<uint32_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) {
                return true;
            }
            shift += 7;
        }
        return false;
    }

  private:
    const uint8_t *mPtr{nullptr};
    const uint8_t *mEnd{nullptr};
};

// -----------------------------------------------------------------------------
// Decompression: Zlib Raw / Zlib / Snappy
// -----------------------------------------------------------------------------

bool inflateWithWindow(const uint8_t *src, size_t srcLen, int windowBits,
                       std::vector<uint8_t> &dst) {
    if (srcLen > std::numeric_limits<uInt>::max()) {
        return false;
    }

    z_stream strm{};
    strm.next_in = const_cast<Bytef *>(src);
    strm.avail_in = static_cast<uInt>(srcLen);

    if (inflateInit2(&strm, windowBits) != Z_OK) {
        return false;
    }

    const size_t initial = std::min<size_t>(
        kMaxDecompressedBlockSize,
        std::max<size_t>(8192, srcLen > kMaxDecompressedBlockSize / 4
                                   ? kMaxDecompressedBlockSize
                                   : srcLen * 4));
    dst.assign(initial, 0);
    if (dst.empty()) {
        dst.resize(8192);
    }

    while (true) {
        if (strm.total_out >= dst.size()) {
            if (dst.size() >= kMaxDecompressedBlockSize) {
                inflateEnd(&strm);
                return false;
            }
            const size_t nextSize = std::min(
                kMaxDecompressedBlockSize,
                std::max<size_t>(dst.size() * 2, dst.size() + 8192));
            dst.resize(nextSize);
        }

        strm.next_out = dst.data() + strm.total_out;
        strm.avail_out = static_cast<uInt>(dst.size() - strm.total_out);

        const int ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            if (strm.total_out > kMaxDecompressedBlockSize) {
                inflateEnd(&strm);
                return false;
            }
            dst.resize(strm.total_out);
            inflateEnd(&strm);
            return true;
        }
        if (ret != Z_OK) {
            inflateEnd(&strm);
            return false;
        }
    }
}

bool decompressZlib(const uint8_t *src, size_t srcLen, std::vector<uint8_t> &dst) {
    if (srcLen == 0) {
        return false;
    }
    // Bedrock normally uses raw deflate, but accept a zlib wrapper as well.
    if (inflateWithWindow(src, srcLen, -15, dst)) {
        return true;
    }
    return inflateWithWindow(src, srcLen, 15, dst);
}

bool decompressSnappy(const uint8_t *src, size_t srcLen, std::vector<uint8_t> &dst) {
    if (srcLen == 0) {
        return false;
    }
    const uint8_t *p = src;
    const uint8_t *limit = src + srcLen;

    uint32_t expectedLen = 0;
    if (!readVarint32(p, limit, expectedLen)) {
        return false;
    }
    if (expectedLen > kMaxDecompressedBlockSize) {
        return false;
    }

    dst.clear();
    dst.reserve(expectedLen);

    auto append = [&](const uint8_t *data, size_t count) -> bool {
        if (count > kMaxDecompressedBlockSize - dst.size()) {
            return false;
        }
        dst.insert(dst.end(), data, data + count);
        return true;
    };

    auto copyFromOffset = [&](size_t offset, size_t count) -> bool {
        if (offset == 0 || offset > dst.size()) {
            return false;
        }
        if (count > kMaxDecompressedBlockSize - dst.size()) {
            return false;
        }
        const size_t start = dst.size() - offset;
        for (size_t i = 0; i < count; ++i) {
            dst.push_back(dst[start + i]);
        }
        return true;
    };

    while (p < limit) {
        const uint8_t tag = *p++;
        const uint8_t elemType = tag & 0x03;

        if (elemType == 0) {
            size_t len = static_cast<size_t>(tag >> 2) + 1;
            if (len > 60) {
                const size_t extraBytes = len - 60;
                if (extraBytes > 4 ||
                    extraBytes > static_cast<size_t>(limit - p)) {
                    return false;
                }
                len = 0;
                for (size_t i = 0; i < extraBytes; ++i) {
                    len |= static_cast<size_t>(p[i]) << (i * 8);
                }
                len += 1;
                p += extraBytes;
            }
            if (len > static_cast<size_t>(limit - p) || !append(p, len)) {
                return false;
            }
            p += len;
        } else if (elemType == 1) {
            if (p >= limit) {
                return false;
            }
            const size_t len = ((tag >> 2) & 0x07) + 4;
            const size_t offset = (static_cast<size_t>(tag & 0xE0) << 3) | *p++;
            if (!copyFromOffset(offset, len)) {
                return false;
            }
        } else if (elemType == 2) {
            if (static_cast<size_t>(limit - p) < 2) {
                return false;
            }
            const size_t len = static_cast<size_t>(tag >> 2) + 1;
            const size_t offset = static_cast<size_t>(p[0]) |
                                  (static_cast<size_t>(p[1]) << 8);
            p += 2;
            if (!copyFromOffset(offset, len)) {
                return false;
            }
        } else {
            if (static_cast<size_t>(limit - p) < 4) {
                return false;
            }
            const size_t len = static_cast<size_t>(tag >> 2) + 1;
            const size_t offset = static_cast<size_t>(p[0]) |
                                  (static_cast<size_t>(p[1]) << 8) |
                                  (static_cast<size_t>(p[2]) << 16) |
                                  (static_cast<size_t>(p[3]) << 24);
            p += 4;
            if (!copyFromOffset(offset, len)) {
                return false;
            }
        }
    }

    return dst.size() == expectedLen;
}

// -----------------------------------------------------------------------------
// Little-endian NBT parser
// -----------------------------------------------------------------------------

bool skipNbtPayload(ByteReader &reader, uint8_t type, int depth);

bool skipNbtCompoundBody(ByteReader &reader, int depth) {
    if (depth > kMaxNbtDepth) {
        return false;
    }
    while (true) {
        uint8_t type = 0;
        if (!reader.readU8(type)) {
            return false;
        }
        if (type == 0) {
            return true;
        }
        std::string name;
        if (!reader.readString(name)) {
            return false;
        }
        if (!skipNbtPayload(reader, type, depth + 1)) {
            return false;
        }
    }
}

bool skipNbtPayload(ByteReader &reader, uint8_t type, int depth) {
    if (depth > kMaxNbtDepth) {
        return false;
    }

    switch (type) {
        case 0: // End
            return false;
        case 1: // Byte
            return reader.skip(1);
        case 2: // Short
            return reader.skip(2);
        case 3: // Int
            return reader.skip(4);
        case 4: // Long
            return reader.skip(8);
        case 5: // Float
            return reader.skip(4);
        case 6: // Double
            return reader.skip(8);
        case 7: { // Byte array
            int32_t length = 0;
            if (!reader.readI32(length) || length < 0) {
                return false;
            }
            const size_t count = static_cast<size_t>(length);
            if (count > reader.remaining()) {
                return false;
            }
            return reader.skip(count);
        }
        case 8: { // String
            std::string ignored;
            return reader.readString(ignored);
        }
        case 9: { // List
            uint8_t elementType = 0;
            int32_t length = 0;
            if (!reader.readU8(elementType) || !reader.readI32(length) || length < 0) {
                return false;
            }
            if (length > 1024 * 1024) {
                return false;
            }
            if (elementType == 0) {
                return length == 0;
            }
            if (static_cast<size_t>(length) > reader.remaining()) {
                return false;
            }
            for (int32_t i = 0; i < length; ++i) {
                if (!skipNbtPayload(reader, elementType, depth + 1)) {
                    return false;
                }
            }
            return true;
        }
        case 10: // Compound
            return skipNbtCompoundBody(reader, depth + 1);
        case 11: { // Int array
            int32_t length = 0;
            if (!reader.readI32(length) || length < 0) {
                return false;
            }
            const size_t count = static_cast<size_t>(length);
            if (count > reader.remaining() / 4) {
                return false;
            }
            return reader.skip(count * 4);
        }
        case 12: { // Long array
            int32_t length = 0;
            if (!reader.readI32(length) || length < 0) {
                return false;
            }
            const size_t count = static_cast<size_t>(length);
            if (count > reader.remaining() / 8) {
                return false;
            }
            return reader.skip(count * 8);
        }
        default:
            return false;
    }
}

bool parseNbtBlockName(ByteReader &reader, std::string &outName) {
    uint8_t rootType = 0;
    if (!reader.readU8(rootType) || rootType != 0x0A) {
        return false;
    }

    // Root compound name (Bedrock normally writes an empty name).
    std::string rootName;
    if (!reader.readString(rootName)) {
        return false;
    }

    outName.clear();

    while (true) {
        uint8_t type = 0;
        if (!reader.readU8(type)) {
            return false;
        }
        if (type == 0) {
            break;
        }

        std::string name;
        if (!reader.readString(name)) {
            return false;
        }

        if (type == 0x08 && name == "name") {
            if (!reader.readString(outName)) {
                return false;
            }
        } else if (!skipNbtPayload(reader, type, 1)) {
            return false;
        }
    }

    return !outName.empty();
}

// -----------------------------------------------------------------------------
// Block-name interning and top-block classification
// -----------------------------------------------------------------------------

std::vector<std::string> &internedNames() {
    static std::vector<std::string> names{
        "",
        "<parse_error>",
    };
    return names;
}

std::unordered_map<std::string, uint16_t> &internedNameMap() {
    static std::unordered_map<std::string, uint16_t> map;
    return map;
}

uint16_t internBlockName(std::string_view name) {
    if (name.empty()) {
        return kUnknownBlockId;
    }

    auto &names = internedNames();
    auto &map = internedNameMap();

    const std::string owned(name);
    const auto found = map.find(owned);
    if (found != map.end()) {
        return found->second;
    }

    if (names.size() > std::numeric_limits<uint16_t>::max()) {
        return kUnknownBlockId;
    }

    const auto id = static_cast<uint16_t>(names.size());
    names.push_back(owned);
    map.emplace(names.back(), id);
    return id;
}

std::string_view stripMinecraftNamespace(std::string_view name) {
    static constexpr std::string_view kNamespace = "minecraft:";
    if (name.starts_with(kNamespace)) {
        name.remove_prefix(kNamespace.size());
    }
    return name;
}

bool isAirName(std::string_view name) {
    name = stripMinecraftNamespace(name);
    return name == "air" || name == "cave_air" || name == "void_air" ||
           name == "structure_void";
}

bool isSkippableDecoration(std::string_view name) {
    name = stripMinecraftNamespace(name);

    static constexpr std::string_view kExactNames[] = {
        "short_grass",  "tall_grass",   "fern",          "large_fern",
        "dandelion",    "poppy",        "blue_orchid",   "allium",
        "azure_bluet",  "oxeye_daisy",  "cornflower",    "lily_of_the_valley",
        "wither_rose",  "snow_layer",   "vine",          "lily_pad",
        "deadbush",     "sugar_cane",   "bamboo",        "kelp",
        "seagrass",     "wheat",        "carrots",       "potatoes",
        "beetroot",     "torchflower",  "pitcher_plant", "pink_petals",
        "spore_blossom", "hanging_roots", "glow_lichen", "moss_carpet",
        "carpet",       "sculk_vein",   "weeping_vines", "twisting_vines",
        "sweet_berry_bush", "melon_stem", "pumpkin_stem", "bush",
        "sunflower",    "lilac",        "rose_bush",     "peony",
        "big_dripleaf", "small_dripleaf", "dripleaf",    "frogspawn",
    };

    for (const auto exact : kExactNames) {
        if (name == exact) {
            return true;
        }
    }

    return name.ends_with("_sapling") || name.ends_with("_flower") ||
           name.ends_with("_tulip") || name.ends_with("_carpet") ||
           name.ends_with("_coral_fan") || name.ends_with("_coral_wall_fan");
}

struct PaletteEntry {
    uint16_t nameId{0};
    uint8_t flags{0};
};

constexpr uint8_t kPaletteAir = 1u << 0;
constexpr uint8_t kPaletteDecoration = 1u << 1;

std::vector<RGB> &colorCache() {
    static std::vector<RGB> cache;
    return cache;
}

std::vector<uint8_t> &colorCacheReady() {
    static std::vector<uint8_t> ready;
    return ready;
}

RGB getColorForId(uint16_t id) {
    if (id == kNoBlockId) {
        return kUnresolvedColor;
    }
    if (id == kUnknownBlockId) {
        return kParseErrorColor;
    }

    auto &names = internedNames();
    if (id >= names.size()) {
        return kParseErrorColor;
    }

    auto &cache = colorCache();
    auto &ready = colorCacheReady();
    if (cache.size() < names.size()) {
        cache.resize(names.size(), kUnresolvedColor);
        ready.resize(names.size(), 0);
    }
    if (ready[id] == 0) {
        cache[id] = getBlockColor(names[id]);
        ready[id] = 1;
    }
    return cache[id];
}

} // namespace

ChunkColumnData::ChunkColumnData() {
    for (int i = 0; i < 256; ++i) {
        heights[i] = 64;
        colors[i] = kUnresolvedColor;
    }
}

RGB getBlockColor(std::string_view name) {
    name = stripMinecraftNamespace(name);

    if (name == "grass_block" || name == "grass" || name == "moss_block" ||
        name == "moss_carpet") {
        return {95, 159, 53};
    }
    if (name.find("dirt") != std::string_view::npos ||
        name == "podzol" || name == "mycelium" || name == "mud" ||
        name == "farmland" || name == "rooted_dirt" || name == "path") {
        return {134, 96, 67};
    }
    if (name.find("deepslate") != std::string_view::npos ||
        name == "bedrock" || name == "obsidian" || name == "blackstone" ||
        name == "basalt" || name == "coal_ore" || name == "coal_block") {
        return {55, 55, 60};
    }
    if (name.find("redstone") != std::string_view::npos) {
        return {180, 30, 30};
    }
    if (name.find("stone") != std::string_view::npos ||
        name == "andesite" || name == "diorite" || name == "granite" ||
        name == "tuff" || name == "cobbled_deepslate") {
        return {125, 125, 125};
    }
    if (name.find("sand") != std::string_view::npos) {
        return name.find("red") != std::string_view::npos
                   ? RGB{190, 102, 33}
                   : RGB{219, 207, 153};
    }
    if (name.find("water") != std::string_view::npos) {
        return {45, 95, 190};
    }
    if (name == "ice" || name == "packed_ice" || name == "blue_ice") {
        return {170, 210, 245};
    }
    if (name.find("snow") != std::string_view::npos) {
        return {240, 245, 250};
    }
    if (name.find("leaves") != std::string_view::npos ||
        name.find("foliage") != std::string_view::npos) {
        if (name.find("cherry") != std::string_view::npos) {
            return {235, 170, 200};
        }
        return {50, 110, 30};
    }
    if (name.find("log") != std::string_view::npos ||
        name.find("wood") != std::string_view::npos ||
        name.find("planks") != std::string_view::npos) {
        if (name.find("cherry") != std::string_view::npos) {
            return {180, 110, 120};
        }
        return {105, 80, 50};
    }
    if (name.find("terracotta") != std::string_view::npos) {
        return {150, 92, 66};
    }
    if (name.find("clay") != std::string_view::npos) {
        return {160, 166, 179};
    }
    if (name.find("gravel") != std::string_view::npos) {
        return {136, 126, 126};
    }
    if (name.find("lava") != std::string_view::npos ||
        name.find("magma") != std::string_view::npos) {
        return {235, 90, 20};
    }
    if (name.find("copper") != std::string_view::npos) {
        return {200, 120, 70};
    }
    if (name.find("iron") != std::string_view::npos) {
        return {190, 180, 170};
    }
    if (name.find("gold") != std::string_view::npos) {
        return {230, 200, 60};
    }
    if (name.find("diamond") != std::string_view::npos) {
        return {90, 220, 220};
    }
    if (name.find("emerald") != std::string_view::npos) {
        return {60, 200, 120};
    }
    if (name.find("lapis") != std::string_view::npos) {
        return {40, 80, 190};
    }
    if (name.find("amethyst") != std::string_view::npos) {
        return {150, 90, 200};
    }
    if (name.find("netherrack") != std::string_view::npos) {
        return {110, 40, 40};
    }
    if (name.find("quartz") != std::string_view::npos) {
        return {225, 220, 210};
    }

    // Deterministic fallback for custom/unrecognized blocks.
    uint32_t hash = 2166136261u;
    for (const char c : name) {
        hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    const uint8_t r = static_cast<uint8_t>(60 + (hash & 0x7F));
    const uint8_t g = static_cast<uint8_t>(60 + ((hash >> 8) & 0x7F));
    const uint8_t b = static_cast<uint8_t>(60 + ((hash >> 16) & 0x7F));
    return {r, g, b};
}

namespace {

// -----------------------------------------------------------------------------
// SubChunk / Data3D parsing
// -----------------------------------------------------------------------------

enum class SubChunkParseResult {
    Ok,
    UnsupportedVersion,
    Error,
};

SubChunkParseResult parseSubChunk(const uint8_t *data, size_t size,
                                  int8_t keySubY, SubChunkTopData &out,
                                  ScanStats &stats) {
    ByteReader reader(data, size);

    uint8_t version = 0;
    if (!reader.readU8(version)) {
        return SubChunkParseResult::Error;
    }
    if (version != 8 && version != 9) {
        stats.skippedSubChunkVersions[version]++;
        return SubChunkParseResult::UnsupportedVersion;
    }
    stats.subChunkVersions[version]++;

    uint8_t layerCount = 0;
    if (!reader.readU8(layerCount)) {
        return SubChunkParseResult::Error;
    }
    if (layerCount == 0) {
        out = SubChunkTopData{};
        out.subY = keySubY;
        out.version = version;
        return SubChunkParseResult::Ok;
    }
    if (layerCount > 16) {
        return SubChunkParseResult::Error;
    }

    if (version == 9) {
        uint8_t payloadY = 0;
        if (!reader.readU8(payloadY)) {
            return SubChunkParseResult::Error;
        }
        if (static_cast<int8_t>(payloadY) != keySubY) {
            stats.yIndexMismatches++;
        }
    }

    out = SubChunkTopData{};
    out.subY = keySubY;
    out.version = version;

    for (uint8_t layer = 0; layer < layerCount; ++layer) {
        uint8_t header = 0;
        if (!reader.readU8(header)) {
            return SubChunkParseResult::Error;
        }

        const uint8_t paletteType = header & 0x01;
        const uint8_t bitsPerBlock = header >> 1;
        if (!(bitsPerBlock == 0 || bitsPerBlock == 1 || bitsPerBlock == 2 ||
              bitsPerBlock == 3 || bitsPerBlock == 4 || bitsPerBlock == 5 ||
              bitsPerBlock == 6 || bitsPerBlock == 8 || bitsPerBlock == 16)) {
            return SubChunkParseResult::Error;
        }

        const uint32_t blocksPerWord =
            bitsPerBlock > 0 ? (32u / bitsPerBlock) : 0u;
        const uint32_t wordsCount =
            bitsPerBlock > 0 ? (4096u + blocksPerWord - 1u) / blocksPerWord : 0u;

        const uint8_t *words = reader.ptr();
        if (!reader.skip(static_cast<size_t>(wordsCount) * 4u)) {
            return SubChunkParseResult::Error;
        }

        int64_t paletteCount = 0;
        if (paletteType == 0) {
            int32_t count = 0;
            if (!reader.readI32(count)) {
                return SubChunkParseResult::Error;
            }
            paletteCount = count;
        } else {
            uint32_t raw = 0;
            if (!reader.readVarint32(raw)) {
                return SubChunkParseResult::Error;
            }
            paletteCount = static_cast<int32_t>(
                (raw >> 1) ^ (0u - (raw & 1u)));
        }

        if (paletteCount <= 0 ||
            paletteCount > static_cast<int64_t>(kMaxPaletteEntries)) {
            return SubChunkParseResult::Error;
        }

        std::vector<PaletteEntry> palette;
        palette.reserve(static_cast<size_t>(paletteCount));

        for (int64_t i = 0; i < paletteCount; ++i) {
            std::string name;
            if (!parseNbtBlockName(reader, name)) {
                return SubChunkParseResult::Error;
            }

            PaletteEntry entry;
            entry.nameId = internBlockName(name);
            if (isAirName(name)) {
                entry.flags |= kPaletteAir;
            }
            if (isSkippableDecoration(name)) {
                entry.flags |= kPaletteDecoration;
            }
            palette.push_back(entry);
        }

        std::array<uint16_t, 256> layerTopIds{};
        std::array<int8_t, 256> layerTopYs{};

        for (int col = 0; col < 256; ++col) {
            // Column index is (x << 4) | z: X is the slow axis and Z is the
            // fast axis. This matches Bedrock's (x << 8) | (z << 4) | y
            // block index after removing the 4 Y bits.
            const int localX = (col >> 4) & 0x0F;
            const int localZ = col & 0x0F;

            for (int y = 15; y >= 0; --y) {
                uint32_t paletteIndex = 0;

                if (bitsPerBlock > 0) {
                    const uint32_t blockIndex = static_cast<uint32_t>(
                        (localX << 8) | (localZ << 4) | y);
                    const uint32_t wordIndex = blockIndex / blocksPerWord;
                    const uint32_t bitOffset =
                        (blockIndex % blocksPerWord) * bitsPerBlock;
                    if (wordIndex >= wordsCount) {
                        return SubChunkParseResult::Error;
                    }

                    const uint32_t word =
                        readLe32(words + static_cast<size_t>(wordIndex) * 4u);
                    const uint32_t mask = (1u << bitsPerBlock) - 1u;
                    paletteIndex = (word >> bitOffset) & mask;
                }

                if (paletteIndex >= palette.size()) {
                    return SubChunkParseResult::Error;
                }

                const PaletteEntry &entry = palette[paletteIndex];
                if ((entry.flags & kPaletteAir) != 0) {
                    continue;
                }
                if ((entry.flags & kPaletteDecoration) != 0) {
                    continue;
                }

                layerTopIds[col] = entry.nameId;
                layerTopYs[col] = static_cast<int8_t>(y);
                break;
            }
        }

        // Layers are stored bottom-to-top. A later layer wins at the same Y
        // (for example water in layer 1 under/at a plant in layer 0).
        for (int col = 0; col < 256; ++col) {
            if (layerTopIds[col] == kNoBlockId) {
                continue;
            }
            if (out.topNameIds[col] == kNoBlockId ||
                layerTopYs[col] >= out.topLocalYs[col]) {
                out.topNameIds[col] = layerTopIds[col];
                out.topLocalYs[col] = layerTopYs[col];
            }
        }
    }

    return SubChunkParseResult::Ok;
}

bool parseData3D(const uint8_t *data, size_t size, Data3DPayload &out) {
    if (size < 512) {
        return false;
    }
    for (int i = 0; i < 256; ++i) {
        out.heights[i] = static_cast<int16_t>(readLe16(data + i * 2));
    }
    return true;
}

// -----------------------------------------------------------------------------
// LevelDB block iteration
// -----------------------------------------------------------------------------

template <typename Fn>
bool forEachBlockEntry(const std::vector<uint8_t> &block, Fn &&fn) {
    if (block.size() < 4) {
        return false;
    }

    const uint32_t restartCount = readLe32(block.data() + block.size() - 4);
    if (restartCount > (block.size() / 4) - 1) {
        return false;
    }

    const size_t dataLimit =
        block.size() - (static_cast<size_t>(restartCount) + 1u) * 4u;
    const uint8_t *p = block.data();
    const uint8_t *end = block.data() + dataLimit;
    std::string key;

    while (p < end) {
        uint32_t shared = 0;
        uint32_t unshared = 0;
        uint32_t valueLength = 0;
        if (!readVarint32(p, end, shared) ||
            !readVarint32(p, end, unshared) ||
            !readVarint32(p, end, valueLength)) {
            return false;
        }

        if (shared > key.size() ||
            unshared > static_cast<size_t>(end - p)) {
            return false;
        }

        key.resize(shared);
        key.append(reinterpret_cast<const char *>(p), unshared);
        p += unshared;

        if (valueLength > static_cast<size_t>(end - p)) {
            return false;
        }

        if (!fn(key, p, static_cast<size_t>(valueLength))) {
            return false;
        }
        p += valueLength;
    }

    return p == end;
}

bool parseInternalKey(const std::string &internalKey, std::string &userKey,
                      uint64_t &sequence, uint8_t &valueType) {
    if (internalKey.size() < 8) {
        return false;
    }

    const auto *raw = reinterpret_cast<const uint8_t *>(internalKey.data());
    const uint64_t trailer = readLe64(raw + internalKey.size() - 8);
    valueType = static_cast<uint8_t>(trailer & 0xFFu);
    sequence = trailer >> 8;

    if (valueType != 0 && valueType != 1) {
        return false;
    }

    userKey.assign(internalKey.data(), internalKey.size() - 8);
    return true;
}

void applyPendingRecord(ScanState &state, const std::string &userKey,
                        PendingRecord &&record) {
    auto found = state.records.find(userKey);
    if (found == state.records.end()) {
        state.records.emplace(userKey, std::move(record));
        return;
    }

    PendingRecord &existing = found->second;
    if (record.sequence > existing.sequence ||
        (record.sequence == existing.sequence &&
         record.fileNumber >= existing.fileNumber)) {
        existing = std::move(record);
        state.stats.supersededRecords++;
    }
}

int16_t fallbackHeight(const ChunkColumnData &chunk, int column) {
    if (!chunk.hasData3D) {
        return 64;
    }

    // Bedrock's Data3D heightmap stores world-min-relative values in the
    // current format: subtract 64 to obtain the real Y.
    int y = static_cast<int>(chunk.data3dHeights[column]) - 64;
    y = std::clamp(y, -64, 319);
    return static_cast<int16_t>(y);
}

} // namespace

bool LdbReader::readLdbFile(const std::string &filePath, uint64_t fileNumber,
                            ScanState &state) {
    auto fail = [&state]() {
        state.stats.filesFailed++;
        return false;
    };

    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return fail();
    }

    const std::streamoff streamSize = file.tellg();
    if (streamSize < 48) {
        return fail();
    }
    const uint64_t fileSize = static_cast<uint64_t>(streamSize);
    if (fileSize > static_cast<uint64_t>(
                       std::numeric_limits<std::streamoff>::max())) {
        return fail();
    }

    file.seekg(static_cast<std::streamoff>(fileSize - 48));
    uint8_t footer[48]{};
    file.read(reinterpret_cast<char *>(footer), sizeof(footer));
    if (!file) {
        return fail();
    }

    static constexpr uint64_t kLevelDbMagic = 0xdb4775248b80fb57ull;
    if (readLe64(footer + 40) != kLevelDbMagic) {
        return fail();
    }

    const uint8_t *footerPtr = footer;
    const uint8_t *footerLimit = footer + 40;
    BlockHandle metaIndexHandle;
    BlockHandle indexHandle;
    if (!metaIndexHandle.decode(footerPtr, footerLimit) ||
        !indexHandle.decode(footerPtr, footerLimit)) {
        return fail();
    }
    (void)metaIndexHandle;

    auto readBlock = [&](const BlockHandle &handle,
                         std::vector<uint8_t> &out) -> bool {
        uint64_t blockEnd = 0;
        if (handle.size > kMaxDecompressedBlockSize ||
            !safeAdd64(handle.offset, handle.size, blockEnd) ||
            !safeAdd64(blockEnd, 5, blockEnd) ||
            blockEnd > fileSize) {
            return false;
        }

        file.seekg(static_cast<std::streamoff>(handle.offset));
        if (!file) {
            return false;
        }

        const size_t onDiskSize = static_cast<size_t>(handle.size);
        std::vector<uint8_t> disk(onDiskSize + 5);
        file.read(reinterpret_cast<char *>(disk.data()),
                  static_cast<std::streamsize>(disk.size()));
        if (!file) {
            return false;
        }

        const uint8_t compression = disk[onDiskSize];
        if (compression == 0) {
            if (onDiskSize > kMaxDecompressedBlockSize) {
                return false;
            }
            out.assign(disk.begin(), disk.begin() + onDiskSize);
            return true;
        }
        if (compression == 1) {
            return decompressSnappy(disk.data(), onDiskSize, out);
        }
        if (compression == 2 || compression == 4) {
            return decompressZlib(disk.data(), onDiskSize, out);
        }
        return false;
    };

    std::vector<uint8_t> indexBlock;
    if (!readBlock(indexHandle, indexBlock)) {
        return fail();
    }

    std::vector<BlockHandle> dataBlocks;
    if (!forEachBlockEntry(indexBlock,
                           [&dataBlocks](const std::string & /*key*/,
                                         const uint8_t *value,
                                         size_t valueLength) {
                               const uint8_t *p = value;
                               const uint8_t *end = value + valueLength;
                               BlockHandle handle;
                               if (handle.decode(p, end)) {
                                   dataBlocks.push_back(handle);
                               }
                               return true;
                           })) {
        return fail();
    }

    std::vector<uint8_t> dataBlock;
    for (const BlockHandle &handle : dataBlocks) {
        if (!readBlock(handle, dataBlock)) {
            state.stats.malformedDataBlocks++;
            continue;
        }

        if (!forEachBlockEntry(
                dataBlock,
                [&state, fileNumber](const std::string &internalKey,
                                     const uint8_t *value, size_t valueLength) {
                    state.stats.totalRecords++;

                    std::string userKey;
                    uint64_t sequence = 0;
                    uint8_t valueType = 0;
                    if (!parseInternalKey(internalKey, userKey, sequence,
                                          valueType)) {
                        state.stats.ignoredOtherRecords++;
                        return true;
                    }

                    const size_t keySize = userKey.size();
                    if (keySize == 13 || keySize == 14) {
                        state.stats.ignoredDimensionRecords++;
                        return true;
                    }
                    if (keySize != 9 && keySize != 10) {
                        state.stats.ignoredOtherRecords++;
                        return true;
                    }

                    state.stats.overworldRecords++;
                    if (valueType == 1) {
                        state.stats.valueRecords++;
                    } else {
                        state.stats.deletionRecords++;
                    }

                    const auto *keyBytes =
                        reinterpret_cast<const uint8_t *>(userKey.data());
                    const int32_t chunkX = readLeSigned32(keyBytes);
                    const int32_t chunkZ = readLeSigned32(keyBytes + 4);
                    const uint8_t tag = keyBytes[8];
                    const bool hasSubY = keySize == 10;
                    const int8_t subY =
                        hasSubY ? static_cast<int8_t>(keyBytes[9]) : 0;

                    if (tag != 0x2B && tag != 0x2D && tag != 0x2F) {
                        state.stats.ignoredTags[tag]++;
                        return true;
                    }

                    if (tag == 0x2F) {
                        if (!hasSubY) {
                            // A SubChunk key must carry its signed Y byte.
                            state.stats.ignoredOtherRecords++;
                            return true;
                        }
                        if (subY < -4 || subY > 19) {
                            state.stats.outOfRangeSubChunks++;
                            return true;
                        }
                    }

                    if (tag == 0x2D) {
                        // Legacy Data2D terrain. Recorded as unsupported.
                        state.stats.data2dRecords++;
                        return true;
                    }

                    PendingRecord record;
                    record.sequence = sequence;
                    record.fileNumber = fileNumber;
                    record.valueType = valueType;
                    record.tag = tag;
                    record.chunkX = chunkX;
                    record.chunkZ = chunkZ;
                    record.subY = subY;
                    record.hasSubY = hasSubY;

                    if (valueType == 1) {
                        if (tag == 0x2B) {
                            state.stats.data3dRecords++;
                            Data3DPayload payload;
                            if (parseData3D(value, valueLength, payload)) {
                                record.payload = payload;
                            } else {
                                record.parseError = true;
                            }
                        } else if (tag == 0x2F) {
                            state.stats.subChunkRecords++;
                            SubChunkTopData top;
                            top.subY = subY;
                            const auto result = parseSubChunk(
                                value, valueLength, subY, top, state.stats);
                            if (result == SubChunkParseResult::Ok) {
                                SubChunkPayload payload;
                                payload.data = top;
                                record.payload = payload;
                            } else if (result == SubChunkParseResult::Error) {
                                state.stats.subChunkParseErrors++;
                                top.parseError = true;
                                SubChunkPayload payload;
                                payload.data = top;
                                record.payload = payload;
                            }
                        }
                    }

                    applyPendingRecord(state, userKey, std::move(record));
                    return true;
                })) {
            state.stats.malformedDataBlocks++;
        }
    }

    state.stats.filesRead++;
    return true;
}

void LdbReader::finalizeChunks(ScanState &state) {
    state.chunks.clear();
    state.stats.chunksFinalized = 0;
    state.stats.resolvedColumns = 0;
    state.stats.unresolvedColumns = 0;
    state.stats.parseErrorColumns = 0;

    for (auto &[userKey, record] : state.records) {
        (void)userKey;
        if (record.valueType != 1) {
            continue;
        }

        if (record.tag == 0x2B) {
            const auto *payload =
                std::get_if<Data3DPayload>(&record.payload);
            if (payload != nullptr) {
                auto &chunk = state.chunks[makeChunkCoordKey(
                    record.chunkX, record.chunkZ)];
                chunk.chunkX = record.chunkX;
                chunk.chunkZ = record.chunkZ;
                chunk.hasData3D = true;
                chunk.data3dHeights = payload->heights;
                if (record.parseError) {
                    chunk.parseError = true;
                }
            } else if (record.parseError) {
                auto &chunk = state.chunks[makeChunkCoordKey(
                    record.chunkX, record.chunkZ)];
                chunk.chunkX = record.chunkX;
                chunk.chunkZ = record.chunkZ;
                chunk.parseError = true;
            }
        } else if (record.tag == 0x2F) {
            const auto *payload =
                std::get_if<SubChunkPayload>(&record.payload);
            if (payload != nullptr) {
                auto &chunk = state.chunks[makeChunkCoordKey(
                    record.chunkX, record.chunkZ)];
                chunk.chunkX = record.chunkX;
                chunk.chunkZ = record.chunkZ;
                chunk.subChunks.push_back(payload->data);
            }
        }
    }

    // The raw winning records are no longer needed; release the memory now so
    // finalisation and map generation have a smaller peak footprint.
    std::unordered_map<std::string, PendingRecord>().swap(state.records);

    for (auto &[coordKey, chunk] : state.chunks) {
        (void)coordKey;

        std::sort(chunk.subChunks.begin(), chunk.subChunks.end(),
                  [](const SubChunkTopData &a, const SubChunkTopData &b) {
                      if (a.subY != b.subY) {
                          return a.subY > b.subY;
                      }
                      return a.version > b.version;
                  });

        uint64_t chunkResolved = 0;
        uint64_t chunkUnresolved = 0;
        uint64_t chunkParseErrors = 0;

        for (int column = 0; column < 256; ++column) {
            bool found = false;
            bool hitParseError = false;
            uint16_t nameId = kNoBlockId;
            int topY = 0;
            int8_t previousSubY = 0;
            bool havePreviousSubY = false;

            for (const SubChunkTopData &subChunk : chunk.subChunks) {
                if (havePreviousSubY && subChunk.subY == previousSubY) {
                    continue;
                }
                previousSubY = subChunk.subY;
                havePreviousSubY = true;

                if (subChunk.parseError) {
                    hitParseError = true;
                    break;
                }

                if (subChunk.topNameIds[column] == kNoBlockId) {
                    continue;
                }

                nameId = subChunk.topNameIds[column];
                topY = static_cast<int>(subChunk.subY) * 16 +
                       static_cast<int>(subChunk.topLocalYs[column]);
                found = true;
                break;
            }

            if (found) {
                chunk.heights[column] = static_cast<int16_t>(topY);
                chunk.topBlockIds[column] = nameId;

                if (nameId == kUnknownBlockId) {
                    chunk.colors[column] = kParseErrorColor;
                    chunk.resolved[column] = 0;
                    chunkUnresolved++;
                    chunkParseErrors++;
                } else {
                    chunk.colors[column] = getColorForId(nameId);
                    chunk.resolved[column] = 1;
                    chunkResolved++;
                }
            } else {
                chunk.heights[column] = fallbackHeight(chunk, column);
                chunk.topBlockIds[column] = kNoBlockId;
                chunk.resolved[column] = 0;
                chunkUnresolved++;

                if (hitParseError || chunk.parseError) {
                    chunk.colors[column] = kParseErrorColor;
                    chunkParseErrors++;
                } else {
                    chunk.colors[column] = kUnresolvedColor;
                }
            }
        }

        state.stats.resolvedColumns += chunkResolved;
        state.stats.unresolvedColumns += chunkUnresolved;
        state.stats.parseErrorColumns += chunkParseErrors;
    }

    state.stats.chunksFinalized = state.chunks.size();
}

const std::string &LdbReader::blockNameById(uint16_t id) {
    static const std::string kEmpty;
    const auto &names = internedNames();
    if (id >= names.size()) {
        return kEmpty;
    }
    return names[id];
}

} // namespace lod_poc
