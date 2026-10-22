#pragma once

#include <optional>
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
};

nlohmann::json makeDefaultConfigJson();
nlohmann::json makeConfigSchemaJson();

} // namespace lod_poc
