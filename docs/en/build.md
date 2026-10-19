# Build

This document covers dependency discovery, configuration options, and build artifacts. For the architecture overview see [architecture.md](architecture.md); for tests and CI see [testing.md](testing.md); for the web build see [webgl-backend.md](webgl-backend.md).

## 1. Quick Start

```bash
# 1. Prepare dependencies (any one option; missing ones are fetched from source as fallback)
#    MSYS2 UCRT64:
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-gcc-libs \
                    mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
                    mingw-w64-ucrt-x86_64-glfw mingw-w64-ucrt-x86_64-glew \
                    mingw-w64-ucrt-x86_64-box2d mingw-w64-ucrt-x86_64-jolt-physics \
                    mingw-w64-ucrt-x86_64-assimp
#    CLion bundled MinGW / vcpkg / custom prefix: no extra steps, CMake infers the prefix

# 2. Configure + build
cmake -S . -B build -G Ninja
cmake --build build --parallel

# 3. Run tests (all must pass)
ctest --test-dir build --output-on-failure

# 4. Run (interactive mode: WASD to move, mouse to look)
build/GryceRenderSmoke.exe --api opengl --play
```

## 2. Dependencies

| Dependency | Source | Notes |
| --- | --- | --- |
| GCC / gcc-libs | MSYS2 UCRT64 / CLion MinGW | C++26 support required |
| CMake >= 3.24, Ninja | MSYS2 UCRT64 / CLion MinGW | Build system |
| GLFW | System package or FetchContent | Window and input |
| GLEW | System package or FetchContent | OpenGL extension loading |
| Box2D | System package or FetchContent | 2D physics, imported target `box2d::box2d` |
| Jolt Physics | System package or FetchContent | 3D physics, imported target `Jolt::Jolt` |
| Assimp | Optional, system package or FetchContent | Model import (FBX / glTF / DAE / PLY / STL / 3DS / Blend; OBJ uses the built-in loader). Without it, non-OBJ formats are unavailable |
| Vulkan SDK | Optional | If missing, the Vulkan backend is disabled automatically (CTest does not depend on it) |
| Amplitude Audio SDK | Vendored in `third_party/amplitude` | Prebuilt static library, ships with the repository |
| stb / tinyexr / nlohmann_json / imgui / quickjs | Vendored in `third_party` | Images, EXR, JSON, editor UI, scripting |

### Three Dependency Source Strategies

CMake does not download dependencies on its own; `FetchContent` must be declared explicitly. This project controls it with `GRYCE_DEP_FETCH`:

| Value | Behavior |
| --- | --- |
| `AUTO` (default) | System packages first; for any package not found locally, only that one is built from source |
| `ON` | Ignore system packages, build everything from source (most reproducible, first configure downloads) |
| `OFF` | System packages only, fully offline (CI / controlled environments) |

