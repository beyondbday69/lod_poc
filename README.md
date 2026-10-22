# LOD PoC (Proof-of-Concept) Native Mod

A LeviLaunchroid native mod (`arm64-v8a`) for Minecraft Bedrock 1.26.51.1 built on `preloader-android` (SDK commit `5d108da350ce3e7f13f51415f81e65344ecd5d14`).

This repository implements **log-only, non-intrusive diagnostic probes** designed to verify hook points, graphics runtime context, chunk delivery pathways, and camera matrices for a future "Distant Horizons"-style LOD engine.

---

## Probes Implemented

| Probe | Target | Mechanism | Goal & Telemetry |
|---|---|---|---|
| **1. Chunk Lighting** | `ChunkSource::_lightingTask` (VA `0x103dce4c`) | Inline hook (`pl::memory::HookHandle`) via single-match signature | Observes chunk preparation frequency and thread context. Reads first 4 quadwords of context `x0` (`[q0, q1, q2, q3]`) before invoking the original, forwards 8 pointer arguments, logs first 20 calls followed by 5s call rate summaries. |
| **2. FBO Binding** | `glBindFramebuffer` | Inline detour via `dlsym(libGLESv2, "glBindFramebuffer")` | Intercepts OpenGL ES framebuffer bindings to locate scene-to-post processing passes. Records per-frame sequence of FBO IDs (logged every 120 frames) + thread ID. |
| **3. Frame Swap** | `eglSwapBuffers` | Inline detour via `dlsym(libEGL, "eglSwapBuffers")` | Serves as the frame boundary delimiter. Measures and reports true rendering FPS every 5 seconds along with thread ID. |
| **4. SubChunk Lookup** | `SubChunk::getBlock` (VA `0x1032a354`) | **Signature resolution only** (no detour) | Verifies runtime resolution of block storage access (`kSigSubChunkGetBlockStorage`) and validates Bedrock index formula `(x << 8) \| (z << 4) \| y`. |
| **5. Camera Culling** | `LevelRendererCamera::cullPointOrChunk` (VA `0xb27f49c`) | Inline hook (`pl::memory::HookHandle`) via single-match signature | Intercepts active camera culling. Forwards 8 pointer arguments. Evaluates telemetry only on first 20 calls or every 4096 calls, logging Camera World Position and Direction every 5 seconds. |
| **Probe A. GL Frame Trace** | OpenGL ES Render Calls (`glClear`, `glViewport`, `glScissor`, `glEnable`, `glDisable`, `glDepthMask`, `glDepthFunc`, `glBlendFunc`, `glCullFace`, `glUseProgram`, `glBindFramebuffer`, `glDrawElements`, `glDrawArrays`, etc.) | Inline detours via `dlsym(libGLESv2.so)` on the render thread | Single-frame execution trace recording up to 6000 events. Identifies where 3D world geometry is drawn vs 2D UI passes. |
| **Probe B. Camera Position Discovery** | `LevelRendererCamera::cullPointOrChunk` (`a0` object) | Safe memory snapshots (`process_vm_readv`) at T1 (+15s), T2 (+25s), T3 (+35s) | Differential memory scanning across 0x1000 bytes of `a0` and sub-objects `*(a0 + 0x8)`, `*(a0 + 0x10)`, `*(a0 + 0x18)`. Identifies candidate 3-consecutive-float world coordinates. |

---

## Probe A: GL Frame Trace

### How It Works
- Hooks GL functions using `dlsym` on `libGLESv2.so`.
- Runs **strictly on the render thread** (the thread executing `eglSwapBuffers`). Worker threads performing background shader compiles or texture uploads are ignored.
- Under normal gameplay, the hooks only increment lockless atomic counters with zero allocations and zero log spam.
- When **armed** for a single frame, events are captured into a preallocated ring buffer (max 6000 events).
- At the subsequent `eglSwapBuffers`, a compressed state summary is dumped to the log:
  - Consecutive identical draw calls are collapsed as `DRAW x N (type, mode, count)`.
  - Every `glClear`, `glViewport`, `glScissor`, `glEnable`/`glDisable` (`GL_DEPTH_TEST`, `GL_BLEND`, `GL_CULL_FACE`), depth mask/func change, program change, and FBO bind is listed with the count of draw calls executed between each state change.
  - Summary metrics report: total draw calls, largest run of draws with depth test ON, index of first draw after last depth clear, and index where depth test is first disabled after the main 3D run (UI start candidate).

