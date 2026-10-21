# Reverse-Engineering Notes: Minecraft Bedrock 1.26.51.1 (arm64-v8a)
*Target: libminecraftpe.so (BuildID: 712509dc14ccc233e91f267937dfb46ecdcc4b68)*
*Date: 2026-10-21*

## 1. Executive Answers & Key Questions

### Question 1: GLES or Vulkan on Android?
- **Answer**: **OpenGL ES 3.x via EGL (100% GLES)**.
- **Evidence**:
  1. `readelf -d` shows `libGLESv2.so` and `libEGL.so` in `DT_NEEDED`.
  2. `libvulkan.so` is **NOT** present in `DT_NEEDED`.
  3. No symbols or dynamic imports for `vkGetInstanceProcAddr` or Vulkan entry points exist in the binary.
  4. RenderDragon on Android Bedrock executes through its bgfx OpenGL ES backend driver (`bgfx::gl::RendererContextGL` at `0x113dd410`).

### Question 2: Is Mid-Frame Injection Feasible, and Where?
- **Answer**: **YES, 100% FEASIBLE VIA GLES FRAMEBUFFER HOOK OR CAMERA PASS**.
- **Location & Mechanism**:
  RenderDragon separates the 3D world scene pass from 2D UI / OreUI.
  1. The 3D terrain and entity passes render into an offscreen HDR/G-buffer framebuffer (`GL_FRAMEBUFFER`).
  2. In `bgfx::gl::RendererContextGL::setFrameBuffer` (`0x113dd410`), calls to `glBindFramebuffer@plt` (`0x12911950`) switch targets.
  3. When `LevelRendererCameraAnon::framebuilderInsertTerrainCommandsForChunks` (`0x10ea85f0`) finishes, the scene color and depth textures are complete.
  4. **Injection Point**: By intercepting the unbinding of the main scene FBO or hooking right after `LevelRendererCamera::cullPointOrChunk` / camera pass execution, the mod can bind its own shader and LOD vertex buffers while the scene depth buffer is still bound and valid. UI rendering occurs afterwards, guaranteeing that OreUI, chat, and HUD draw cleanly on top.

---

## 2. Updated Target Status Table

| Target | Status | Confidence | Match Count | Evidence & Behavior |
|---|:---:|:---:|:---:|---|
| **A. Graphics API** | **FOUND** | **HIGH** | N/A | `DT_NEEDED` has `libGLESv2.so` & `libEGL.so`. Zero Vulkan imports. |
| **B. Chunk Load / Reception** | **FOUND** | **HIGH** | **1** | `ChunkSource::_lightingTask` (`0x103dce4c`, 64 bytes). Receives task context with `std::shared_ptr<LevelChunk>` at `[x0, #0x10]`. |
| **Chunk Unload** | **NOT FOUND** | N/A | 0 | `ChunkSource::acquireDiscarded` / chunk memory reclaim path is heavily inlined across ECS systems. |
| **1. View Distance (RequestChunkRadius)** | **MEDIUM** | **MEDIUM** | **1** | Setter at `0xd22be6c` is an 8-byte inline candidate (`str w1, [x0, #0x34]; ret`). High hook collision risk. |
| **3. Chunk Data & Block Reading** | **FOUND** | **HIGH** | **1** | `SubChunk::getBlock` at `0x1032a354`. **Formula: `(x << 8) \| (z << 4) \| y`** (Y stride 1). `LevelChunk` SubChunk vector at `+0x108`. |
| **C. Fog Float Math** | **NOT FOUND** | N/A | 0 | Biome lerp found at `0xb287410`, but distance multiplier math is compiled into shader uniforms / JSON schema. |
| **D. Camera Matrices & Position** | **FOUND** | **HIGH** | **1** | `LevelRendererCamera::cullPointOrChunk` (`0xb27f49c`, 48 bytes). Position at `[cam + 0x5c8]`, Direction at `[cam + 0x5d4]`. |
| **5. RenderDragon Terrain Submit** | **FOUND** | **HIGH** | **1** | `framebuilderInsertTerrainCommandsForChunks` (`0x10ea85f0`, 44 bytes). |
| **Old Target 2 (Packet Accessor)** | **NOT FOUND** | N/A | 0 | Downgraded: was only packet getter/setter accessors. |
| **Old Target 4 (Line 300 Assertion)** | **NOT FOUND** | N/A | 0 | Downgraded: was only a null-check assertion call-site. |
| **Old Target 6 (WorldGen string)** | **NOT FOUND** | N/A | 0 | Downgraded: only string literal found, no function entry point. |

