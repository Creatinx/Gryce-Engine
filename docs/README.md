# Gryce Engine Documentation

Gryce Engine is a C++26 game engine for Windows, built around a reflection-driven ECS and a renderer with four backends (OpenGL / Vulkan / D3D12 / WebGL). The main line of development is the renderer: scene serialization, the resource system, 2D/3D physics, audio, and animation are in place, with two hosts: `GryceRenderSmoke` (renderer harness/demo) and `GryceApp` (runtime whose project entry is `main.js`).

- Chinese documentation: [docs/zh/](zh/README.md)
- English documentation: [docs/en/](en/README.md)

## Documentation Map

| Module | Chinese | English |
| --- | --- | --- |
| Overview | [zh/architecture.md](zh/architecture.md) | [en/architecture.md](en/architecture.md) |
| Build | [zh/build.md](zh/build.md) | [en/build.md](en/build.md) |
| Rendering | [zh/renderer.md](zh/renderer.md) | [en/renderer.md](en/renderer.md) |
| Shaders | [zh/shader-conventions.md](zh/shader-conventions.md) | [en/shader-conventions.md](en/shader-conventions.md) |
| Asset import | [zh/model-import.md](zh/model-import.md) | [en/model-import.md](en/model-import.md) |
| LOD | [zh/lod.md](zh/lod.md) | [en/lod.md](en/lod.md) |
| Performance | [zh/performance.md](zh/performance.md) | [en/performance.md](en/performance.md) |
| WebGL | [zh/webgl-backend.md](zh/webgl-backend.md) | [en/webgl-backend.md](en/webgl-backend.md) |
| Toolchain | [zh/toolchain.md](zh/toolchain.md) | [en/toolchain.md](en/toolchain.md) |
| Scripting | [zh/scripting.md](zh/scripting.md) | [en/scripting.md](en/scripting.md) |
| Application entry | [zh/app-entry.md](zh/app-entry.md) | [en/app-entry.md](en/app-entry.md) |
| Native extensions | [zh/extension.md](zh/extension.md) | [en/extension.md](en/extension.md) |
| Editor API | [zh/editor-api.md](zh/editor-api.md) | [en/editor-api.md](en/editor-api.md) |
| Built-in UI | [zh/ui-api.md](zh/ui-api.md) | [en/ui-api.md](en/ui-api.md) |
| Testing | [zh/testing.md](zh/testing.md) | [en/testing.md](en/testing.md) |

Historical design plans (the D3D12, WebGL, and QuickJS scripting adoption plans) are archived under [zh/plans/](zh/plans/) and kept in Chinese only.

## Quick Start

```bash
# Configure + build (MSYS2 UCRT64, CLion MinGW, vcpkg, or a custom prefix;
# missing dependencies are fetched from source automatically)
cmake -S . -B build -G Ninja
cmake --build build --parallel

# Tests (all must pass)
ctest --test-dir build --output-on-failure

# Run the demo (interactive: WASD to move, mouse to look)
build/GryceRenderSmoke.exe --api opengl --play
```

See [en/build.md](en/build.md) for dependency strategies and options, [en/renderer.md](en/renderer.md) for pipeline and effects, [en/testing.md](en/testing.md) for the test gate and CI.

## License

The project is licensed under the GNU Affero General Public License v3.0 (SPDX: `AGPL-3.0-or-later`); see [LICENSE](../LICENSE). Third-party libraries under `third_party/` keep their original licenses.