### How to Arm the Traces
1. **Automatic Main Menu Trace**: Runs once shortly after startup (~frame 60) when menu rendering is active (`main_menu`).
2. **Automatic In-World Trace**: The mod keeps a single permanent, cheap atomic counter hooking `glDrawElements`, `glDrawElementsInstanced`, and `glDrawArraysInstanced` with zero logging and zero allocations per frame. When draws-per-frame > 500 for 60 consecutive frames, it logs `"world detected"` and arms the `in_world` trace after `traceDelayAfterWorldSec` (default 10s), reinstalling the heavy hooks for exactly one frame to log the PASS SUMMARY before immediately removing them again.
3. **Manual Re-Arming via `config.json`**:
   Edit `<mod_data_dir>/config.json` or `<mod_config_dir>/config.json`:
   ```json
   {
     "traceFrame": true,
     "traceLabel": "inventory"
   }
   ```
   The mod polls config every 2 seconds. When `traceLabel` changes or is re-armed, it captures exactly one frame with that label.

---

## Probe B: Camera Position Discovery

### How It Works
- Inside `cullPointOrChunk(a0, ...)`:
  - At **T1 = 15 s** after the first call: takes a safe memory snapshot of the first `0x1000` bytes of `a0` (and sub-pointers `*(a0 + 0x8)`, `*(a0 + 0x10)`, `*(a0 + 0x18)`).
  - At **T2 = 25 s** (+10 s): takes snapshot 2 while the player walks or turns.
  - At **T3 = 35 s** (+10 s): takes snapshot 3 while the player walks or turns again.
- Diff analysis:
  - Scans every 4-byte float offset from `+0x000` to `+0xFFC` and logs all offsets whose values changed between T1, T2, and T3.
  - Evaluates every 3 consecutive floats and isolates candidate world coordinate triplets (`|val| < 30000000`, not NaN/Inf, and changed between snapshots).

### F3-Free Hint & Matching
- While walking in-game during the 15s–35s window, note down your approximate player coordinates at T1, T2, and T3.
- You can store your coordinates in `config.json`:
  ```json
  {
    "probeCameraDiff": true,
    "userNote": "T1=(100, 64, -200), T2=(105, 64, -195), T3=(110, 64, -190)"
  }
  ```
- Compare your noted coordinates against the candidate triplets logged by Probe B.

---

## Stage 2: Depth-Tested Test Quad (`pocQuad`)

### Purpose & What It Tests
Stage 2 provides a visual, non-intrusive test that verifies the OpenGL ES depth buffer behavior and rendering pipeline in Minecraft Bedrock on real hardware:
1. **Depth Buffer Verification**: Injects a fullscreen quad rendered with clip-space depth $Z_{NDC} = \text{pocDepth} \times 2 - 1$ directly into Bedrock's main world framebuffer (`FBO 0`) with `GL_DEPTH_TEST` enabled (depth func `GL_LEQUAL` matching terrain) and depth write mask disabled.
2. **Terrain Occlusion**:
   - At `pocDepth = 0.0` ($Z = -1.0$, near plane), the quad appears in front of all world terrain.
   - At `pocDepth = 0.5` ($Z = 0.0$), terrain blocks closer than mid-depth occlude the quad, while further terrain blocks are hidden behind the quad.
   - At `pocDepth = 1.0` ($Z = +1.0$, far plane), all terrain blocks occlude the quad (only visible in empty sky).
3. **Pipeline Trigger Points**:
   - `"before_fbo1"`: Drawn inside `glBindFramebuffer` immediately before Bedrock transitions from world rendering on `FBO 0` to post-processing on `FBO 1`.
   - `"after_depth_clear"`: Drawn inside `glClear` immediately after `GL_DEPTH_BUFFER_BIT` clear on `FBO 0`.
