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
