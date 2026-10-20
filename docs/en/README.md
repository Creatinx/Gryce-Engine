# Gryce Engine Documentation (English)

This directory contains the project documentation for Gryce Engine. The Chinese edition is at [docs/zh/](../zh/README.md).

Documents are organized by module and cover architecture, build, rendering, asset import, performance, and the toolchain. Historical design plans (the D3D12, WebGL, and QuickJS scripting adoption plans) are archived under [docs/zh/plans/](../zh/plans/) and kept in Chinese only.

| Module | Document | Contents |
| --- | --- | --- |
| Overview | [architecture.md](architecture.md) | DLL layering, C API boundary, ECS and reflection, scene serialization, resource system, runtime model |
| Build | [build.md](build.md) | Dependency discovery and source strategies, build options, artifacts, DLL sync, CI |
| Rendering | [renderer.md](renderer.md) | Four backends, clustered forward pipeline, shadows, screen-space and environment effects, debug tooling |
| Shaders | [shader-conventions.md](shader-conventions.md) | Source layout, backend variants, macro rules, ESSL300 conversion constraints |
| Asset import | [model-import.md](model-import.md) | Texture resolution, PBR material derivation, surface types, color space |
| LOD | [lod.md](lod.md) | Error-driven automatic LOD and chunked LOD |
| Performance | [performance.md](performance.md) | Measurement method, time breakdown, completed optimizations |
| WebGL | [webgl-backend.md](webgl-backend.md) | Web build, shim architecture, platform differences, troubleshooting |
| Toolchain | [toolchain.md](toolchain.md) | GryceGC command surface and release workflow |
| Scripting | [scripting.md](scripting.md) | QuickJS script component: serialized fields, lifecycle hooks, base engine.* API, value conventions, sandbox, hot reload |
| Application entry | [app-entry.md](app-entry.md) | `GryceApp` host + `main.js` entry: startup order, hooks, window/scene/entity/app API, command line |
| Native extensions | [extension.md](extension.md) | Complete `extension.cpp` ABI reference: loading, per-field virtual table semantics, value channel, build and distribution, troubleshooting |
| Editor API | [editor-api.md](editor-api.md) | Edit commands, undo/redo, viewport output and the editor camera, dual viewports, gizmo and screen origin, display modes, asset sidecars and directory walking |
| Built-in UI | [ui-api.md](ui-api.md) | The `GUi_*` surface: ImGui ownership across the DLL boundary, the two gates, the forbidden-call list and intent queue, viewport rect and coordinates, undo grouping, input yielding |
| Testing | [testing.md](testing.md) | CTest gate, test inventory, CI configuration |
