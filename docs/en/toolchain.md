# GryceGC Toolchain

`toolchain/` is the engine's command-line toolchain (C++23, self-contained, zero engine DLL dependencies): the needed engine sources (aes_gcm / gpack / pak / glog / crypto) are compiled directly into the `grycegc_core` static library. It builds with the main project (`GRYCE_BUILD_TOOLCHAIN=ON` by default) and can also be configured standalone.

## 1. Build

```bash
# With the main project (default)
cmake -S . -B build -G Ninja && cmake --build build --parallel

# Standalone (no Python needed)
cmake -S toolchain -B toolchain/build -G Ninja
```

## 2. Command Surface

Run `GryceGC help <command>` for full parameters.

| Command | Purpose |
| --- | --- |
| `version` | Print the GryceGC version |
| `help` | Show help (running GryceGC with no arguments is equivalent to `help`) |
| `new` | Scaffold a project (`main.js` + `project.data` + scene + cube model) |
| `deps` | List the full DLL dependency closure of an exe (categorized as core / dependency) |
| `verify` | Verify release package self-consistency (all imported DLLs resolvable inside the package) |
| `package` | Package an offline-runnable release directory (exe + core DLL side by side + `dll/` + `demo_project` + launcher + README) |
| `run` | Cross-platform launcher: injects `dll/` into PATH then starts the host (replaces `run*.bat`) |
| `copyright` | Copyright stamping: COPYRIGHT.txt + README footer + PE trailer (optional VERSIONINFO) |
| `meta` | Metadata read/write: `gryce.meta.json` / `project.data` / PE trailer / git stamp |
| `encrypt` / `decrypt` | AES-256-GCM / ChaCha20 encryption (byte-compatible with the engine's `aes_gcm_buf`) |
| `keygen` | Generate cryptographically secure random keys (32 bytes by default) |
| `pak` | Asset packs create / list / extract / verify (`.gpack` v1 / `.pak` v3+v4 encrypted) |
| `debug` | Launch the host with preset debug environment variables (D3D12 debug layer / trace / probe / glog) |
| `bench` | Frame-time A/B benchmark (single-line KV output) |
| `font` | Regenerate `engine/render/imgui_font_data.h` |
| `build` | cmake configure + build wrapper |
| `webpack` | Pack a multi-file Emscripten web build into one self-contained HTML file |

## 3. Creating a Project (`new`)

```bash
GryceGC new MyGame                        # directory name becomes the project name
GryceGC new MyGame --name "My Game" --api vulkan --extension
GryceGC new . --force                     # fill in place: overwrite listed files, keep others
```

Output:

| File | Purpose |
| --- | --- |
| `main.js` | Entry script: create window, load scene, fly camera, Esc to quit |
| `project.data` | Project config: window size, default backend, render switches |
| `scenes/main.gesc` | Scene (`.gesc` v2 is plain JSON): camera / directional light / cube / ground |
| `models/cube.obj` | Procedurally generated unit cube; no repository assets required |
| `README.md` | Run and package commands |
| `extension.cpp` | Only with `--extension`: native extension template |

Notes:

- The project needs no `main.cpp` of its own; the host is fixed and `main.js` decides behaviour (see [app-entry.md](app-entry.md)).
- Shaders are not copied: when `res:/shaders` is missing the engine falls back to its built-in directory (both GL sources and Vulkan `.spv` resolve). Copy `engine/render/shaders` into the project only when customising.
- A non-empty target directory is rejected by default; `--force` overwrites only the generated files and leaves everything else alone.
- Afterwards `build/GryceApp.exe --project <dir>` runs it (build `GryceApp` first).

## 4. Typical Release Workflow

```bash
GryceGC package --build-dir build --verify
GryceGC copyright --package dist/GryceEngine --holder "Your Name" --license AGPL-3.0-or-later
GryceGC run --package dist/GryceEngine -- --frames 120 --shot out.png
```

## 5. Single-File Web Distribution (`webpack`)

CMake's web build only produces multiple files (`.html` + `.js` + `.wasm` + `.data`). For the "one html, double-click to run" form, use the packager as a post-link step; it does not touch the build system and does not re-link:

```bash
toolchain/build/GryceGC.exe webpack --build-dir build-webgl --name GryceRenderSmoke
# Output: build-webgl/GryceRenderSmoke.single.html (~5.1MB, zero external references)
```

Implementation (`toolchain/src/core/web_pack.cpp`): inline Emscripten's `.js` into the html shell, then eliminate remaining network requests via two public runtime hooks: `Module.locateFile` (returns a base64 data URL for `.wasm`) and `Module.getPreloadedPackage` (hands `.data` to the file packager directly as an ArrayBuffer). Render output is byte-identical to the multi-file HTTP build. Detailed comparison and constraints are in [webgl-backend.md](webgl-backend.md) section 1.

## 6. Testing

The `grycegc_*` CTest cases cover the command surface and the correctness of crypto and packaging; the inventory is in [testing.md](testing.md).
