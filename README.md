# LOD PoC (Proof-of-Concept) Native Mod

A LeviLaunchroid native mod (`arm64-v8a`) for Minecraft Bedrock 1.26.51.1 built on `preloader-android` (SDK commit `5d108da350ce3e7f13f51415f81e65344ecd5d14`).

This repository implements **log-only, non-intrusive diagnostic probes** designed to verify hook points, graphics runtime context, chunk delivery pathways, and camera matrices for a future "Distant Horizons"-style LOD engine.

---

## Probes Implemented

| Probe | Target | Mechanism | Goal & Telemetry |
|---|---|---|---|
| **1. Chunk Lighting** | `ChunkSource::_lightingTask` (VA `0x103dce4c`) | Inline hook (`pl::memory::HookHandle`) via single-match signature | Observes chunk preparation frequency and thread context. Logs first 20 calls + raw 4 quadwords of context `x0` (`[q0, q1, q2, q3]`) without dereferencing, followed by 5s call rate summaries. |
| **2. FBO Binding** | `glBindFramebuffer` | Inline detour via `dlsym(RTLD_DEFAULT, "glBindFramebuffer")` | Intercepts OpenGL ES framebuffer bindings to locate scene-to-post processing passes. Records per-frame sequence of FBO IDs (logged every 120 frames) + thread ID. |
| **3. Frame Swap** | `eglSwapBuffers` | Inline detour via `dlsym(RTLD_DEFAULT, "eglSwapBuffers")` | Serves as the frame boundary delimiter. Measures and reports true rendering FPS every 5 seconds along with thread ID. |
| **4. SubChunk Lookup** | `SubChunk::getBlock` (VA `0x1032a354`) | **Signature resolution only** (no detour) | Verifies runtime resolution of block storage access (`kSigSubChunkGetBlockStorage`) and validates Bedrock index formula `(x << 8) \| (z << 4) \| y`. |
| **5. Camera Culling** | `LevelRendererCamera::cullPointOrChunk` (VA `0xb27f49c`) | Inline hook (`pl::memory::HookHandle`) via single-match signature | Intercepts active camera culling. Extracts and logs Camera World Position (`[this + 0x5c8]`) and Forward View Direction (`[this + 0x5d4]`) every 5 seconds. |

---

## Safety & Hardening Rules

- **Zero Game Mutation**: Every hook invokes the original implementation first before performing lightweight telemetry.
- **Strict Anti-Spam Logging**: Detours log only the first 20 calls followed by aggregated 5-second interval summaries. No per-frame spam.
- **Zero Hot-Path Allocations**: Bounded static ring buffers and primitives are used for frame binding tracking.
- **Graceful Degradation**: If a signature or symbol fails to resolve at runtime, the failure is logged and only that probe is disabled. The mod continues running.
- **Clean Lifecycle**: Hooks are registered in `enable()`, safely unhooked in `disable()`, and all state is released in `unload()`.

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

## Log Inspection

Monitor the diagnostic output via `adb logcat`:

```bash
adb logcat -s LeviLaunchroid:I lod_poc:I
```
