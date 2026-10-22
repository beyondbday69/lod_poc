#pragma once

#include <filesystem>
#include <string>

namespace lod_poc {

/**
 * @brief Background World Data Reader and LOD Map Generator.
 *
 * Discovers the active Bedrock LevelDB save via /proc/self/fd, waits for the
 * world database to be opened by the game, parses immutable .ldb files without
 * taking the DB lock, computes LOD tiers, and writes a top-down diagnostic PNG.
 *
 * The scan always runs on a detached worker thread. The render thread only
 * starts the worker or updates the rescan interval.
 */
class WorldReader {
  public:
    static void startBackgroundScan(const std::filesystem::path &primaryDir,
                                    int rescanIntervalSec);

    /** Updates the optional periodic rescan interval (0 = disabled). */
    static void setRescanIntervalSec(int seconds);

  private:
    static void runScan(const std::filesystem::path &primaryDir,
                        const std::string &dbDir);
};

} // namespace lod_poc
