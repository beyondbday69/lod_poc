# Claude Handoff — LOD PoC (`/root/lod_poc`)

This file is the working handoff for the next Claude/agent session. It records
what was actually changed, why, how the world reader works, and what still needs
verification on a real device.

---

## 1. Project purpose

`lod_poc` is an `arm64-v8a` preload-native mod for Minecraft Bedrock
`1.26.51.1` on LeviLaunchroid. It is a diagnostics/proof-of-concept project for
a future Distant Horizons-style LOD renderer.

The repository currently contains:

- GL/EGL tracing and FBO probes.
- Camera-position discovery probes.
- A depth-tested debug quad and an in-world debug cube.
- A **background Bedrock LevelDB world reader** that builds a top-down
  `lod_map.png` and mesh statistics.

The mod is not a renderer yet. The world reader is a data-extraction
foundation; no LOD terrain is injected into the game render pass.

---

## 2. Current repository state

- Branch: `main`
- Last released/tagged commit before this work: `3bf1cf8`
  (`v0.1.5`, cube double-sided/depth fix).
- Current handoff version: `0.1.6` (see `manifest.json`).
- Remote: `origin` (GitHub).
- CI: `.github/workflows/build.yml` builds with Android NDK r28c and publishes
  release assets for `v*` tags.
- `v0.1.5` CI runs `39752366987` and `39752322134` were green.
- The first `0.1.6` CI attempt (`39762966634`) failed only on two references to
  `LdbReader::ScanState`; `ScanState` is namespace-level. Fixed in `2796877`.
- `0.1.6` CI run `39763771134` is green (Android build, package, artifact).
- `v0.1.6` is tagged and released. Tag CI run `39764493359` passed and
  published `lod_poc.levipack` and `liblod_poc.so`:
  https://github.com/beyondbday69/lod_poc/releases/tag/v0.1.6

Uncommitted-at-the-start files that were changed:

- `src/mod/LdbReader.h`
- `src/mod/LdbReader.cpp`
- `src/mod/WorldReader.h`
- `src/mod/WorldReader.cpp`
- `src/mod/Config.h`
- `src/mod/LodPocMod.cpp`
- `src/mod/LodMesh.cpp`
- `README.md`
- `manifest.json`
- `CLAUDE.md` (this file)

---

## 3. What was actually done

### 3.1 Prior history (already committed)

| Version | Commit | Work |
|---|---|---|
| `v0.1.0` | `8f9b425` | Fix NativeMod pointer dereference. |
| `v0.1.1` | `2864556` | Safe `lightingTask` snapshot, 8-argument forwarding, throttled cull telemetry. |
| `v0.1.2` | `1c98f69` | File logging, crash capture, heartbeat telemetry. |
| `v0.1.3` | `be3a07d` | First LevelDB reader, LOD tier mesh builder, `lod_map.png`. |
| `v0.1.4` | `81f4b4b` … `bbd0d53` | GL frame trace, camera probe, build-id gate, SigDiag. |
| `v0.1.5` | `3bf1cf8` | Stage 3 debug cube: double-sided rendering, depth writes, corrected winding. |

The cube is intentionally a temporary floating debug marker spawned in front
of the camera at eye level. It is not LOD terrain. Do not remove it as part of
world-reader work.

### 3.2 This rewrite (`0.1.6`)

The old reader scanned once during `enable()` and failed before Minecraft had
opened the save database. The on-device log showed:

```text
[LOD Reader] Starting background Bedrock world scan...
[LOD Reader] No active world DB found in open file descriptors. Checked 0 candidate files.
```

The reader also had format weaknesses: it depended on byte-scanning palette NBT
for block names, assumed a particular file order, did not honor sequence
deletions correctly, and did not parse SubChunk palettes/index arrays properly.

This rewrite replaces the reader end to end:

