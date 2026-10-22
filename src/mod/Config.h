#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <pl/Config.hpp>

namespace lod_poc {

/**
 * @brief Typed configuration for the LOD PoC mod.
 */
struct ModConfig {
    int version = 1;
    bool enabled = true;

    /** Enable probe 1: ChunkSource::_lightingTask */
    bool probe_chunk_lighting = true;

    /** Enable probe 2: glBindFramebuffer sequence tracking */
    bool probe_bind_framebuffer = true;

    /** Enable probe 3: eglSwapBuffers fps tracker */
    bool probe_egl_swap_buffers = true;

    /** Enable probe 5: LevelRendererCamera::cullPointOrChunk */
    bool probe_level_renderer_cull = true;

    /** Enable background LevelDB world data reader and top-down LOD map generation */
    bool buildLodMap = true;

    /** Optional periodic world rescan interval in seconds (0 = scan once). */
    int rescanIntervalSec = 10;

    /** Probe A: GL Frame Trace */
    bool traceFrame = true;
    bool traceRaw = false;
    std::string traceLabel = "";
    int traceDelayAfterWorldSec = 10;

    /** Camera origin supplied from F3 for absolute chunk-translation tracking. */
    float originX = 0.0f;
    float originY = 0.0f;
    float originZ = 0.0f;
    bool originValid = false;

    /** Probe B: Camera Position Discovery */
    bool probeCameraDiff = true;
    std::string userNote = "";

    /** Build-ID safety gate: require known RE binary for signature-based hooks */
    bool requireKnownBuild = true;

    /** Stage 2: Depth-tested test quad */
    bool pocQuad = true;
    float pocDepth = 0.5f;
    std::string pocColor = "ff00ff";
    std::string pocTrigger = "before_fbo1";

    /** Stage 3: 3D World Cube Test */
    bool poc3d = true;
    float poc3dDist = 8.0f;

    /** Stage 4: LOD Terrain Distance Cutoffs (blocks) */
    float minLodDist = 64.0f;
    float maxLodDist = 512.0f;
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace lod_poc
