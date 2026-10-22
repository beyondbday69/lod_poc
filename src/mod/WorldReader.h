#pragma once

#include <filesystem>
#include <string>

namespace lod_poc {

/**
 * @brief Background World Data Reader and LOD Map Generator.
 *
 * Discovers the active Bedrock LevelDB save via /proc/self/fd, parses terrain
 * data directly from immutable .ldb files without taking the DB lock, computes
 * LOD tiers, and writes a top-down diagnostic PNG map.
 */
class WorldReader {
  public:
    static void startBackgroundScan(const std::filesystem::path &primaryDir);

  private:
    static void runScan(const std::filesystem::path &primaryDir);
};

} // namespace lod_poc
