#include "mod/ElfDiag.h"
#include "mod/LodPocMod.h"

#include <elf.h>
#include <link.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <pl/memory/Signature.hpp>

namespace lod_poc {

namespace {

constexpr const char *kMinecraftLibrary = "libminecraftpe.so";
constexpr const char *kKnownReBuildId = "712509dc14ccc233e91f267937dfb46ecdcc4b68";

bool safeReadMem(uintptr_t addr, void *dst, size_t len) {
    if (addr < 0x10000 || dst == nullptr || len == 0) return false;
    struct iovec local{dst, len};
    struct iovec remote{reinterpret_cast<void *>(addr), len};
    ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return n == static_cast<ssize_t>(len);
}

std::string parseGnuBuildId(uintptr_t base) {
    if (base == 0) return "";

    Elf64_Ehdr ehdr{};
    if (!safeReadMem(base, &ehdr, sizeof(ehdr))) {
        return "";
    }

    if (std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 || ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        return "";
    }

    for (int i = 0; i < ehdr.e_phnum; ++i) {
        Elf64_Phdr phdr{};
        uintptr_t phdrAddr = base + ehdr.e_phoff + (i * sizeof(Elf64_Phdr));
        if (!safeReadMem(phdrAddr, &phdr, sizeof(phdr))) {
            continue;
        }

        if (phdr.p_type != PT_NOTE || phdr.p_memsz == 0 || phdr.p_memsz > 65536) {
            continue;
        }

        std::vector<uint8_t> noteBuf(phdr.p_memsz);
        if (!safeReadMem(base + phdr.p_vaddr, noteBuf.data(), noteBuf.size())) {
            continue;
        }

        const uint8_t *notePtr = noteBuf.data();
        const uint8_t *noteEnd = notePtr + noteBuf.size();

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

std::string truncatePatternToBytes(std::string_view sig, size_t targetBytes) {
    std::string result;
    size_t byteCount = 0;
    size_t i = 0;

    while (i < sig.size() && byteCount < targetBytes) {
        while (i < sig.size() && (sig[i] == ' ' || sig[i] == '\t' || sig[i] == '\n' || sig[i] == '\r')) {
            i++;
        }
        if (i >= sig.size()) break;

        size_t tokenStart = i;
        while (i < sig.size() && sig[i] != ' ' && sig[i] != '\t' && sig[i] != '\n' && sig[i] != '\r') {
            i++;
        }

        if (!result.empty()) {
            result += ' ';
        }
        result.append(sig.substr(tokenStart, i - tokenStart));
        byteCount++;
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

                if (info.baseAddress == 0) {
                    info.baseAddress = start;
                    info.path = pathStr;
                    break;
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
                                      std::string_view sigName) {
    LodPocMod::logAll(LogLevel::Warn, "[SigDiag] Signature '{}' failed full resolution. Checking prefix lengths...", sigName);

    const std::vector<size_t> testLengths = {16, 24, 32, 40};
    bool anyFound = false;

    for (size_t numBytes : testLengths) {
        std::string prefixPattern = truncatePatternToBytes(sigPattern, numBytes);
        if (prefixPattern.empty()) continue;

        uintptr_t resolved = 0;
        try {
            resolved = pl::memory::resolveSignature(prefixPattern, kMinecraftLibrary);
        } catch (...) {
            resolved = 0;
        }

        if (resolved != 0) {
            anyFound = true;
            LodPocMod::logAll(LogLevel::Info,
                              "[SigDiag]   -> Prefix {:2d} bytes: RESOLVED at 0x{:x}",
                              numBytes, resolved);
        } else {
            LodPocMod::logAll(LogLevel::Info,
                              "[SigDiag]   -> Prefix {:2d} bytes: NOT FOUND",
                              numBytes);
        }
    }

    if (!anyFound) {
        LodPocMod::logAll(LogLevel::Warn,
                          "[SigDiag]   -> no match for any prefix (16, 24, 32, 40 bytes).");
    }
}

} // namespace lod_poc
