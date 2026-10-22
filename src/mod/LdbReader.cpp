#include "mod/LdbReader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
#include <zlib.h>

namespace lod_poc {
namespace {

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

struct BlockHandle {
    uint64_t offset{0};
    uint64_t size{0};

    bool decode(const uint8_t *&ptr, const uint8_t *limit) {
        return readVarint64(ptr, limit, offset) && readVarint64(ptr, limit, size);
    }
};

// -----------------------------------------------------------------------------
// Decompression: Zlib Raw and Snappy
// -----------------------------------------------------------------------------

bool decompressZlib(const uint8_t *src, size_t srcLen, std::vector<uint8_t> &dst) {
    z_stream strm{};
    strm.next_in = const_cast<Bytef *>(src);
    strm.avail_in = static_cast<uInt>(srcLen);

    // Bedrock LevelDB uses raw deflate (-15)
    if (inflateInit2(&strm, -15) != Z_OK) {
        if (inflateInit(&strm) != Z_OK) {
            return false;
        }
    }

    dst.resize(std::max<size_t>(srcLen * 4, 8192));
    while (true) {
        strm.next_out = dst.data() + strm.total_out;
        strm.avail_out = static_cast<uInt>(dst.size() - strm.total_out);

        const int ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            dst.resize(strm.total_out);
            inflateEnd(&strm);
            return true;
        }
        if (ret != Z_OK) {
            inflateEnd(&strm);
            return false;
        }
        dst.resize(dst.size() * 2);
    }
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

    dst.clear();
    dst.reserve(expectedLen);

    while (p < limit) {
        const uint8_t tag = *p++;
        const uint8_t elemType = tag & 0x03;
        if (elemType == 0) {
            // Literal
            size_t len = (tag >> 2) + 1;
            if (len > 60) {
                const size_t extraBytes = len - 60;
                if (p + extraBytes > limit) return false;
                len = 0;
                for (size_t i = 0; i < extraBytes; ++i) {
                    len |= static_cast<size_t>(p[i]) << (i * 8);
                }
                len += 1;
                p += extraBytes;
            }
            if (p + len > limit) return false;
            dst.insert(dst.end(), p, p + len);
            p += len;
        } else if (elemType == 1) {
            // Copy 1-byte offset
            if (p >= limit) return false;
            const size_t len = ((tag >> 2) & 0x07) + 4;
            const size_t offset = ((tag & 0xE0) << 3) | *p++;
            if (offset == 0 || offset > dst.size()) return false;
            const size_t start = dst.size() - offset;
            for (size_t i = 0; i < len; ++i) {
                dst.push_back(dst[start + i]);
            }
        } else if (elemType == 2) {
            // Copy 2-byte offset
            if (p + 2 > limit) return false;
            const size_t len = (tag >> 2) + 1;
            const size_t offset = p[0] | (static_cast<size_t>(p[1]) << 8);
            p += 2;
            if (offset == 0 || offset > dst.size()) return false;
            const size_t start = dst.size() - offset;
            for (size_t i = 0; i < len; ++i) {
                dst.push_back(dst[start + i]);
            }
        } else {
            // Copy 4-byte offset
            if (p + 4 > limit) return false;
            const size_t len = (tag >> 2) + 1;
            const size_t offset = p[0] | (static_cast<size_t>(p[1]) << 8) |
                                  (static_cast<size_t>(p[2]) << 16) |
                                  (static_cast<size_t>(p[3]) << 24);
            p += 4;
            if (offset == 0 || offset > dst.size()) return false;
            const size_t start = dst.size() - offset;
            for (size_t i = 0; i < len; ++i) {
                dst.push_back(dst[start + i]);
            }
        }
    }
    return dst.size() == expectedLen;
}

// -----------------------------------------------------------------------------
// Little-Endian NBT Block Name Extractor
// -----------------------------------------------------------------------------

const uint8_t *extractBlockName(const uint8_t *p, const uint8_t *limit,
                                std::string &outName) {
    outName.clear();
    // Search for string tag with name "name"
    // In block compound: tag 0x08, length 4, "name", length N, string
    static constexpr uint8_t kNamePattern[] = {0x08, 0x04, 0x00, 'n', 'a', 'm', 'e'};
    const auto searchLimit = std::min(limit, p + 128);
    for (const uint8_t *cur = p; cur + 9 <= searchLimit; ++cur) {
        if (std::memcmp(cur, kNamePattern, sizeof(kNamePattern)) == 0) {
            const uint16_t valLen = readLe16(cur + sizeof(kNamePattern));
            const uint8_t *valStr = cur + sizeof(kNamePattern) + 2;
            if (valStr + valLen <= limit) {
                outName.assign(reinterpret_cast<const char *>(valStr), valLen);
                break;
            }
        }
    }
    // Advance past compound: scan for end tag 0x00 or bounded skip
    while (p < limit && *p != 0x00) {
        ++p;
    }
    if (p < limit && *p == 0x00) {
        ++p;
    }
    return p;
}

} // namespace

