#include "mod/PngWriter.h"

#include <cstdio>
#include <cstring>
#include <zlib.h>

namespace lod_poc {
namespace {

inline void writeBe32(uint8_t *buf, uint32_t val) {
    buf[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    buf[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    buf[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    buf[3] = static_cast<uint8_t>(val & 0xFF);
}

void writeChunk(FILE *fp, const char type[4], const uint8_t *data, size_t length) {
    uint8_t lenBuf[4];
    writeBe32(lenBuf, static_cast<uint32_t>(length));
    std::fwrite(lenBuf, 1, 4, fp);
    std::fwrite(type, 1, 4, fp);

    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, reinterpret_cast<const Bytef *>(type), 4);

    if (length > 0 && data != nullptr) {
        std::fwrite(data, 1, length, fp);
        crc = crc32(crc, reinterpret_cast<const Bytef *>(data), static_cast<uInt>(length));
    }

    uint8_t crcBuf[4];
    writeBe32(crcBuf, static_cast<uint32_t>(crc));
    std::fwrite(crcBuf, 1, 4, fp);
}

} // namespace

bool PngWriter::writeRgbPng(const std::string &filePath, int width, int height,
                            const std::vector<uint8_t> &rgbPixels) {
    if (width <= 0 || height <= 0 ||
        rgbPixels.size() < static_cast<size_t>(width * height * 3)) {
        return false;
    }

    FILE *fp = std::fopen(filePath.c_str(), "wb");
    if (fp == nullptr) {
        return false;
    }

    // 1. PNG Signature (8 bytes)
    static constexpr uint8_t kPngSignature[8] = {
        0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
    };
    std::fwrite(kPngSignature, 1, 8, fp);

    // 2. IHDR Chunk (13 bytes)
    uint8_t ihdr[13];
    writeBe32(ihdr + 0, static_cast<uint32_t>(width));
    writeBe32(ihdr + 4, static_cast<uint32_t>(height));
    ihdr[8] = 8;  // bit depth: 8
    ihdr[9] = 2;  // color type: RGB
    ihdr[10] = 0; // compression: deflate
    ihdr[11] = 0; // filter: standard
    ihdr[12] = 0; // interlace: none
    writeChunk(fp, "IHDR", ihdr, sizeof(ihdr));

    // 3. Raw filtered scanlines: 1 filter byte (0 = None) + row bytes
    const size_t rowBytes = static_cast<size_t>(width * 3);
    const size_t rawSize = static_cast<size_t>(height) * (1 + rowBytes);
    std::vector<uint8_t> rawScanlines(rawSize);

    for (int y = 0; y < height; ++y) {
        const size_t rawOffset = static_cast<size_t>(y) * (1 + rowBytes);
        rawScanlines[rawOffset] = 0; // Filter byte: None
        std::memcpy(rawScanlines.data() + rawOffset + 1,
                    rgbPixels.data() + static_cast<size_t>(y) * rowBytes,
                    rowBytes);
    }

    // 4. Deflate raw scanlines using zlib
    uLongf compressedBound = compressBound(static_cast<uLong>(rawSize));
    std::vector<uint8_t> compressed(compressedBound);
    if (compress(compressed.data(), &compressedBound, rawScanlines.data(),
                 static_cast<uLong>(rawSize)) != Z_OK) {
        std::fclose(fp);
        return false;
    }

    // 5. IDAT Chunk
    writeChunk(fp, "IDAT", compressed.data(), compressedBound);

    // 6. IEND Chunk
    writeChunk(fp, "IEND", nullptr, 0);

    std::fflush(fp);
    std::fclose(fp);
    return true;
}

} // namespace lod_poc
