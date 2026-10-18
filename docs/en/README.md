# Gryce Engine Documentation (English)

This directory contains the project documentation for Gryce Engine. The Chinese edition is at [docs/zh/](../zh/README.md).

Documents are organized by module and cover architecture, build, rendering, asset import, performance, and the toolchain. Historical design plans (the D3D12 and WebGL backend adoption plans) are archived under [docs/zh/plans/](../zh/plans/) and kept in Chinese only.

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
| Testing | [testing.md](testing.md) | CTest gate, test inventory, CI configuration |