1. **`LdbReader.h` / `LdbReader.cpp`**
   - Standalone LevelDB SSTable parser:
     - Footer magic/version check.
     - Index block → data block handles.
     - Block compression trailers (CRC ignored, bounds checked).
     - Snappy decompression implemented locally.
     - Raw-deflate and zlib-wrapped decompression through zlib.
     - 64 MB cap on decompressed block size.
   - LevelDB internal-key handling:
     - Strips the last 8-byte trailer.
     - `sequence = trailer >> 8`, `type = trailer & 0xFF`.
     - Only type 0 (deletion) and type 1 (value) survive parsing.
     - Highest sequence per user key wins across all files.
     - Deletions win over lower-sequence values.
     - Equal sequences use the later/higher file number.
   - Overworld key handling:
     - 9-byte keys: `[int32 x][int32 z][tag]`.
     - 10-byte keys: adds signed `subY`.
     - 13/14-byte dimension keys are ignored.
     - `0x2B` = Data3D heightmap.
     - `0x2D` = Data2D, counted as unsupported.
     - `0x2F` = SubChunk; requires `subY`, `-4..19`.
     - Other tags are counted in `ignoredTags`.
   - Real little-endian NBT parser for palette entries:
     - Root compound `0x0A` with a name string (normally empty).
     - Reads the `name` string tag from each palette compound.
     - Supports byte, short, int, long, float, double, byte array, string,
       list, compound, int array, long array, and `End`.
     - Nested compounds/lists are skipped safely with depth limits.
     - Bounds-checked reads only; no pointer walking past the value buffer.
   - SubChunk parser:
     - Versions 8 and 9 only.
     - Version 9 reads the payload Y-index byte and compares it with the key
       `subY`; the key remains authoritative and mismatches are counted.
     - Unsupported versions are skipped and counted by version.
     - One or more block layers; layer count capped at 16.
     - Layer header: bit 0 = palette type, bits 1–7 = bits per block.
     - Supported bits per block: `0,1,2,3,4,5,6,8,16`.
     - Palette type 0: int32 little-endian palette count.
     - Palette type 1: zigzag varint palette count.
     - Layer indices are little-endian `uint32` words, `floor(32/bits)` indices
       per word, `ceil(4096 / indicesPerWord) * 4` bytes.
     - Block index is `(x << 8) | (z << 4) | y`.
     - For each column and layer, scans local Y `15..0` for the first non-air,
       non-decoration block. Stores only the per-column top candidate, keeping
       memory bounded.
     - Layers are bottom-to-top; a later layer wins at the same local Y.
     - Palette indices outside the palette are parse errors.
     - A SubChunk parse error is stored as `SubChunkTopData.parseError`; it
       masks lower SubChunks for that column and produces magenta instead of
       silently using a lower top.
     - Unsupported versions do **not** mask lower SubChunks.
   - Data3D parser:
     - Requires at least 512 bytes.
     - 256 little-endian `int16` heights.
     - Used only as a fallback and for cross-check logging.
   - Finalization is order-independent:
     - All files are read first; records are held as winning per-user-key
       entries.
     - `finalizeChunks()` applies survivors, builds per-chunk column data,
       sorts SubChunks by descending `subY`, computes top blocks/colors, and
       frees the raw record map.
   - Sentinel colors:
     - Unresolved: dark gray `RGB(64,64,64)`.
     - Parse error: magenta `RGB(255,0,255)`.
     - No unresolved/parse-error column is ever grass-green.
   - Stats include file counts, records, deletions, superseded entries,
     ignored dimensions/tags, Data2D/Data3D/SubChunk counts, per-version
     counts, skipped versions, Y-index mismatches, out-of-range SubChunks,
     malformed blocks, finalized chunks, resolved/unresolved/parse-error
     columns.

2. **`WorldReader.h` / `WorldReader.cpp`**
   - `startBackgroundScan(primaryDir, rescanIntervalSec)` starts a detached
     worker; it never parses on the render thread.
   - Polls `/proc/self/fd` every 3 seconds for an open file under a
     `.../db/` directory:
     - `.ldb`
     - `.log`
     - `MANIFEST*`
     - `CURRENT`
   - Strips `" (deleted)"` from `/proc` link targets.
   - Logs `waiting for world...` immediately and every 30 seconds.
   - Gives up after 15 minutes.
   - Scans `.ldb` files in ascending file-number order.
   - Logs scan stats, Data3D cross-checks for the first 10 chunks, 5 sample
     chunks at column `(8,8)`, and the top 20 top-block names.
   - Generates a top-down PNG:
     - `lod_map.png` next to `lod_poc.log` (mod data directory).
     - `/storage/emulated/0/Download/lod_map.png` when that directory exists.
   - Optional rescan loop uses `rescanIntervalSec`; `0` means scan once.
   - Restart logic:
     - `gLaunched` prevents duplicate workers.
     - `gRunning` tracks a live worker.
     - `gRestartRequested` allows a finished one-shot worker to be restarted
       when the interval changes from 0 to positive.
     - Interval changes are applied between scans; long rescans are sliced into
       250 ms sleeps.