ChunkColumnData::ChunkColumnData() {
    for (int i = 0; i < 256; ++i) {
        heights[i] = 64;
        colors[i] = {95, 159, 53}; // Default grass
    }
}

RGB getBlockColor(std::string_view name) {
    if (name.starts_with("minecraft:")) {
        name.remove_prefix(10);
    }
    if (name.find("grass") != std::string_view::npos) return {95, 159, 53};
    if (name.find("dirt") != std::string_view::npos ||
        name.find("podzol") != std::string_view::npos ||
        name.find("path") != std::string_view::npos) return {134, 96, 67};
    if (name.find("stone") != std::string_view::npos ||
        name.find("andesite") != std::string_view::npos ||
        name.find("diorite") != std::string_view::npos ||
        name.find("granite") != std::string_view::npos) return {125, 125, 125};
    if (name.find("deepslate") != std::string_view::npos ||
        name.find("bedrock") != std::string_view::npos) return {55, 55, 60};
    if (name.find("sand") != std::string_view::npos) return {219, 207, 153};
    if (name.find("water") != std::string_view::npos) return {45, 95, 190};
    if (name.find("snow") != std::string_view::npos ||
        name.find("ice") != std::string_view::npos) return {240, 245, 250};
    if (name.find("leaves") != std::string_view::npos ||
        name.find("foliage") != std::string_view::npos) return {50, 110, 30};
    if (name.find("log") != std::string_view::npos ||
        name.find("wood") != std::string_view::npos) return {105, 80, 50};
    if (name.find("terracotta") != std::string_view::npos) return {150, 92, 66};
    if (name.find("clay") != std::string_view::npos) return {160, 166, 179};
    if (name.find("gravel") != std::string_view::npos) return {136, 126, 126};
    if (name.find("lava") != std::string_view::npos) return {235, 90, 20};
    if (name.find("flower") != std::string_view::npos) return {220, 60, 50};

    // Deterministic hash fallback for custom/unrecognized blocks
    uint32_t hash = 2166136261u;
    for (const char c : name) {
        hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
    }
    const uint8_t r = static_cast<uint8_t>(60 + (hash & 0x7F));
    const uint8_t g = static_cast<uint8_t>(60 + ((hash >> 8) & 0x7F));
    const uint8_t b = static_cast<uint8_t>(60 + ((hash >> 16) & 0x7F));
    return {r, g, b};
}

