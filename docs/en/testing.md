# Testing

This document describes the CTest gate, the test inventory, and CI configuration.

## 1. Gate Status

CI has exactly one pass condition: every case in `ctest` must pass. If any case fails, `ctest` returns non-zero and the job fails (there is no `continue-on-error` and no skipping in CI). Configure uses `--no-tests=error`: if a CMake change ever results in zero registered cases, ctest exits with an error instead of falsely passing with "zero cases, zero failures".

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 2. Test Inventory

There are currently 30 cases across six hosts.

### 2.1 Math and Resource Rules (`GryceMathTests.exe`, 7 cases)

The tested sources (`engine/math/*.cpp`, `engine/assets/material_rules.cpp`, `engine/assets/mesh_lod.cpp`) are compiled directly into the test executable, which **links no engine DLL**, fully decoupling it from GPU, drivers, and window / headless environments. CI results come back in milliseconds. All cases are numeric checks of pure computation results, deterministic: the same code produces the same results on any machine.

| Case | Verifies |
| --- | --- |
| `math_vector` | `Vector2/3/4` add / sub / mul / div, dot and cross products, length and normalization, and the right-handed convention (X x Y = Z) |
| `math_matrix` | `Matrix4` translate / scale / rotate / multiply / transpose / inverse, plus perspective, ortho, look_at, and billboard projection results |
| `math_quaternion` | Axis-angle construction, conjugate and inverse, multiplication, slerp, `rotate_vector`, and round-trip consistency of `to_matrix` / `from_rotation_matrix` / Euler angles |
| `math_camera` | Default orientation and right-handed orthonormal basis, `look_at` inverse yaw/pitch, WASD movement, pitch clamping, view and projection matrices |
| `math_ray` | `Ray::at` evaluation, slab-method AABB intersection (hit / miss / inside origin) and hit distances |
| `math_material` | Derivation rules from model-file materials to PBR parameters (`assets/material_rules.cpp`, see [model-import.md](model-import.md)) |
| `math_lod` | Structural guarantees of the automatic LOD chain (`assets/mesh_lod.cpp`, see [lod.md](lod.md)) |

### 2.2 Toolchain (`GryceGCTests.exe`, 11 cases)

| Case | Verifies |
| --- | --- |
| `grycegc_cli` | Command-line parsing and command dispatch |
| `grycegc_crc32` | CRC32 implementation |
| `grycegc_pe` | PE trailer / VERSIONINFO read/write |
| `grycegc_aes` | AES-256-GCM encryption and decryption |
| `grycegc_smoke` | End-to-end smoke run of the main commands on a temporary project |
| `grycegc_crypto` | Crypto primitives and container formats |
| `grycegc_pak` | `.gpack` / `.pak` asset packs create / list / extract / verify |
| `grycegc_meta` | `gryce.meta.json` / `project.data` metadata read/write |
| `grycegc_package` | Release directory packaging and `verify` self-consistency |
| `grycegc_webpack` | Single-file HTML packaging output structure |
| `grycegc_new` | Project scaffolding: file list, `project.data` / scene JSON validity, cube OBJ vertices and normals, non-empty directory rejection and `--force` behaviour |

### 2.3 Scripting (`GryceScriptTests.exe`, 5 cases)

The tested sources (`engine/script/*` + `runtime/engine_context` + glog + `res:/` path resolution) are compiled directly into the test executable, which links no engine DLL. Script source files are written under the system temp directory (`temp/gryce_script_tests/<suite>`), created and cleaned up by each suite. Case design is described in [scripting.md](scripting.md).

| Case | Verifies |
| --- | --- |
| `script_value_convert` | C++ <-> JS value conversion: scalar round-trips, NaN / Infinity / out-of-range boundaries, Vector / Quaternion / Matrix4 (column-major) array forms, nested nlohmann::json round-trips, `engine.*` binding smoke |
| `script_lifecycle` | awake / start / update / destroy hook order and timing, props injected as `this.props`, frame clock (frame starts at 1), `_input` dispatch before update and non-consuming |
| `script_error_isolation` | Syntax-error load failure, execution stops after a runtime throw, infinite loops interrupted by the frame budget; component state flips correctly in all three cases without crashing |
| `script_hot_reload` | File changes trigger recompilation with props preserved across reloads; late-appearing files load on the poll interval; `reload_now` forced reload and `set_script_path` switching (command-surface core) |
| `script_app_entry` | Entry script (`main.js`) dispatch of `on_start` / `on_update` / `on_destroy`, hot-reload module swapping (mtime + size), and extension ABI `add_function` registration with scalar round-trips (see [app-entry.md](app-entry.md) / [extension.md](extension.md)) |

### 2.4 Asset sidecar files (`GryceAssetTests.exe`, 2 cases)

The sources under test (`engine/assets/asset_meta.cpp`, `engine/assets/import_options.cpp`, glog) are compiled straight into the test executable with no engine DLL; cases build and clean their own scratch project under the system temp directory and never write into the repository.