3. **`Config.h` / `LodPocMod.cpp`**
   - Added `int rescanIntervalSec = 0;` to `ModConfig`.
   - `enable()` reads `buildLodMap` and `rescanIntervalSec` and starts the
     background scan. No game-hook internals changed.
   - `checkConfigReload()` updates `WorldReader`:
     - if `buildLodMap` is true, calls `startBackgroundScan(...)`;
     - if false, calls `setRescanIntervalSec(0)`.
   - The call is idempotent while a worker is running and cheap when idle.

4. **`LodMesh.cpp`**
   - Missing/unresolved grid colors default to `kUnresolvedColor`.
   - Column lookup corrected to `(localX << 4) | localZ` to match the reader.
   - No game hooks or cube code touched.

5. **`README.md`**
   - Added `rescanIntervalSec` to the config table.
   - Added a World Reader section covering discovery timing, scan order,
     formats, colors, outputs, and rescans.
   - Added a “World Reader” log block to “What Lines to Send”.

6. **`manifest.json`**
   - Version bumped to `0.1.6`.

---

## 4. Critical indexing convention

Bedrock SubChunk indices are **YZX**: Y changes fastest, then Z, then X.

```text
blockIndex = (x << 8) | (z << 4) | y
```

Therefore, a 16×16 column array flattened without Y is:

```text
column = (x << 4) | z
```

X is the slow axis; Z is the fast axis. Data3D is documented as “ZX order
(Z increments first)”, which is the same `column = (x << 4) | z` layout.

This convention is used consistently in:

- `LdbReader.cpp` SubChunk column scan.
- `LdbReader.cpp` Data3D fallback indexing.
- `WorldReader.cpp` map generation.
- `WorldReader.cpp` Data3D cross-check sample `(localX, localZ)`.
- `LodMesh.cpp` `colIdx`.

Do **not** revert any of these to `(z << 4) | x`; that transposes the map and
mesh.

The Data3D fallback height is world-min-relative:

```text
realY = rawHeight - 64
```

Clamp to `-64..319` (Overworld range).

---

## 5. Threading and lifecycle

- `enable()` runs on the mod/load thread. It only starts the worker.
- `checkConfigReload()` runs on the render thread every 2 seconds. It only
  updates atomics or starts the worker; it never scans files.
- The worker is detached and does all discovery, parsing, PNG generation, and
  logging.
- The worker logs through `LodPocMod::logAll`, which writes to the mod logger
  and `lod_poc.log`.
- The worker does not hold the LevelDB lock; it only reads immutable `.ldb`
  files. It does not open the database.
- A rescan creates a fresh `ScanState`; previous chunk data is not reused.
- If the DB is no longer open, the rescan loop logs a warning and stops.
- `rescanIntervalSec` changes while the worker is running are picked up between
  scans. A 0 → positive change can restart a finished one-shot worker through
  `gRestartRequested`, provided `startBackgroundScan` is called again (the
  config reload path does this every 2 seconds).

---

## 6. Output and log locations

- Main log: `<mod data dir>/lod_poc.log`
- Map next to log: `<mod data dir>/lod_map.png`
- Download copy: `/storage/emulated/0/Download/lod_map.png`
- The Download copy is best-effort; Android scoped storage may deny it. Failure
  is logged and the primary map is still written.

Useful log lines:

```text
[LOD Reader] waiting for world... polling /proc/self/fd every 3s (timeout 15 min)
[LOD Reader] Discovered active world DB directory: ...
[LOD Reader] Found N .ldb files (total size: ... MB), scanning in ascending file-number order
[LOD Reader] LevelDB records: total=..., overworld=..., values=..., deletions=..., superseded=...
[LOD Reader] Chunk tags: Data3D=..., SubChunk=..., Data2D(unsupported)=...
[LOD Reader] SubChunk versions: v8=..., v9=...; skipped: ...
[LOD Reader] SubChunk parse errors=..., Y-index mismatches=..., out-of-range skipped=...
[LOD Reader] Finalized N chunks (... columns): resolved=..., unresolved=..., parse-error=...
[LOD Reader] Data3D cross-check chunk (x, z): ...
[LOD Reader] Sample chunk (x, z): top block at column (8,8) = ... y=...
[LOD Reader] Top block #1: ... = ... columns
[LOD Map] Saved primary PNG map next to log: ...
[LOD Map] Copied PNG map to Download: ...
[LOD Reader] Complete: N chunks parsed in ...s, approximate peak memory: ... MB
```

---

## 7. Validation performed locally

There is no Android NDK in this environment, so the full mod is compiled only
by GitHub Actions. The host-testable reader was verified directly.

### Host compile

```bash
cd /root/lod_poc
g++ -std=c++20 -Wall -Wextra -Wpedantic -Isrc \
    -c src/mod/LdbReader.cpp -o /tmp/LdbReader.o
```

Result: clean, no warnings.

### Synthetic SSTable tests

Artifacts:

- `/tmp/gen_test_ldb.py` — generates synthetic LevelDB SSTables.
- `/tmp/test_ldb.cpp` — host harness for `LdbReader`.
- `/tmp/test_snappy.cpp` — host harness for Snappy decompression.
- `/tmp/ldb-test/` — generated test database.

Build/run:

```bash
cd /root/lod_poc
python3 /tmp/gen_test_ldb.py /tmp/ldb-test
g++ -std=c++20 -Wall -Wextra -Wpedantic -Isrc \
    /tmp/test_ldb.cpp src/mod/LdbReader.cpp -lz -o /tmp/test_ldb
/tmp/test_ldb /tmp/ldb-test/000001.ldb /tmp/ldb-test/000002.ldb
```

Covered by the synthetic tests:

- LevelDB footer/index/data-block parsing.
- Internal-key stripping and sequence resolution.
- Deletion records.
- Real NBT palette parsing.
- SubChunk v8 and v9.
- Unsupported v7 skip with count.
- Bits-per-block 1 and 2 (including decoration skipping).
- Multiple layers in the test data.
- Asymmetric `(x=1,z=2)` vs `(x=2,z=1)` columns for SubChunk and Data3D.
- Data3D fallback (`raw - 64`).
- Malformed SubChunk → magenta, and lower SubChunk masking.
- Y-index mismatch count.
- Out-of-range SubChunk Y skip.
- Multiple data blocks in one `.ldb`.
- Uncompressed, raw-deflate, and zlib-wrapped blocks.
- Snappy literal and copy type 1/2/4 streams.

The current synthetic test files are **not** committed; they are temporary
host-only artifacts under `/tmp`.

---

## 8. Known limitations / risks

1. **Android build not locally verified.** `WorldReader.cpp` depends on
   preloader and mod headers. CI is the compile check.
2. **Direct Download write may fail** on Android 11+ scoped storage. The code
   logs the failure and still writes next to `lod_poc.log`.
3. **Palette colors are approximate.** `getBlockColor()` maps common block
   names to hand-picked RGB values and hashes unknown names deterministically.
   It is a diagnostic map, not a resource-pack-accurate texture atlas.
4. **Top-block heuristic.** Decoration detection is name-based and
   intentionally conservative. Some modded/renamed plants may still become the
   top block. Add names to `isSkippableDecoration()` when identified.
5. **Water counts as top block**, per the task requirements. It currently gets
   a blue-ish palette color.
6. **No WAL/memtable parsing.** Only immutable `.ldb` files are read. If a
   world has only a `.log` WAL, the scan logs “No .ldb files found”. Waiting for
   the first `.ldb` is a future improvement.
7. **Single active DB directory.** `findActiveWorldDb()` returns the first
   matching directory. If multiple worlds are open simultaneously, the first
   one wins.