bool LdbReader::readLdbFile(const std::string &filePath,
                            std::unordered_map<uint64_t, ChunkColumnData> &outChunks) {
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }

    const auto fileSize = static_cast<uint64_t>(file.tellg());
    if (fileSize < 48) {
        return false;
    }

    // Read footer (last 48 bytes)
    file.seekg(fileSize - 48);
    uint8_t footer[48];
    file.read(reinterpret_cast<char *>(footer), 48);
    if (!file) {
        return false;
    }

    // Verify LevelDB magic
    static constexpr uint64_t kLevelDbMagic = 0xdb4775248b80fb57ull;
    const uint64_t magic = readLe64(footer + 40);
    if (magic != kLevelDbMagic) {
        return false;
    }

    // Decode metaindex and index handles
    const uint8_t *footerPtr = footer;
    const uint8_t *footerLimit = footer + 40;
    BlockHandle metaIndexHandle;
    BlockHandle indexHandle;
    if (!metaIndexHandle.decode(footerPtr, footerLimit) ||
        !indexHandle.decode(footerPtr, footerLimit)) {
        return false;
    }

    // Helper lambda to read and decompress a block from disk
    auto readBlock = [&](const BlockHandle &handle, std::vector<uint8_t> &outDecompressed) -> bool {
        if (handle.offset + handle.size + 5 > fileSize) {
            return false;
        }
        file.seekg(handle.offset);
        std::vector<uint8_t> diskBuf(handle.size + 5);
        file.read(reinterpret_cast<char *>(diskBuf.data()), diskBuf.size());
        if (!file) {
            return false;
        }

        const uint8_t compType = diskBuf[handle.size];
        if (compType == 0) {
            // Uncompressed
            outDecompressed.assign(diskBuf.begin(), diskBuf.begin() + handle.size);
            return true;
        }
        if (compType == 1) {
            // Snappy
            return decompressSnappy(diskBuf.data(), handle.size, outDecompressed);
        }
        if (compType == 2 || compType == 4) {
            // Zlib / Zlib raw
            return decompressZlib(diskBuf.data(), handle.size, outDecompressed);
        }
        return false;
    };

    // Read and parse Index Block to get data block handles
    std::vector<uint8_t> indexBlock;
    if (!readBlock(indexHandle, indexBlock) || indexBlock.size() < 4) {
        return false;
    }

    const uint32_t indexRestarts = readLe32(indexBlock.data() + indexBlock.size() - 4);
    const size_t indexDataLimit = indexBlock.size() - (indexRestarts + 1) * 4;

    std::vector<BlockHandle> dataBlocks;
    const uint8_t *p = indexBlock.data();
    const uint8_t *pEnd = indexBlock.data() + indexDataLimit;
    std::string currentKey;

    while (p < pEnd) {
        uint32_t shared = 0, unshared = 0, valueLen = 0;
        if (!readVarint32(p, pEnd, shared) ||
            !readVarint32(p, pEnd, unshared) ||
            !readVarint32(p, pEnd, valueLen)) {
            break;
        }
        if (p + unshared + valueLen > pEnd) {
            break;
        }
        currentKey.resize(shared);
        currentKey.append(reinterpret_cast<const char *>(p), unshared);
        p += unshared;

        BlockHandle dataHandle;
        const uint8_t *valPtr = p;
        if (dataHandle.decode(valPtr, p + valueLen)) {
            dataBlocks.push_back(dataHandle);
        }
        p += valueLen;
    }

    // Now iterate through every data block and extract chunk records
    std::vector<uint8_t> dataBlock;
    for (const auto &bHandle : dataBlocks) {
        if (!readBlock(bHandle, dataBlock) || dataBlock.size() < 4) {
            continue;
        }

        const uint32_t restarts = readLe32(dataBlock.data() + dataBlock.size() - 4);
        if (dataBlock.size() < (restarts + 1) * 4) {
            continue;
        }
        const size_t dataLimit = dataBlock.size() - (restarts + 1) * 4;

        const uint8_t *cur = dataBlock.data();
        const uint8_t *curEnd = dataBlock.data() + dataLimit;
        std::string recordKey;

        while (cur < curEnd) {
            uint32_t shared = 0, unshared = 0, valueLen = 0;
            if (!readVarint32(cur, curEnd, shared) ||
                !readVarint32(cur, curEnd, unshared) ||
                !readVarint32(cur, curEnd, valueLen)) {
                break;
            }
            if (cur + unshared + valueLen > curEnd) {
                break;
            }
            recordKey.resize(shared);
            recordKey.append(reinterpret_cast<const char *>(cur), unshared);
            cur += unshared;

            const uint8_t *valBytes = cur;
            cur += valueLen;

            // Only parse Overworld chunks (length 9 or 10)
            if (recordKey.size() == 9) {
                const auto *kBytes = reinterpret_cast<const uint8_t *>(recordKey.data());
                const int32_t cx = readLeSigned32(kBytes);
                const int32_t cz = readLeSigned32(kBytes + 4);
                const uint8_t tag = kBytes[8];

                // Tag 0x2B: Data3D (Heightmap + Biomes)
                if (tag == 0x2B && valueLen >= 512) {
                    const uint64_t coordKey = makeChunkCoordKey(cx, cz);
                    auto &chunk = outChunks[coordKey];
                    chunk.chunkX = cx;
                    chunk.chunkZ = cz;
                    chunk.hasData3D = true;

                    for (int i = 0; i < 256; ++i) {
                        chunk.heights[i] = static_cast<int16_t>(readLe16(valBytes + i * 2));
                    }
                }
            } else if (recordKey.size() == 10) {
                const auto *kBytes = reinterpret_cast<const uint8_t *>(recordKey.data());
                const int32_t cx = readLeSigned32(kBytes);
                const int32_t cz = readLeSigned32(kBytes + 4);
                const uint8_t tag = kBytes[8];
                const int8_t subY = static_cast<int8_t>(kBytes[9]);

                // Tag 0x2F: SubChunk
                if (tag == 0x2F && valueLen > 2) {
                    const uint64_t coordKey = makeChunkCoordKey(cx, cz);
                    auto &chunk = outChunks[coordKey];
                    chunk.chunkX = cx;
                    chunk.chunkZ = cz;

                    // Parse block palette from storage 0
                    const uint8_t *sp = valBytes + 2; // skip version and numStorages
                    const uint8_t *sEnd = valBytes + valueLen;
                    if (sp < sEnd) {
                        const uint8_t header = *sp++;
                        const uint8_t bitsPerBlock = header >> 1;
                        std::vector<RGB> paletteColors;

                        if (bitsPerBlock == 0 && sp + 4 <= sEnd) {
                            const int32_t palSize = readLeSigned32(sp);
                            sp += 4;
                            for (int pIdx = 0; pIdx < palSize && sp < sEnd; ++pIdx) {
                                std::string blockName;
                                sp = extractBlockName(sp, sEnd, blockName);
                                paletteColors.push_back(getBlockColor(blockName));
                            }
                            if (!paletteColors.empty()) {
                                for (int col = 0; col < 256; ++col) {
                                    const int y = chunk.heights[col];
                                    if (std::floor(static_cast<float>(y) / 16.0f) == subY) {
                                        chunk.colors[col] = paletteColors[0];
                                    }
                                }
                            }
                        } else if (bitsPerBlock > 0) {
                            const uint32_t blocksPerWord = 32 / bitsPerBlock;
                            const uint32_t wordsCount = (4096 + blocksPerWord - 1) / blocksPerWord;
                            const auto *words = reinterpret_cast<const uint32_t *>(sp);
                            sp += wordsCount * 4;

                            if (sp + 4 <= sEnd) {
                                const int32_t palSize = readLeSigned32(sp);
                                sp += 4;
                                for (int pIdx = 0; pIdx < palSize && sp < sEnd; ++pIdx) {
                                    std::string blockName;
                                    sp = extractBlockName(sp, sEnd, blockName);
                                    paletteColors.push_back(getBlockColor(blockName));
                                }

                                if (!paletteColors.empty()) {
                                    const uint32_t mask = (1u << bitsPerBlock) - 1u;
                                    for (int col = 0; col < 256; ++col) {
                                        const int y = chunk.heights[col];
                                        if (static_cast<int8_t>(std::floor(static_cast<float>(y) / 16.0f)) == subY) {
                                            const int localX = col & 0xF;
                                            const int localZ = (col >> 4) & 0xF;
                                            const int localY = (y % 16 + 16) % 16;
                                            const int bIdx = (localX << 8) | (localZ << 4) | localY;
                                            const size_t wIdx = bIdx / blocksPerWord;
                                            const size_t bOffset = (bIdx % blocksPerWord) * bitsPerBlock;
                                            if (wIdx < wordsCount) {
                                                const uint32_t palIdx = (words[wIdx] >> bOffset) & mask;
                                                if (palIdx < paletteColors.size()) {
                                                    chunk.colors[col] = paletteColors[palIdx];
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return true;
}

} // namespace lod_poc