4. **Zero-Impact State Isolation**:
   - Saves and restores every OpenGL ES state touched (`GL_CURRENT_PROGRAM`, VAO, VBO, EBO, texture unit & binding, UBOs 0..3, viewport, scissor, depth test/mask/func/range, blend enable/factors/equation, cull face, stencil, color mask, and draw/read FBOs).
   - Thread-local re-entrancy guard ensures mod draw calls never re-trigger detours.
   - Built-in kill switch shuts down the quad permanently if any OpenGL error (`glGetError`) occurs.
5. **Live Reload**:
   - Change `pocQuad`, `pocDepth`, `pocColor`, or `pocTrigger` in `config.json` while the game is running. The mod detects file changes every 2 seconds and updates live without restarting.

### How to Enable & Test
In `<mod_config_dir>/config.json` (or `<mod_data_dir>/config.json`):
```json
{
  "pocQuad": true,
  "pocDepth": 0.5,
  "pocColor": "ff00ff",
  "pocTrigger": "before_fbo1"
}
```
Telemetry logs once per second while active:
```text
[PocQuad] Telemetry: draws=60, depth=0.500 (NDC z=0.000), func=LEQUAL, invalidated=false, fps=59.8
```

---

## Configuration (`config.json`)

Stored in `<mod_config_dir>/config.json` (or `<mod_data_dir>/config.json`):

| Key | Type | Default | Description |
|---|---|---|---|
| `traceFrame` | boolean | `true` | Enables single-frame GL trace. Traces `main_menu` and `in_world` automatically once. |
| `traceRaw` | boolean | `false` | When `false`, writes only the compact pass segment summary (~20 lines). When `true`, also dumps raw arguments for every draw call. |
| `traceLabel` | string | `""` | User label for manual re-arming (e.g. set to `"inventory"` or `"paused"` to capture a specific state). |
| `traceDelayAfterWorldSec` | integer | `10` | Delay in seconds after world detection (60 consecutive frames >500 draws) before capturing the in-world GL trace. |
| `pocQuad` | boolean | `false` | Stage 2: Enables live depth-tested test quad rendering into FBO 0. Zero overhead when false. |
| `pocDepth` | float (0..1) | `0.5` | Clip-space depth setting for test quad ($z_{NDC} = \text{depth} \times 2 - 1$). `0.0` = near plane, `1.0` = far plane. |
| `pocColor` | string | `"ff00ff"` | Hex color string for test quad (`"rrggbb"` or `"rrggbbaa"`). Default is magenta (`#ff00ff`). |
| `pocTrigger` | string | `"before_fbo1"` | Trigger point: `"before_fbo1"` (draw into FBO 0 immediately before switching to FBO 1) or `"after_depth_clear"` (draw immediately after `glClear(DEPTH)` on FBO 0). |
| `poc3d` | boolean | `true` | Stage 3: When true, renders a 3D textured/shaded cube anchored in the Minecraft 3D world using the captured camera matrices. |
| `poc3dDist` | float | `8.0` | Distance in blocks to anchor the 3D test cube in front of player camera when spawned. |
| `probeCameraDiff` | boolean | `true` | Enables Probe B 3-snapshot memory diffing on `cullPointOrChunk`. |
| `userNote` | string | `""` | Free-form notes with your recorded player coordinates at T1/T2/T3. |
| `requireKnownBuild` | boolean | `true` | When true (default), signature-based probes are skipped if the loaded `libminecraftpe.so` GNU build-id does not match `712509dc14ccc233e91f267937dfb46ecdcc4b68`. |
| `buildLodMap` | boolean | `true` | Enables background LevelDB scan and generation of `lod_map.png`. |
| `rescanIntervalSec` | integer | `0` | Optional periodic world rescan interval in seconds. `0` scans once. The first scan waits for the game to open the world database. |
| `probe_chunk_lighting` | boolean | `true` | Enables Probe 1 `ChunkSource::_lightingTask` hook. |
| `probe_bind_framebuffer` | boolean | `true` | Enables Probe 2 FBO tracking. |
| `probe_egl_swap_buffers` | boolean | `true` | Enables Probe 3 FPS and frame swap tracking. |
| `probe_level_renderer_cull` | boolean | `true` | Enables Probe 5 `LevelRendererCamera::cullPointOrChunk` hook. |

