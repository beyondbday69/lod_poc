#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lod_poc {

/**
 * @brief Standalone PNG image encoder using standard zlib.
 *
 * Encodes 24-bit RGB pixel buffers into compliant PNG files without external dependencies.
 */
class PngWriter {
  public:
    static bool writeRgbPng(const std::string &filePath, int width, int height,
                            const std::vector<uint8_t> &rgbPixels);
};

} // namespace lod_poc