Source builds pin fixed versions: GLFW `3.4`, GLEW `glew-2.2.0`, Box2D `v3.1.1`, Jolt Physics `v5.3.0`, Assimp `v5.4.3`. Tarballs are used instead of `git clone` (git's smart HTTP protocol is easily cut off through proxies; tarballs can be SHA256-pinned and need no local git). Downloads are cached in `build/_deps/tarballs` and are not re-fetched on reconfigure.

Source URLs try direct GitHub first, then fall back to mirrors (`ghfast.top` / `gh-proxy.com` / `gh.llkk.cc`); the probe result is written to the CMake cache. Override with `-DGRYCE_DEP_GIT_BASE=https://ghfast.top/https://github.com/`. On repeated failures, switch mirrors, use a local source tree (`-DFETCHCONTENT_SOURCE_DIR_JOLT=D:/deps/JoltPhysics`), or install system packages.

### Dependency Prefix Inference

No manual paths are needed; prefixes are inferred in this order:

1. `-DGRYCE_DEP_PREFIX=<prefix>` or the `GRYCE_DEP_PREFIX` environment variable (multiple paths separated by `;`)
2. `CMAKE_PREFIX_PATH` environment variable
3. `MSYS2_ROOT` + `MSYSTEM` (e.g. `D:/msys64` + `UCRT64`)
4. MSYS2's `MSYSTEM_PREFIX` / `MINGW_PREFIX`
5. Reverse inference from the C/C++ compiler path: `<root>/<msystem>/bin/g++.exe` -> `<root>/<msystem>` (this is how CLion's bundled MinGW is detected)

| Scenario | Action |
| --- | --- |
| MSYS2 on a non-default drive / non-UCRT64 | Configure from the matching MSYS2 terminal, or `-DGRYCE_DEP_PREFIX=D:/msys64/ucrt64` |
| CLion / JetBrains bundled MinGW | Nothing, the prefix is inferred from the compiler path |
| vcpkg | `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` |
| Custom prefix | `-DGRYCE_DEP_PREFIX=<prefix>` or `export GRYCE_DEP_PREFIX=<prefix>` |

The Vulkan SDK is optional: after installing the LunarG SDK, the `VULKAN_SDK` environment variable points to it; without it the Vulkan backend and its tests are disabled automatically.

## 3. Build Options

| Option | Default | Description |
| --- | --- | --- |
| `GRYCE_ENABLE_IMGUI` | `ON` | ImGui editor overlay |
| `BUILD_TESTING` | `ON` | CTest cases |
| `GRYCE_BUILD_TOOLCHAIN` | `ON` | GryceGC command-line toolchain (see [toolchain.md](toolchain.md)) |
| `GRYCE_ENABLE_D3D12` | `ON` | D3D12 backend; on Windows + GCC/Clang it probes for `d3d12.h` from the bundled MinGW or the Windows SDK, and skips silently if neither exists |
| `GRYCE_ENABLE_WEBGL` | `OFF` | WebGL (Emscripten) build; enabled automatically when an Emscripten toolchain is detected |
| `GRYCE_ENABLE_LTO` | `OFF` | Interprocedural optimization for non-Debug builds |
| `GRYCE_ENABLE_X86_V3` | `ON` | x86-64-v3 target ISA (AVX2 / FMA / BMI / F16C / LZCNT / POPCNT); use `OFF` for machines older than Haswell |
| `GRYCE_WEBGL_DEBUG` | `ON` | Keep abort assertions and exception information in web builds |

Other common variables: `GRYCE_DEP_FETCH`, `GRYCE_DEP_PREFIX`, `GRYCE_DEP_GIT_BASE` (see above).

## 4. Build Artifacts

| Target | Description |
| --- | --- |
| `GryceCore.dll` | Abstraction layer + scene / ECS / components / reflection / resources / render abstraction + physics and audio |
| `GryceRenderer.dll` | Render pipeline and OpenGL / Vulkan / D3D12 backends |
| `GrycePlatform.dll` | Window, input, cursor |
| `GryceRenderSmoke.exe` | Renderer harness (manual runs, not a CTest case) |
| `GryceApp.exe` | Application runtime (started with `--project <dir>`, entry `res:/main.js`); `GryceDemoExtension.dll` next to it is the extension ABI sample |
| `GryceMathTests.exe` | Math and resource-rule test host, built only with `BUILD_TESTING=ON` |
| `GryceGC.exe` | Toolchain executable, built only with `GRYCE_BUILD_TOOLCHAIN=ON` |

After a build, the `GryceSyncRuntimeDlls` target copies the engine DLLs, third-party runtime libraries (Box2D / Jolt / GLFW / GLEW), and the MinGW runtime (`libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll`) next to `GryceRenderSmoke.exe`, so it runs outside a MinGW / MSYS2 terminal. The engine DLLs additionally have their own POST_BUILD copy so that building a single engine library never leaves a stale DLL next to the exe.

The toolchain can also be built standalone (no Python needed):

```bash
cmake -S toolchain -B toolchain/build -G Ninja
```

## 5. CI

CI runs configure + build + CTest on Windows / MSYS2 UCRT64 in Release, with a single gate: all CTest cases must pass. Configuration details are in [testing.md](testing.md).
