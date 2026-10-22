#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lod_poc {

struct MinecraftElfInfo {
    uintptr_t baseAddress{0};
    uintptr_t execStart{0};
    uintptr_t execEnd{0};
    std::string path;
    size_t fileSize{0};
    std::string buildId;
    bool matchesKnownReBuild{false};
};

class ElfDiag {
public:
    static MinecraftElfInfo inspectMinecraftBinary();

    static void runSignatureDiagnostics(std::string_view sigPattern,
                                        std::string_view sigName,
                                        const MinecraftElfInfo &elfInfo);
};

} // namespace lod_poc
