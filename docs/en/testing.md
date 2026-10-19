# Testing

This document describes the CTest gate, the test inventory, and CI configuration.

## 1. Gate Status

CI has exactly one pass condition: every case in `ctest` must pass. If any case fails, `ctest` returns non-zero and the job fails (there is no `continue-on-error` and no skipping in CI). Configure uses `--no-tests=error`: if a CMake change ever results in zero registered cases, ctest exits with an error instead of falsely passing with "zero cases, zero failures".

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## 2. Test Inventory

There are currently 23 cases across three hosts.

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

## 3. Execution Notes

`grycegc_*` cases interfere with each other when run in parallel (shared temporary directories and process-level state), so run the full suite serially for local verification:

```bash
ctest --test-dir build -j1 --output-on-failure
```

On CI the cases are registered individually and scheduled by the runner, so this does not apply.

## 4. Adding Cases

- Math / resource-rule suites: add the name to `GRYCE_MATH_TEST_SUITES` in the root `CMakeLists.txt`; it is registered as a CTest case automatically.
- Toolchain suites: add the name to `_grycegc_suites` in `toolchain/CMakeLists.txt`.

## 5. History: Pixel-Comparison Cases Removed

The early `renderer_smoke_opengl` / `renderer_smoke_vulkan` pixel cases and their Python tools were removed: they judged by "the share of non-background pixels in a screenshot", which is heavily affected by headless environments, software rendering, and driver versions, and reflects neither build nor computational correctness. `GryceRenderSmoke.exe` remains as a manually run demo host; the web build likewise stays out of CTest and is verified manually (see [webgl-backend.md](webgl-backend.md) section 7).

## 6. CI Configuration

`.github/workflows/ci.yml`: triggered on any push / PR, Windows / MSYS2 UCRT64, Release configuration, installing GCC / CMake / Ninja / GLFW / GLEW / Box2D / Jolt system packages (no Assimp or Vulkan SDK; those paths degrade automatically), then configure + build + CTest.