| Case | Coverage |
| --- | --- |
| `asset_meta` | Sidecar read/write round-trip, guid generation when missing (32 hex chars, no collision across 512 draws), defaults for missing fields, malformed JSON treated as absent, invalid settings falling back while keeping the guid, source-fingerprint staleness, no file written for a nonexistent source |
| `asset_import_options` | Scaling, Z-up / X-up conversion (positions, normals and tangents together), material switch, roughness multiplier clamping, invalid scale ignored (see [editor-api.md](editor-api.md) section 7) |

### 2.5 Editor API (`GryceEditorTests.exe`, 3 cases)

The only unit suite that **links `GryceCore.dll`**: command consumption, the undo stack and scene serialization all live inside core, so there is no pure-compute core to strip out. None of these paths touch GPU or window state (`GCore_Init` only builds a World and a scene), so CI still needs no graphics card.

Observations are made by comparing scene-JSON bytes rather than entity counts: `GEntity_GetCount` only counts entities that have a parent, so asserting on it yields false positives.

| Case | Coverage |
| --- | --- |
| `editor_edit` | All 10 payload structs fit in 256 bytes, invalid arguments rejected (return 0), create auto-selects, rename, and the "missing components keep current value" semantics of `SetTransform` (see [editor-api.md](editor-api.md) section 1) |
| `editor_undo` | Undo returns byte-identical previous state, redo restores exactly, `BeginEdit`/`EndEdit` folds 5 transform commands into 1 step, stack depth limit, `SetUndoLimit(0)` disables recording, loading another scene clears history |
| `editor_scene` | save → new scene → load → save produces byte-identical files (covers both "serializer drops a field" and "deserializer fails to restore"), loading a missing path returns -1 and leaves the scene usable |

### 2.6 UI op queue (`GryceUiTests.exe`, 2 cases)

The sources under test (`engine/api/ui_op_queue.cpp` + `engine/api/ui_api.cpp`) are compiled straight into the test executable with no engine DLL, and **without `GRYCE_HAS_IMGUI`** — which also verifies that the queue works in builds with ImGui disabled (headless hosts take that path too). `GRYCE_RENDERER_BUILDING` turns the export macros into dllexport so the static exe links against itself.

The `queue` suite covers the OpQueue itself: FIFO order and seq, in-place coalescing (position and seq untouched), same key with a different type does not merge, discrete ops never merge, a full queue evicts only the oldest coalescing op, a full queue of discrete ops rejects the push and counts it, drain budget, drain interval (the first drain is exempt, an empty drain does not consume the interval), `clear` keeps cumulative stats, the same key after a drain is a new entry, and bad arguments leave the queue untouched.

The `abi` suite covers the C layer: field round-trip with engine-assigned seq, coalescing and discrete ops through the ABI, policy get/set, interval throttling, `GUi_ClearOps`, stats queries tolerating nullptr, bad arguments rejected, and a request beyond capacity capped at capacity. The ABI acts on the process-wide singleton, so cases isolate leftover interval state by clearing and advancing a few frames.

Full queue semantics and policy are documented in [ui-api.md](ui-api.md) section 7.5.

## 3. Execution Notes

`grycegc_*` cases interfere with each other when run in parallel (shared temporary directories and process-level state), so run the full suite serially for local verification:

```bash
ctest --test-dir build -j1 --output-on-failure
```

On CI the cases are registered individually and scheduled by the runner, so this does not apply.

## 4. Adding Cases

- Math / resource-rule suites: add the name to `GRYCE_MATH_TEST_SUITES` in the root `CMakeLists.txt`; it is registered as a CTest case automatically.
- Asset / editor / UI suites: add the name to `GRYCE_ASSET_TEST_SUITES` / `GRYCE_EDITOR_TEST_SUITES` / `GRYCE_UI_TEST_SUITES` respectively. Editor cases must respect the two return conventions (enqueue returns 1/0, queries and scene operations return 0/-1); getting it wrong turns every assertion into a silent always-true pass.
- Toolchain suites: add the name to `_grycegc_suites` in `toolchain/CMakeLists.txt`.

## 5. History: Pixel-Comparison Cases Removed

The early `renderer_smoke_opengl` / `renderer_smoke_vulkan` pixel cases and their Python tools were removed: they judged by "the share of non-background pixels in a screenshot", which is heavily affected by headless environments, software rendering, and driver versions, and reflects neither build nor computational correctness. `GryceRenderSmoke.exe` remains as a manually run demo host; the web build likewise stays out of CTest and is verified manually (see [webgl-backend.md](webgl-backend.md) section 7).

## 6. CI Configuration

`.github/workflows/ci.yml`: triggered on any push / PR, Windows / MSYS2 UCRT64, Release configuration, installing GCC / CMake / Ninja / GLFW / GLEW / Box2D / Jolt system packages (no Assimp or Vulkan SDK; those paths degrade automatically), then configure + build + CTest.