---

## World Reader (Bedrock LevelDB)

The world reader runs entirely on a detached background thread and never scans
on the render thread.

- **Discovery**: At enable, a poller checks `/proc/self/fd` every 3 seconds for
  an open file under a `.../db/` directory (`.ldb`, `.log`, `MANIFEST*`, or
  `CURRENT`). It logs `waiting for world...` at start and every 30 seconds, and
  gives up after 15 minutes. This avoids the old failure where scanning ran
  before Minecraft opened the world database.
- **Scan order**: Once discovered, all `*.ldb` files are read in ascending
  file-number order. LevelDB internal keys have their 8-byte trailer
  `(sequence << 8) | type` stripped. For each user key, the highest sequence
  across all files wins; type `0` deletions are honored. Overworld chunk keys
  are 9 or 10 bytes (`x, z, tag[, subY]`); 13/14-byte dimension keys are
  ignored.
- **Formats**:
  - Real little-endian NBT palette parsing (root compound `0x0A`, empty root
    name, nested tags through `End`). Palette entries are read from their
    `name` string.
  - SubChunk versions 8 and 9, including one or more block layers, palette
    type 0 (int32 count) and type 1 (zigzag varint count), bits per block
    `1..6, 8, 16`, little-endian 32-bit words, and YZX block order
    `(x << 8) | (z << 4) | y`.
  - Per column, SubChunks are scanned from the highest Y downward for the
    first non-air block. Plants and transparent decoration such as
    `tall_grass`, flowers, `snow_layer`, carpets, and coral fans are skipped
    when identifiable. Water counts as a top block.
  - Data3D (`0x2B`) is used only as a fallback height (`raw - 64`, clamped to
    `-64..319`) and for a first-10-chunk cross-check log. SubChunk data is
    authoritative. Data2D (`0x2D`) is counted as unsupported.
- **Colors**: Resolved blocks use the block palette. Unresolved columns are
  visible dark gray `(64,64,64)`; parse failures are magenta `(255,0,255)`.
  The log reports resolved/unresolved/parse-error column counts, 5 sample
  chunks, and the top 20 top-block names.
- **Outputs**: `lod_map.png` is written next to `lod_poc.log` and, when the
  directory exists, copied to
  `/storage/emulated/0/Download/lod_map.png`.
- **Rescan**: `rescanIntervalSec` defaults to `0` (scan once). A positive value
  keeps the worker alive and rescans the discovered database at that interval.

---

## Build-ID Safety Gate & Signatures

All game function signatures (`ChunkSource::_lightingTask`, `SubChunk::getBlock`, `LevelRendererCamera::cullPointOrChunk`) are reverse-engineered specifically against Minecraft Bedrock 1.26.51.1 arm64-v8a with GNU build-id:
```text
712509dc14ccc233e91f267937dfb46ecdcc4b68
```

- **Build-ID Verification**: At `load()`, the mod inspects `libminecraftpe.so` using zero-fault `process_vm_readv` ELF parsing and logs the loaded build-id, disk path, and file size on the very first lines of `lod_poc.log`.
- **Mismatch Safety Gate**: If `requireKnownBuild` is `true` (default) and the build-id differs (e.g. patched APK, minor sub-version update, or different architecture):
  - Signature-based probes (Probes 1, 4, 5) are **safely disabled** to prevent crashes or hooking arbitrary code.
  - GL/EGL detours (FBO tracking, frame swap, GL frame trace), world LevelDB reader, file logging, and crash handlers **continue running normally**.
  - To attempt resolving signatures on a non-matching binary at your own risk, set `"requireKnownBuild": false` in `config.json`.
- **Safe Signature Diagnostics**: If a signature fails full resolution, prefix checks (16, 24, 32, 40 bytes) are delegated strictly to `pl::memory::resolveSignature`. The mod does not perform raw memory scans across `libminecraftpe.so`, preventing unmapped page faults (`SEGV_ACCERR`).

---

## What Lines to Send

Open `/storage/emulated/0/Download/lod_poc.log` (or retrieve via ADB) and send the following blocks:

