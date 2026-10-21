#pragma once

#include <string_view>

/**
 * @file signatures.h
 * @brief Verified single-match byte signatures for Minecraft Bedrock 1.26.51.1 (arm64-v8a).
 *
 * Target: libminecraftpe.so (BuildID: 712509dc14ccc233e91f267937dfb46ecdcc4b68)
 * Only verified single-match signatures from reverse engineering are included.
 */

namespace mc_signatures {

/**
 * @brief Target 1 [MEDIUM]: RequestChunkRadiusPacket::getId_and_counts (VA 0xd22be4c)
 * Matches: exactly 1 in .text.
 */
inline constexpr std::string_view kSigRequestChunkRadiusPacket =
    "a0 08 80 52 c0 03 5f d6 ?? ?? ?? ?? ?? ?? ?? ?? "
    "01 03 80 52 c0 03 5f d6 00 34 40 b9 c0 03 5f d6 "
    "01 34 00 b9 c0 03 5f d6";

/**
 * @brief Target B [HIGH]: ChunkSource::_lightingTask (VA 0x103dce4c)
 *
 * Executed once per chunk load/generation when a LevelChunk becomes ready for lighting.
 * Arguments:
 *   - x0: Task context pointer.
 * Matches: exactly 1 in .text (64 bytes).
 */
inline constexpr std::string_view kSigChunkLightingTask =
    "ff 83 04 d1 fd 7b 0d a9 fc 67 0e a9 f8 5f 0f a9 "
    "f6 57 10 a9 f4 4f 11 a9 fd 43 03 91 58 d0 3b d5 "
    "f3 03 00 aa 08 17 40 f9 a8 83 1f f8 14 d8 40 a9 "
    "15 10 40 f9 e0 03 16 aa ?? ?? ?? ?? 60 18 00 37";

/**
 * @brief Target 3 [HIGH]: SubChunk::getBlock storage lookup (VA 0x1032a354)
 *
 * SubChunk block reader in SubChunk.cpp (line 654).
 * Loads mBlockStorage from SubChunk offset +0x38 and calls virtual getBlock(index) at vtable + 0x20.
 * Index formula: (x << 8) | (z << 4) | y (y fastest stride 1, z stride 16, x stride 256).
 * Matches: exactly 1 in .text.
 */
inline constexpr std::string_view kSigSubChunkGetBlockStorage =
    "00 1c 40 f9 80 00 00 b4 08 00 40 f9 02 11 40 f9 "
    "40 00 1f d6 fd 7b bf a9 fd 03 00 91 ?? ?? ?? ?? "
    "?? ?? ?? ?? 00 00 40 f9";

/**
 * @brief Target D [HIGH]: LevelRendererCamera::cullPointOrChunk (VA 0xb27f49c)
 *
 * Camera culling and frustum orientation function in LevelRendererCamera.
 * Offsets:
 *   - [LevelRendererCamera + 0x5c8]: Camera World Position (Vec3: float x, y, z)
 *   - [LevelRendererCamera + 0x5d4]: Camera Forward View Direction (Vec3: float dirX, dirY, dirZ)
 * Matches: exactly 1 in .text (48 bytes).
 */
inline constexpr std::string_view kSigLevelRendererCameraCull =
    "ff 03 07 d1 e8 ab 00 fd fd 7b 16 a9 fc 6f 17 a9 "
    "fa 67 18 a9 f8 5f 19 a9 f6 57 1a a9 f4 4f 1b a9 "
    "fd 83 05 91 59 d0 3b d5 f3 03 00 aa e0 a3 01 91";

} // namespace mc_signatures