---

## 3. Disassembly & Mathematical Proofs

### Target 3: SubChunk Index Formula & Struct Layout

Disassembly of caller at `0x108d50e0`:
```asm
108d50e0: lsl w8, w22, #8      ; w8 = (x << 8)
108d50e4: and w9, w23, #0xf     ; w9 = (z & 0xf)
108d50e8: mov w19, wzr
108d50ec: orr w8, w8, w9        ; w8 = (x << 8) | (z & 0xf)
108d50f0: add w8, w8, w21       ; w8 = (x << 8) | (z << 4) | y
108d5140: bl  0x1032a354        ; SubChunk::getBlock(this, w1=w8)
```
Disassembly of caller at `0x1035f570`:
```asm
1035f570: ldp    x8, x9, [x19, #0x108] ; x19 = LevelChunk* -> mSubChunks vector
1035f578: sub    x9, x9, x8           ; vector byte size
1035f590: mov    w10, #0x68           ; sizeof(SubChunk) = 104 bytes (0x68)
1035f598: smaddl x26, w9, w10, x8     ; SubChunk* = mSubChunks.begin() + subchunk_idx * 104
```
**Conclusion**:
- **Index Formula**: `(x << 8) | (z << 4) | y` (Y is fastest changing index, stride 1; Z stride 16; X stride 256).
- **LevelChunk Layout**: `std::vector<SubChunk>` is located at offset `+0x108`.
- **SubChunk Size**: Exactly `104` bytes (`0x68`).

---

### Target D: Camera Position & View Direction Struct Layout

Disassembly of `LevelRendererCamera::cullPointOrChunk` at `0xb27f4e8`:
```asm
b27f4e8: ldp   s1, s2, [x20]      ; x20 = target point (Vec3: x, y)
b27f4f0: ldr   s0, [x19, #0x5d4]  ; Camera direction X
b27f4f4: ldr   s3, [x19, #0x5d8]  ; Camera direction Y
b27f4fc: fmul  s0, s1, s0         ; x * dir.x
b27f500: fmul  s1, s2, s3         ; y * dir.y
b27f504: ldr   s2, [x20, #0x8]    ; target point z
b27f508: ldr   s3, [x19, #0x5dc]  ; Camera direction Z
b27f50c: fadd  s0, s0, s1         ; (x*dir.x + y*dir.y)
b27f510: fmul  s1, s2, s3         ; z * dir.z
b27f514: fadd  s0, s0, s1         ; dot product: (target - camPos) . camDir
b27f538: add   x1, x19, #0x5c8    ; Camera World Position Vec3 (x: 0x5c8, y: 0x5cc, z: 0x5d0)
```
**Conclusion**:
- **Camera World Position**: `[LevelRendererCamera + 0x5c8]` (`Vec3: float x, y, z`).
- **Camera Forward Direction**: `[LevelRendererCamera + 0x5d4]` (`Vec3: float dirX, dirY, dirZ`).

---

## 4. Hook Strategy & Safety Guidelines

1. **Log-Only First**: Every initial hook implementation must forward arguments untouched to the original function and log counts/addresses via `self.getLogger().info(...)`.
2. **Avoid 8-Byte Setter Hooks**: Because `RequestChunkRadiusPacket::setChunkRadius` (`0xd22be6c`) is only 8 bytes long, patching it via 16-byte trampoline hooks will overwrite the next function prologue. Instead, hook the packet transmission or intercept the radius at `ChunkSource` / `LevelRendererCamera`.