8. **Detached worker lifetime.** The worker is detached; if the mod is
   unloaded while it is still running, logging after unload is a theoretical
   risk. Normal app process teardown is not affected.
9. **One-shot worker restart is config-driven.** A finished `rescanIntervalSec=0`
   worker only restarts when the config reload path sees a positive interval
   and `buildLodMap` is still true.
10. **CRC is not validated.** The reader reads the 4-byte trailer CRC but does
    not reject mismatches; malformed data is caught by bounds checks and block
    parsers instead.

---

## 9. Device/test context

The on-device session that produced the old failure used a Minecraft build with:

```text
Loaded Build-ID:   56de9eed077631e03a31f4f58eb2f0e00071d335
Expected Build-ID: 712509dc14ccc233e91f267937dfb46ecdcc4b68
Renderer:          Mali-G615 MC2
GL Version:        OpenGL ES 3.2 v1.r44p1-01eac0...
```

Because the build-id does not match the reverse-engineered target, the
signature-based probes are skipped when `requireKnownBuild=true`. The world
reader and GL/EGL probes still run. Do not disable the build-id gate just to
make the world reader run; the reader does not depend on signatures.

Expected test procedure for `0.1.6`:

1. Install the new `.levipack`.
2. Launch Minecraft and open a world.
3. Leave the game running for at least 10 seconds.
4. Pull `/storage/emulated/0/Download/lod_poc.log` or the mod data log.
5. Confirm:
   - `waiting for world...` appears, followed by a discovered DB directory.
   - `.ldb` file count is non-zero.
   - `SubChunk versions: v8=..., v9=...` shows real data (v9 expected).
   - `resolved` column count is non-zero.
   - Sample chunks and top-block names look plausible.
   - `lod_map.png` exists and is not entirely dark gray/magenta.
6. If the map is transposed, check the column convention immediately (Section
   4). Do not “fix” it by swapping only one call site.
7. If all columns are dark gray, inspect `Data2D(unsupported)`, SubChunk
   version counts, and parse-error counts before assuming a layout bug.

---

## 10. Guardrails for the next agent

- **Do not touch game hooks** for world-reader work:
  - `src/mod/GlTrace.cpp`
  - `src/mod/PocQuad.cpp`
  - `src/mod/CameraProbe.cpp`
- Do not remove the Stage 3 debug cube; it is a separate deliverable.
- Do not scan the world on the render thread.
- Do not add a dependency on a LevelDB library; the point is a standalone,
  lock-free `.ldb` reader.
- Keep all file/block/NBT/SubChunk reads bounds-checked.
- Keep the 64 MB decompressed-block cap.
- Keep Snappy and raw-zlib support.
- Keep unresolved dark gray and parse-error magenta distinct.
- Keep the `(x << 4) | z` column convention.
- Keep record resolution independent of file order; then apply after all files
  are read.
- Use the file log (`lod_poc.log`) for diagnostics; do not add noisy per-block
  logging.

---

## 11. Build / release commands

Host config generator only:

```bash
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/host --target levi_config_generator
```

Full Android package (requires NDK r28c):

```bash
./scripts/package.sh
```

Outputs:

```text
dist/arm64-v8a/lod_poc.levipack
dist/arm64-v8a/lod_poc/liblod_poc.so
```

Push and release:

```bash
git push origin main
git tag v0.1.6
git push origin v0.1.6
```

CI publishes release assets only for `v*` tags. Check the Actions run before
claiming the release is green.

---

## 12. Immediate next steps

1. Run a host CMake config-generator build to verify `Config.h` schema
   generation (optional; CI also covers it).
2. Commit and push `0.1.6`.
3. Wait for GitHub Actions Android build to pass.
4. Install the artifact on the device and follow Section 9.
5. If the log still says `waiting for world...` after 15 minutes:
   - Confirm the world was actually opened (not just the main menu).
   - Check `/proc/self/fd` access and that the DB path contains `/db/`.
   - Verify the game is not using a WAL-only state; wait for autosave/quit to
     flush `.ldb` files.
6. If the scan finds files but resolves zero columns:
   - Check skipped SubChunk versions and parse errors first.
   - Check whether the world uses Data2D only.
   - Do not blindly trust Data3D; it is fallback-only.