1. **Build-ID & GL Context**:
   ```text
   [Build-ID] libminecraftpe.so path: ...
   [Build-ID] Loaded GNU Build-ID: ...
   [Build-ID] Matches known RE binary (712509dc14ccc233e91f267937dfb46ecdcc4b68): ...
   [GL Context] Renderer: ...
   [GL Context] Version:  ...
   ```

2. **Signature Diagnostics** (if any signature failed full resolution):
   ```text
   [SigDiag] Signature '...' failed full resolution. Running prefix scan...
   [SigDiag]   -> Prefix 16 bytes: ...
   [SigDiag]   -> Prefix 24 bytes: ...
   ```

3. **Pass Summary (Probe A)**:
   ```text
   [GLTrace] ==================== PASS SUMMARY: in_world ====================
   [GLTrace] SEGMENT #...: FBO ... | Viewport: [...] | Attachments: [...]
   [GLTrace]   Draws: ... | SUM(count): ... | Instanced: ...
   [GLTrace]   State: DepthTest=..., DepthMask=..., Blend=... | Distinct Programs: ...
   [GLTrace] >>> TERRAIN CANDIDATE: Segment #... <<<
   [GLTrace] TOP 3 CANDIDATE SEGMENTS ...
   ```

4. **Camera Coordinates (Probe B)**:
   ```text
   [Probe B: CameraProbe] ==================== CAMERA DIFF ANALYSIS ====================
   [Probe B] CANDIDATE WORLD COORDS [a0 (LevelRendererCamera)] at +0x...
     T1: (x, y, z)
     T2: (x, y, z)
     T3: (x, y, z)
     Delta(T2-T1): ...
     Delta(T3-T2): ...
   ```

5. **World Reader**:
   ```text
   [LOD Reader] waiting for world... polling /proc/self/fd ...
   [LOD Reader] Discovered active world DB directory: ...
   [LOD Reader] Found N .ldb files ... scanning in ascending file-number order
   [LOD Reader] LevelDB records: total=..., overworld=..., ...
   [LOD Reader] SubChunk versions: v8=..., v9=...; skipped: ...
   [LOD Reader] Data3D cross-check chunk (x, z): ...
   [LOD Reader] Finalized N chunks ... resolved=..., unresolved=...
   [LOD Reader] Sample chunk (x, z): top block at column (8,8) = ...
   [LOD Reader] Top block #1: ... = ... columns
   [LOD Map] Saved primary PNG map next to log: ...
   ```

---

## Safety & Hardening Rules

- **Zero Game Mutation**: Every hook invokes the original implementation before performing telemetry (or returns original result).
- **Argument Register Preservation**: Game hooks accept 8 pointer-sized arguments (`void* a0..a7`) and forward all 8 to the original function to prevent dropping argument registers.
- **Async-Safe / Syscall Memory Guard**: `process_vm_readv` syscall ensures that invalid pointer dereferences in `CameraProbe` safely return an error without triggering `SIGSEGV`.
- **Pre-Execution Snapshotting**: `lightingTaskDetour` copies context qwords into locals before invoking original code in case context memory is freed on task completion.
- **Strict Anti-Spam Logging**: Detours log only on initial invocations followed by aggregated intervals or single-frame summaries.
- **Graceful Degradation**: If a GL function or signature fails to hook at runtime, only that specific hook is disabled; the mod never crashes.

---

## Build & Packaging

Build using the bundled packaging script (requires Android NDK r28c / CMake / Ninja):

```bash
./scripts/package.sh
```

Packaged artifacts:
- `dist/arm64-v8a/lod_poc.levipack` (Installable mod archive for LeviLaunchroid)
- `dist/arm64-v8a/lod_poc/liblod_poc.so`

---

## Verification & Status

- **Compiles**: Cleanly builds with Android NDK r28c (Clang 17+, C++20) for `arm64-v8a`. Verified green in GitHub Actions CI.
- **Untested Parts**:
  - Exact in-world GL call order on physical Mali/Adreno driver (must be confirmed from on-device `lod_poc.log`).
  - True struct layout of `LevelRendererCamera` on Bedrock 1.26.51.1 (Probe B will reveal the exact offset from player movement diffs).
