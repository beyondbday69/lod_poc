#include "mod/ElfDiag.h"
#include "mod/LodPocMod.h"

#include <elf.h>
#include <link.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace lod_poc {

namespace {

constexpr const char *kKnownReBuildId = "712509dc14ccc233e91f267937dfb46ecdcc4b68";

std::string parseGnuBuildId(uintptr_t base) {
    if (base == 0) return "";
    const auto *ehdr = reinterpret_cast<const Elf64_Ehdr *>(base);
    if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 || ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        return "";
    }

    const auto *phdr = reinterpret_cast<const Elf64_Phdr *>(base + ehdr->e_phoff);
    for (int i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type != PT_NOTE) continue;

        const uint8_t *notePtr = reinterpret_cast<const uint8_t *>(base + phdr[i].p_vaddr);
        const uint8_t *noteEnd = notePtr + phdr[i].p_memsz;

        while (notePtr + 12 <= noteEnd) {
            uint32_t namesz = *reinterpret_cast<const uint32_t *>(notePtr);
            uint32_t descsz = *reinterpret_cast<const uint32_t *>(notePtr + 4);
            uint32_t type = *reinterpret_cast<const uint32_t *>(notePtr + 8);
            notePtr += 12;

            if (notePtr + namesz > noteEnd) break;
            const char *name = reinterpret_cast<const char *>(notePtr);
            notePtr += (namesz + 3) & ~3;

            if (notePtr + descsz > noteEnd) break;
            const uint8_t *desc = notePtr;
            notePtr += (descsz + 3) & ~3;

            // NT_GNU_BUILD_ID == 3, name == "GNU"
            if (type == 3 && namesz == 4 && std::memcmp(name, "GNU", 4) == 0) {
                std::string hex;
                char buf[3];
                for (uint32_t d = 0; d < descsz; ++d) {
                    snprintf(buf, sizeof(buf), "%02x", desc[d]);
                    hex += buf;
                }
                return hex;
            }
        }
    }
    return "";
}

struct ParsedSigByte {
    uint8_t byte{0};
    bool isWildcard{false};
};

std::vector<ParsedSigByte> parseSignature(std::string_view sig) {
    std::vector<ParsedSigByte> result;
    size_t i = 0;
    while (i < sig.size()) {
        while (i < sig.size() && (sig[i] == ' ' || sig[i] == '\t' || sig[i] == '\n' || sig[i] == '\r')) {
            i++;
        }
        if (i >= sig.size()) break;

        if (sig[i] == '?') {
            ParsedSigByte b;
            b.isWildcard = true;
            result.push_back(b);
            i++;
            if (i < sig.size() && sig[i] == '?') i++;
        } else if (i + 1 < sig.size()) {
            char hexStr[3] = { sig[i], sig[i + 1], '\0' };
            char *endPtr = nullptr;
            unsigned long val = strtoul(hexStr, &endPtr, 16);
            if (endPtr != hexStr) {
                ParsedSigByte b;
                b.byte = static_cast<uint8_t>(val);
                b.isWildcard = false;
                result.push_back(b);
            }
            i += 2;
        } else {
            i++;
        }
    }
    return result;
}

} // namespace

MinecraftElfInfo ElfDiag::inspectMinecraftBinary() {
    MinecraftElfInfo info;

    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) {
        return info;
    }

    std::string line;
    while (std::getline(maps, line)) {
        if (line.find("libminecraftpe.so") != std::string::npos) {
            std::istringstream iss(line);
            std::string rangeStr, permsStr, offsetStr, devStr, inodeStr, pathStr;
            iss >> rangeStr >> permsStr >> offsetStr >> devStr >> inodeStr;
            std::getline(iss >> std::ws, pathStr);

            size_t dash = rangeStr.find('-');
            if (dash != std::string::npos) {
                uintptr_t start = std::strtoull(rangeStr.substr(0, dash).c_str(), nullptr, 16);
                uintptr_t end = std::strtoull(rangeStr.substr(dash + 1).c_str(), nullptr, 16);

                if (info.baseAddress == 0) {
                    info.baseAddress = start;
                    info.path = pathStr;
                }
                if (permsStr.find('x') != std::string::npos) {
                    if (info.execStart == 0) info.execStart = start;
                    info.execEnd = end;
                }
            }
        }
    }
    maps.close();

    if (!info.path.empty()) {
        struct stat st{};
        if (stat(info.path.c_str(), &st) == 0) {
            info.fileSize = static_cast<size_t>(st.st_size);
        }
    }

    if (info.baseAddress != 0) {
        info.buildId = parseGnuBuildId(info.baseAddress);
        info.matchesKnownReBuild = (!info.buildId.empty() && info.buildId == kKnownReBuildId);
    }

    return info;
}

void ElfDiag::runSignatureDiagnostics(std::string_view sigPattern,
                                      std::string_view sigName,
                                      const MinecraftElfInfo &elfInfo) {
    LodPocMod::logAll(LogLevel::Warn, "[SigDiag] Signature '{}' failed full resolution. Running prefix scan...", sigName);

    if (elfInfo.execStart == 0 || elfInfo.execEnd <= elfInfo.execStart) {
        LodPocMod::logAll(LogLevel::Error, "[SigDiag] Executable memory range of libminecraftpe.so not available for scanning.");
        return;
    }

    std::vector<ParsedSigByte> fullPattern = parseSignature(sigPattern);
    if (fullPattern.empty()) {
        LodPocMod::logAll(LogLevel::Error, "[SigDiag] Failed to parse signature pattern tokens.");
        return;
    }

    const uint8_t *textStart = reinterpret_cast<const uint8_t *>(elfInfo.execStart);
    const size_t textSize = elfInfo.execEnd - elfInfo.execStart;

    const std::vector<size_t> testPrefixLengths = {16, 24, 32, 40};
    bool anyMatched = false;

    for (size_t prefixLen : testPrefixLengths) {
        if (prefixLen > fullPattern.size()) continue;

        size_t matchCount = 0;
        uintptr_t firstMatchVA = 0;

        if (textSize >= prefixLen) {
            for (size_t off = 0; off <= textSize - prefixLen; ++off) {
                bool match = true;
                for (size_t p = 0; p < prefixLen; ++p) {
                    if (!fullPattern[p].isWildcard && textStart[off + p] != fullPattern[p].byte) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    matchCount++;
                    if (firstMatchVA == 0) {
                        firstMatchVA = elfInfo.execStart + off;
                    }
                }
            }
        }

        if (matchCount > 0) {
            anyMatched = true;
            LodPocMod::logAll(LogLevel::Info,
                              "[SigDiag]   -> Prefix {:2d} bytes: {} match(es) (first VA=0x{:08x}, offset=+0x{:08x})",
                              prefixLen, matchCount, firstMatchVA, firstMatchVA - elfInfo.baseAddress);
        } else {
            LodPocMod::logAll(LogLevel::Info,
                              "[SigDiag]   -> Prefix {:2d} bytes: 0 matches",
                              prefixLen);
        }
    }

    if (!anyMatched) {
        LodPocMod::logAll(LogLevel::Warn, "[SigDiag]   -> no match for any prefix (16, 24, 32, 40 bytes) across .text.");
    }
}

} // namespace lod_poc
