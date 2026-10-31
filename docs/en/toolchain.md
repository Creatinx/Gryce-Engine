# GryceGC Toolchain

`toolchain/` is the engine's command-line toolchain, codename **GryceGC** (C++23, self-contained, zero engine DLL dependencies): the engine sources it needs (`aes_gcm` / `gpack` / `pak` / `glog` / `crypto`) are compiled directly into the `grycegc_core` static library. It builds with the main project (`GRYCE_BUILD_TOOLCHAIN=ON` by default) and can also be configured standalone.

This document is a **user manual**: how to use every command, what its options are, and what it produces. For implementation details, the crypto container, and the binary layout of the pack format, see the toolchain suite inventory in `docs/en/testing.md` and the source comments.

---

## 1. Build

```bash
# With the main project (GRYCE_BUILD_TOOLCHAIN on by default)
cmake -S . -B build -G Ninja
cmake --build build --parallel

# Standalone (no Python, no engine required)
cmake -S toolchain -B toolchain/build -G Ninja
cmake --build toolchain/build --parallel
```

Build artifacts (present in both places):

| Path | Purpose |
| --- | --- |
| `build/toolchain/GryceGC.exe` | CLI built with the main project |
| `toolchain/build/GryceGC.exe` | CLI built standalone |
| `toolchain/build/GryceGCTests.exe` | Toolchain's own unit tests |

---

## 2. General Conventions

```bash
GryceGC <command> [options]
GryceGC help <command>     # full parameters for any command
GryceGC                   # no args = help: list all commands
GryceGC help              # same
```

