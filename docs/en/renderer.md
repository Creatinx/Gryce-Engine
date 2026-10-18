# Renderer

This document describes the renderer's backends, pipeline structure, and adjustable effects. Build options are in [build.md](build.md); shader constraints are in [shader-conventions.md](shader-conventions.md); web specifics are in [webgl-backend.md](webgl-backend.md).

## 1. Backends

Four backends share the upper pipeline and shader sources, selected with `--api`:

| Backend | Value | Loading |
| --- | --- | --- |
| OpenGL | `opengl` | Link-time dependency on GLEW, driver loaded at runtime |
| Vulkan | `vulkan` | Loads `vulkan-1.dll` and the function chain at runtime; no import library binding. Disabled when the Vulkan SDK is absent |
| D3D12 | `dx12` | `LoadLibrary` for `d3d12.dll` / `dxgi.dll` / `dxcompiler.dll` at runtime; no link-time import libraries |
| WebGL | `webgl` | Emscripten builds only, reuses the GL backend source (see [webgl-backend.md](webgl-backend.md)) |

Backends plug in through the `IRenderBackend` interface in `render/render.h`. Missing capabilities degrade declaratively via `RenderBackendCapabilities`: without compute shaders the engine falls back to CPU culling (`--no-gpu-cull` is the fallback entry point); without indirect draw, submission is done on the CPU.

## 2. Forward Pipeline

- Clustered forward: lights are assigned to 3D clusters (`render/renderer_rd/cluster_builder_rd.cpp`).
- HDR render targets, tone mapping, auto exposure.
- A normal prepass (`prepass_normal`) redraws geometry for SSAO / SSR and follows the current LOD level.
- GPU culling (frustum / occlusion) and a CPU fallback path coexist.

## 3. Shadows

- Shadow atlas + cascaded shadow maps (CSM).
- Filtering: VSM / ESM, PCSS soft shadows, point-light shadows, contact shadows.
- PCSS automatically falls back to PCF under degenerate parameters and logs the reason; criteria and measurements are in [performance.md](performance.md).
- The shadow bias formula (`slope_bias`) and the requirement to keep seven receiver shaders in sync are in [shader-conventions.md](shader-conventions.md).

## 4. Screen-Space Effects

SSR (reflections), SSAO, SSIL, toggled individually with `--ssr` / `--no-ssr`, `--ssao` / `--no-ssao`, `--ssil` / `--no-ssil`.

## 5. Global Illumination and Environment

SDFGI, VoxelGI, reflection probes, IBL (prefiltered environment maps + BRDF LUT), volumetric fog. The IBL specular convention (`F0*A + B`; `fresnel(NdotV)*A + B` is forbidden) is documented in [model-import.md](model-import.md) section 6.1.

## 6. Post-Processing

Bloom, TAA, motion blur, bokeh DOF, FSR2, subsurface scattering, water.

## 7. 2D Pipeline

Sprites, tilemaps, parallax backgrounds, 2D particles, 2D skyboxes, text. Entry points are `render/render2d.cpp` and `render/renderer2d_impl.cpp`.

## 8. 3D Extensions

3D particles, line rendering, trails, instanced meshes, billboards, 3D text, LOD (see [lod.md](lod.md)), volumetric light. Skinned meshes are driven by `components/skinned_mesh_renderer.h`.

## 9. Debug Tooling

- Top-left overlay: FPS / frame time / limiter status (with a frame-time graph).
- ImGui debug window (F1 to toggle): vsync, frame rate cap, power-saver mode, GPU timing, and render toggles for shadows / GPU culling / SSR / IBL.
- Debug views: `--debug-view N` (alias `--debug`), where N is 0 final composite, 1 albedo, 2 normal, 3 roughness, 4 metallic, 5 shadow factor, 6 direct light, 7 ambient / IBL, 8 cascades.
- `--list` prints scene entities, cameras, lights, and mesh statistics.
- `--bench N` produces segmented GPU timings plus CPU phases; the method and data are in [performance.md](performance.md).
