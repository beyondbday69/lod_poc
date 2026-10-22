# LOD PoC (Proof-of-Concept) Native Mod

A LeviLaunchroid native mod (`arm64-v8a`) for Minecraft Bedrock 1.26.51.1 built on `preloader-android` (SDK commit `5d108da350ce3e7f13f51415f81e65344ecd5d14`).

This repository implements **log-only, non-intrusive diagnostic probes** designed to verify hook points, graphics runtime context, chunk delivery pathways, and camera matrices for a future "Distant Horizons"-style LOD engine.

---

## Probes Implemented

| Probe | Target | Mechanism | Goal & Telemetry |
|---|---|---|---|
| **1. Chunk Lighting** | `ChunkSource::_lightingTask` (VA `0x103dce4c`) | Inline hook (`pl::memory::HookHandle`) via single-match signature | Observes chunk preparation frequency and thread context. Reads first 4 quadwords of context `x0` (`[q0, q1, q2, q3]`) before invoking the original, forwards 8 pointer arguments, logs first 20 calls followed by 5s call rate summaries. |
| **2. FBO Binding** | `glBindFramebuffer` | Inline detour via `dlsym(RTLD_DEFAULT, "glBindFramebuffer")` | Intercepts OpenGL ES framebuffer bindings to locate scene-to-post processing passes. Records per-frame sequence of FBO IDs (logged every 120 frames) + thread ID. |
| **3. Frame Swap** | `eglSwapBuffers` | Inline detour via `dlsym(RTLD_DEFAULT, "eglSwapBuffers")` | Serves as the frame boundary delimiter. Measures and reports true rendering FPS every 5 seconds along with thread ID. |
| **4. SubChunk Lookup** | `SubChunk::getBlock` (VA `0x1032a354`) | **Signature resolution only** (no detour) | Verifies runtime resolution of block storage access (`kSigSubChunkGetBlockStorage`) and validates Bedrock index formula `(x << 8) \| (z << 4) \| y`. |
| **5. Camera Culling** | `LevelRendererCamera::cullPointOrChunk` (VA `0xb27f49c`) | Inline hook (`pl::memory::HookHandle`) via single-match signature | Intercepts active camera culling. Forwards 8 pointer arguments. Evaluates telemetry only on first 20 calls or every 4096 calls, logging Camera World Position (`[this + 0x5c8]`) and Forward View Direction (`[this + 0x5d4]`) every 5 seconds. |

---

## Safety & Hardening Rules

- **Zero Game Mutation**: Every hook invokes the original implementation before performing telemetry (or returns original result).
- **Argument Register Preservation**: Game hooks accept 8 pointer-sized arguments (`void* a0..a7`) and forward all 8 to the original function to prevent dropping argument registers.
- **Pre-Execution Snapshotting**: `lightingTaskDetour` copies context qwords into locals before invoking original code in case context memory is freed on task completion.
- **Strict Anti-Spam Logging**: Detours log only the first 20 calls followed by aggregated 5-second interval summaries.
- **Zero Hot-Path Allocations & Overhead**: Bounded static ring buffers are used for frame binding tracking, and `cullPointOrChunk` throttles clock/TID checks to 1 in 4096 calls.
- **Graceful Degradation**: If a signature or symbol fails to resolve at runtime, the failure is logged and only that probe is disabled. The mod continues running.
- **Safe Unhooking**: Detours capture the original function pointer locally and never skip calling it; original function pointers are cleared to `nullptr` only after hooks are reset.

---

## Build & Packaging

Build using the bundled packaging script (requires Android NDK r28c / CMake / Ninja):

```bash
./scripts/package.sh
```

Packaged artifacts will be generated in:
- `dist/arm64-v8a/lod_poc.levipack` (Installable mod archive for LeviLaunchroid)
- `dist/arm64-v8a/lod_poc/liblod_poc.so`

---

## Reading Logs

The mod writes real-time logs to on-device files on every launch so logs can be inspected directly on the phone without needing ADB or a PC.

### Log Locations on Phone

1. **Download Directory (Easiest)**:
   - Path: `/storage/emulated/0/Download/lod_poc.log`
   - Accessible using any standard Android file manager (Files by Google, Solid Explorer, etc.) without root or special permissions.
2. **Primary Mod Data Directory**:
   - Path: `<LeviLaunchroid Mod Data Directory>/lod_poc.log` (resolved at runtime via `NativeMod::getDataDir()`).

### Log Rotation & Crash Investigation

- On every launch, the previous log is automatically renamed to **`lod_poc.log.old`** (and any older `.old` is replaced), then a fresh `lod_poc.log` is created.
- Each line is flushed (`fflush`) immediately so all output survives unexpected game exits and crashes.
- File sizes are safely capped at 5 MB per run.

### What to Send for Debugging / Investigation

If the game crashes or freezes:
- Send **`lod_poc.log`** (current run).
- If you reopened Minecraft after the crash, the crash log will now be in **`lod_poc.log.old`**; please send **both** `lod_poc.log` and `lod_poc.log.old`.

### Real-Time ADB Logcat (Optional)

If monitoring live from a PC:
```bash
adb logcat | grep -iE "lod_poc|Probe"
```

---

## World Data Reader & Top-Down LOD Map

A background world scanner runs a few seconds after `enable()` (configured via `"buildLodMap": true` in `config.json`). It analyzes the Bedrock world save on disk **without modifying game memory, hooking LevelDB, or touching the render thread**:

### 1. Active World Discovery via `/proc/self/fd`
Scans the process file descriptor table using `readlink` to discover the active world's LevelDB directory (`.../minecraftWorlds/<world_id>/db`). Because Minecraft already holds `LOCK`, the reader bypasses standard LevelDB open calls and reads the immutable `.ldb` SSTable files directly using lockless raw streams.

### 2. Bedrock Chunk Parsing & Key Decoding
- **Data3D (Tag `0x2B`)**: Extracts top-surface heightmaps (`int16_t[256]` per chunk column).
- **SubChunk (Tag `0x2F`)**: Decompresses zlib-raw / snappy block storages, decodes little-endian NBT palettes, and samples surface block names (`minecraft:grass_block`, `stone`, `water`, `sand`, etc.).
- **Palette Color Mapping**: Converts block names to representative terrain RGB colors (with deterministic hash fallback for custom blocks).

### 3. Hierarchical LOD Tiers & Mesh Generation
- **Tier 1**: 2x2-block cells (chunks 6–16 away)
- **Tier 2**: 4x4-block cells (chunks 17–32 away)
- **Tier 3**: 8x8-block cells (chunks 33–64 away)
- **Tier 4**: 16x16-block cells (chunks 65+ away)
- **Downsampling**: Max height + average color downsampling.
- **Mesh Construction**: Generates low-poly heightfield tile meshes (256x256 blocks per 16x16-chunk tile) with 4-sided edge skirts that hide seams between differing LOD tiers.

### 4. Debug Top-Down Map (`lod_map.png`)
Writes an RGB top-down visualization of all discovered Overworld terrain:
- Saved to `<mod_data_dir>/lod_map.png` and copied to `/storage/emulated/0/Download/lod_map.png`.
- Scaled up to 1024x1024 max.
- Displays terrain features (oceans, plains, deserts, mountains, snow, forests).

### Limits & Scope
- **Already-Generated Chunks Only**: Scans only chunk records flushed to LevelDB `.ldb` SSTables on disk.
- **Overworld Only**: Nether and End dimensions are skipped.
- **Offline Snapshot**: Captures world state at scan time; no live per-tick chunk streaming yet.
- **Zero Render Overhead**: Generates mesh data and image in memory only; does not submit draw calls.