- **Exit codes**: `0` on success; parameter errors / missing targets / failed verification generally return `1` or `2` (see each command). Scripts should branch on the exit code, not on stderr text.
- **Positionals**: bare arguments before options; most commands take only the first (e.g. `new <dir>`, `meta`'s subcommand).
- **Passthrough `--`**: commands that launch a host pass everything after `--` verbatim to the host process; GryceGC does not parse it.
- **Repeated options**: `--extra-dir`, `--target`, `-- <args>` etc. may repeat and accumulate.
- **Relative paths**: most `--build-dir` / `--package` default to conventional directories; absolute paths are safer.

---

## 3. Command Reference

### 3.1 `version`

Print the GryceGC version itself.

```bash
GryceGC version
```

---

### 3.2 `new` — Scaffold a Project

Generates a **self-contained** set of project files (no `main.cpp` of your own; the host is fixed and `main.js` decides behaviour):

```bash
GryceGC new MyGame                                 # directory name becomes the project name
GryceGC new MyGame --name "My Game" --api vulkan --extension
GryceGC new . --force                              # fill in place: overwrite listed files, keep others
```

| Option | Meaning | Default |
| --- | --- | --- |
| `--name <name>` | Display name (written into `project.data` and the window title) | directory name |
| `--width <px>` / `--height <px>` | Default window size | 1280 / 720 |
| `--api <api>` | Default backend `opengl` / `vulkan` / `dx12` | `opengl` |
| `--extension` | Also emit the `extension.cpp` native extension template | off |
| `--force` | Proceed even if the target directory is non-empty: overwrite only the listed files, keep the rest | off |

Output:

| File | Purpose |
| --- | --- |
| `main.js` | Entry script: create window, load scene, fly camera (WASD + mouse), Esc to quit |
| `project.data` | Project config: window size, default backend, render switches (HDR / tone map / shadow / ambient / IBL …) |
| `scenes/main.gesc` | Scene (`.gesc` v3 is plain JSON): camera / directional light / cube / ground |
| `models/cube.obj` | Procedurally generated unit cube (per-face vertices, correct flat normals, no repo assets) |
| `README.md` | Run and package commands |
| `extension.cpp` | Only with `--extension`: native extension template (wraps engine capabilities not yet exposed to scripts as `engine.*`) |

Notes:

- Shaders are **not** copied: when `res:/shaders` is missing the engine falls back to its built-in directory (both GL sources and Vulkan `.spv` resolve). Copy `engine/render/shaders` into the project only when customising.
- A non-empty target directory is rejected by default; `--force` overwrites only the generated files and leaves everything else alone — handy for "filling in a skeleton inside an existing directory".
- Afterwards `build/GryceApp.exe --project <dir>` runs it (build `GryceApp` first).
- The scene is plain JSON: edit by hand or save from the editor; entity fields are in `docs/en/app-entry.md`.

> Backend consistency: this command's `--api` accepts `dx12`, but the `package` / `run` convenience launcher flags only expose `opengl` / `vulkan`. The actual runtime backend is decided by `project.data`'s `render_api` field, so a `--api dx12` project runs via `project.data`; `package` / `run`'s `--api` only adds an override to the launcher.

---

### 3.3 `deps` — DLL Dependency Closure

List the full DLL dependency closure of an executable, categorised as **core (engine DLLs) / dependency (third-party)**, to diagnose "why is this package missing a DLL on that machine".

```bash
GryceGC deps --build-dir build
GryceGC deps --exe build/GryceApp.exe --extra-dir third_party/some/bin
```

| Option | Meaning | Default |
| --- | --- | --- |
| `--build-dir <dir>` | CMake build dir; auto-detect exe inside | `cmake-build-debug` |
| `--exe <path>` | Specify the exe directly (path or a file name inside the build dir) | auto |
| `--extra-dir <dir>` | Extra DLL search directory (repeatable) | none |

---

### 3.4 `verify` — Release Package Self-Consistency

Verify that every imported DLL inside a release directory resolves within the package (core should sit beside the exe, the rest under `dll/`). Runs automatically with `package --verify`, or re-check an existing package standalone.

```bash
GryceGC verify --package dist/GryceEngine
GryceGC verify --package dist/GryceEngine --dll-folder libs
```

| Option | Meaning | Default |
| --- | --- | --- |
| `--package <dir>` | Release package directory (required) | — |
| `--dll-folder <name>` | Subdirectory name holding dependency DLLs | `dll` |

---

### 3.5 `package` — Package a Release Directory

Bundle build artifacts + core DLL + dependency DLLs + the asset project + launcher + README into an **offline-runnable** directory (ported from the old `toolchain/main.py`).

```bash
GryceGC package --build-dir build --project . --name MyGame --verify
GryceGC package --build-dir build --no-assets --clean --output out
```

| Option | Meaning | Default |
| --- | --- | --- |
| `--build-dir <dir>` | CMake build dir | `cmake-build-debug` |
| `--exe <path>` | Specify exe (default auto-detect `GryceRenderSmoke.exe`) | auto |
| `--output <dir>` | Output root directory | `dist` |
| `--name <name>` | Release folder name | `GryceEngine` |
| `--project <dir>` | Asset project root (copied whole into the package) | `test/demo_project` |
| `--dll-folder <n>` | Dependency DLL folder name | `dll` |
| `--api <api>` | Launcher default backend `opengl` / `vulkan` | `opengl` |
| `--extra-dir <dir>` | Extra DLL search directory (repeatable) | none |
| `--no-assets` | Do not package the demo_project assets | off |
| `--clean` | Empty the output directory before packaging | off |
| `--verify` | Run `verify` immediately after packaging | off |

> Constraint: `--api` only accepts `opengl` / `vulkan` (see the backend-consistency note in 3.2).

---

### 3.6 `run` — Cross-Platform Launcher

Inject `dll/` at the front of `PATH` then launch the host, replacing the pile of `run*.bat`. Works from a release package or a build directory.

```bash
GryceGC run --package dist/GryceEngine --play
GryceGC run --build-dir build --api vulkan -- --frames 120 --shot out.png
```

| Option | Meaning | Default |
| --- | --- | --- |
| `--package <dir>` | Release package directory (the one with the launcher) | — |
| `--build-dir <dir>` | Build directory (launch build artifacts directly) | — |
| `--api <api>` | Render backend `opengl` / `vulkan` | `opengl` |
| `--play` | Interactive mode (WASD + mouse) | off |
| `--` | Everything after is passed verbatim to the host | — |

The exit code propagates the host's exit code unchanged.

---

### 3.7 `copyright` — Copyright / License Stamping

Stamp copyright onto a release: write `COPYRIGHT.txt`, append a copyright footer to the README, and write a PE trailer into the exe (optionally also `VERSIONINFO`).

```bash
GryceGC copyright --package dist/GryceEngine --holder "Your Name" --license AGPL-3.0-or-later
GryceGC copyright --exe build/GryceApp.exe --holder "You" --version-info \
    --product "My Game" --version "1.2.0"
```

| Option | Meaning | Default |
| --- | --- | --- |
| `--package <dir>` / `--exe <file>` | Operate on a whole package directory, or a single PE file (one required) | — |
| `--holder <name>` | Copyright holder (required) | — |
| `--year <yyyy>` | Year | `2026` |
| `--license <id>` | SPDX or custom license id (e.g. `MIT`) | none |
| `--license-file <f>` | Full license text file, appended to `COPYRIGHT.txt` | none |
| `--version-info` | Also write a PE `VERSIONINFO` resource (Windows only) | off |
| `--product <name>` | `VERSIONINFO` ProductName | none |
| `--version <ver>` | `VERSIONINFO` ProductVersion | none |

---

### 3.8 `meta` — Metadata Read/Write

Read/write three kinds of metadata: `gryce.meta.json` (project meta), `project.data` (project config), PE trailer (version stamp embedded in the exe).

```bash
GryceGC meta set   --target ./MyGame --project --key engine_version --value ">=0.2.0"
GryceGC meta get   --target ./MyGame --project --key name
GryceGC meta show  --target ./MyGame
GryceGC meta stamp-git --target ./MyGame
```

Subcommands:

| Subcommand | Purpose |
| --- | --- |
| `set` | Write a key/value (`--key` + `--value` required) |
| `get` | Read and print a key (`--key` required) |
| `show` | Print all readable metadata on the target |
| `stamp-git` | Write the git HEAD short hash and commit date |

| Option | Meaning | Default |
| --- | --- | --- |
| `--key <k>` | Key name (required for set/get) | — |
| `--value <v>` | Value (required for set) | — |
| `--target <path>` | Directory / PE file | current directory |
| `--project` | Treat target as `<target>/project.data` | off |

---

### 3.9 `encrypt` / `decrypt` — Encryption

Symmetric encryption supporting **AES-256-GCM** and **ChaCha20**, byte-compatible with the engine's `aes_gcm_buf`, usable to encrypt any asset beyond `.pak`.

```bash
GryceGC keygen --bytes 32 --hex --out key.hex
GryceGC encrypt --in a.bin --out a.enc --key @key.hex --algo aes-gcm --aad "hdr"
GryceGC decrypt --in a.enc --out a.bin --key @key.hex
```

`encrypt` options:

| Option | Meaning | Default |
| --- | --- | --- |
| `--in <f>` / `--out <f>` | Input / output file (required) | — |
| `--key <spec>` | Key: **64 hex chars** / `@file` (read file) / `env:VAR` / `prompt:text` | — |
| `--algo <a>` | `aes-gcm` (default) / `chacha20` | `aes-gcm` |
| `--aad <text>` | Additional authenticated data (not encrypted, but integrity-checked) | empty |
| `--raw` | Skip the `GRYCENC1` container header, emit raw `iv||ct||tag` (engine raw-format compatible) | off |

`decrypt` auto-detects the `GRYCENC1` container; without it, it falls back to `--algo` raw format (also accepts `--aad` then).

`--key` four forms:

- `0011…ff` (64 hex chars): used directly as a 32-byte key.
- `@key.bin`: read raw bytes from the file as the key.
- `env:MY_KEY`: read from the environment variable.
- `prompt:Enter key`: prompt interactively (no plaintext left in logs/history).

---

### 3.10 `keygen` — Generate Random Keys

Generate cryptographically secure random keys.

```bash
GryceGC keygen                  # 32 bytes by default, printed raw/base64
GryceGC keygen --bytes 16 --hex --out key.hex
```

| Option | Meaning | Default |
| --- | --- | --- |
| `--bytes <N>` | Key length in bytes | `32` |
| `--hex` | Output in hex (handy for `--key @file` reuse) | off |
| `--out <file>` | Write to a file (otherwise stdout) | stdout |

---

### 3.11 `pak` — Asset Pack Management

Create / list / extract / verify asset packs. Two formats:

- `.gpack` (container `GPAK`, v1): unencrypted pack.
- `.pak` (v3 + v4): v4 supports AES encryption (`--key hex`).

```bash
GryceGC pak create  --out game.gpack  assets/ models/ logo.png
GryceGC pak create  --out game.pak --key <hex> assets/        # v4 encrypted
GryceGC pak list    --file game.pak
GryceGC pak extract --file game.pak --out extracted/
GryceGC pak verify  --file game.pak                            # CRC32 check
```

| Subcommand | Options |
| --- | --- |
| `create` | `--out <f>` + one or more input files/dirs; `--key <hex>` (`.pak` v4 encryption only) |
| `list` | `--file <f>`; `--key <hex>` (encrypted packs need it) |
| `extract` | `--file <f>` + `--out <dir>`; `--key <hex>` |
| `verify` | `--file <f>`; `--key <hex>` (CRC32 integrity check) |

---

### 3.12 `debug` — Debug Launcher

Launch the host with a set of preset debug environment variables (D3D12 debug layer / trace / probe / glog) instead of typing a long `set GRYCE_…`.

```bash
GryceGC debug --build-dir build --d3d12 --gpu-validation
GryceGC debug --package dist/GryceEngine --trace-frame 10 --probe-ssr --with-debugger windbg
```

| Option | Environment variable set |
| --- | --- |
| `--d3d12` | `GRYCE_D3D12_DEBUG=1` |
| `--gpu-validation` | `GRYCE_D3D12_GPU_VALIDATION=1` |
| `--serialize` | `GRYCE_D3D12_SERIALIZE=1` |
| `--trace-hdr` | `GRYCE_TRACE_HDR=1` |
| `--trace-frame <N>` | `GRYCE_DX12_TRACE_FRAME=N` |
| `--trace-bind` | `GRYCE_DX12_TRACE_BIND=1` |
| `--probe-ssr` | `GRYCE_DX12_PROBE_SSR=1` |
| `--probe-frame <N>` | `GRYCE_DX12_PROBE_FRAME=N` |
| `--probe-cbv <NAME>` | `GRYCE_DX12_PROBE_CBV=NAME` |
| `--ring` | `GRYCE_DX12_PROBE_RING=1` |
| `--glog <N>` | `GLOG_minloglevel=N` |
| `--with-debugger <D>` | `cdb` / `windbg` / `x64dbg` (attach a debugger after launch) |
| `--api <api>` | `opengl` / `vulkan` / `dx12` |

---

### 3.13 `bench` — Frame-Time A/B Benchmark

Run several frames, measure average/min frame time, and print a single-line KV (ported from `bench_ab.sh`) — ideal for "before vs after a change" comparisons.

```bash
GryceGC bench --build-dir build --w 1280 --h 720 --runs 5 --api opengl
```

| Option | Meaning | Default |
| --- | --- | --- |
| `--build-dir <dir>` | Build directory (required) | — |
| `--w <N>` / `--h <N>` | Render resolution | — |
| `--runs <N>` | Repeat rounds | `3` |
| `--api <api>` | Backend | `opengl` |

> Tip: a single-round benchmark is not trustworthy; at least 3–4 alternating rounds and take the minimum.

---

### 3.14 `font` — Regenerate Font Data

Bake a TTF into `engine/render/imgui_font_data.h` (the built-in font for imgui / the built-in UI, avoiding a runtime file read).

```bash
GryceGC font                                   # default Cousine source
GryceGC font path/to/MyFont.ttf --out engine/render/imgui_font_data.h
```

| Option | Meaning | Default |
| --- | --- | --- |
| positional | Source `.ttf` | `third_party/imgui/misc/fonts/Cousine-Regular.ttf` |
| `--out <file>` | Output header path | `engine/render/imgui_font_data.h` |

---

### 3.15 `build` — cmake Wrapper

Wrap `cmake configure + build`, reusing the main project's toolchain (avoids hand-typing a long list of `-D` switches).

```bash
GryceGC build --preset release --d3d12 on --lto --target GryceApp --jobs 8
GryceGC build --clean --test
```

| Option | Meaning | Default |
| --- | --- | --- |
| `--build-dir <dir>` | Build directory | `build` |
| `--preset <p>` | `debug` (default) / `release` | `debug` |
| `--target <name>` | Build target (repeatable) | all |
| `--d3d12 <on|off>` | D3D12 backend switch | `on` |
| `--lto` | Enable LTO (link-time optimization) | off |
| `--test` | Run CTest after building | off |
| `--clean` | Empty the directory before building | off |
| `--jobs <N>` | Parallelism | auto |

---

### 3.16 `webpack` — Single-File Web Packaging

Merge an Emscripten multi-file web build (`.html` + `.js` + `.wasm` + `.data`) into one **self-contained** `.single.html` that runs from `file://` with zero external network requests.

```bash
GryceGC webpack --build-dir build-webgl --name GryceRenderSmoke
# Output: build-webgl/GryceRenderSmoke.single.html (zero external references)
```

Principle: inline Emscripten's `.js` into the HTML shell, then eliminate runtime `fetch` via two public runtime hooks — `Module.locateFile` (returns a base64 data URL for `.wasm`) and `Module.getPreloadedPackage` (hands `.data` to the file packager directly as an ArrayBuffer). It is a **post-link** step: it does not touch the build system and does not re-link. Render output is byte-identical to the multi-file HTTP build. Detailed comparison and constraints are in [webgl-backend.md](webgl-backend.md) section 1.

| Option | Meaning | Default |
| --- | --- | --- |
| `--build-dir <dir>` | Build directory | `build-webgl` |
| `--name <name>` | App base name / Emscripten output prefix | `GryceRenderSmoke` |
| `--html <file>` | Entry html | `<build-dir>/<name>.html` |
| `--js <file>` | Emscripten JS | `<build-dir>/<name>.js` |
| `--wasm <file>` | wasm | `<build-dir>/<name>.wasm` |
| `--data <file>` | file packager data (may be absent) | `<build-dir>/<name>.data` |
| `--output <file>` | Output single-file html | `<build-dir>/<name>.single.html` |

---

## 4. Typical Workflow

```bash
# 1. Scaffold a project
GryceGC new MyGame --name "My Game" --api vulkan --extension
cd MyGame

# 2. Build the host (GryceApp must be built first)
cmake --build ../build --target GryceApp
../build/GryceApp.exe --project . --frames 60 --shot shot.png

# 3. Package a release directory and self-verify
GryceGC package --build-dir ../build --project . --name MyGame --verify

# 4. Stamp copyright
GryceGC copyright --package dist/MyGame --holder "Your Name" --license AGPL-3.0-or-later

# 5. Run offline (interactive)
GryceGC run --package dist/MyGame --play

# 6. For a web single file
GryceGC webpack --build-dir build-webgl --name MyGame
```

Encrypting assets (sensitive in-game assets):

```bash
GryceGC keygen --bytes 32 --hex --out key.hex
GryceGC pak create --out game.pak --key @key.hex assets/      # v4 encrypted pack
GryceGC pak verify --file game.pak --key @key.hex             # verify
```

---

## 5. Testing

The `grycegc_*` CTest cases cover the command surface and the correctness of crypto and packaging; the inventory is in [testing.md](testing.md).

```bash
# grycegc_* cases interfere when run in parallel (shared temp dirs and process state); run serially locally
ctest -R grycegc -j1 --output-on-failure
```

The `GryceGCTests.exe` host can also run a single suite directly:

```bash
toolchain/build/GryceGCTests.exe --suite grycegc_new
```
