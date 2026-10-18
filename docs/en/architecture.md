# Engine Architecture

This document describes the overall structure of Gryce Engine: binary layering, module responsibilities, and the boundaries between them. For renderer internals see [renderer.md](renderer.md); for build setup see [build.md](build.md).

## 1. Binary Layering

The engine is organized as three dynamic libraries plus a demo host, all under the `gryce_engine` namespace:

| Target | Contents |
| --- | --- |
| `GryceCore.dll` | Abstraction layer, scene / ECS / components / reflection / resources / render abstraction, physics and audio |
| `GryceRenderer.dll` | Render pipeline and backends (OpenGL / Vulkan / D3D12) |
| `GrycePlatform.dll` | Window, input, cursor |
| `GryceRenderSmoke.exe` | Demo host, run manually, not part of CTest |

Engine capabilities are exposed through the C API in `engine/api/`, consumed as a DLL. The API is split by domain: `core` / `entity` / `scene` / `component` / `render` / `material` / `asset` / `animator` / `input` / `platform` / `window` / `viewport` / `script`. Handles are managed via `runtime/entity_handle_map.h`; only stable handles and POD cross the boundary.

## 2. Module Responsibilities

Directory layout under `engine/`:

| Directory | Responsibility |
| --- | --- |
| `api/` | C API boundary, one file pair per domain |
| `math/` | Vectors, matrices, quaternions, cameras, rays (right-handed) |
| `scene/` | Entities, scenes, prefabs, `.gesc` serialization, UUID |
| `ecs/` | `World`, `ComponentStore`, and systems |
| `components/` | Component definitions, grouped as 2d / 3d / common |
| `reflection/` | Reflection registration and built-in type reflection |
| `render/` | Render abstraction, pipeline implementation, backends |
| `assets/` | Model / image loading, material derivation, LOD, vertex optimization, async loading |
| `resources/` | Project configuration, `res:/` paths, Pak / Gpack, AES-GCM |
| `animation/` | Skeletons, animation clips, pose blending |
| `platform/` | Window, input, cursor |
| `runtime/` | Engine context, entity handle map, command buffer |
| `utils/` | Logging (glog), frame limiter, crypto primitives |

## 3. ECS and Reflection

- `ecs/world.h` holds entities and component storage; `ecs/component_store.h` stores components densely per type.
- Systems live in `ecs/systems/`: `animator` / `audio` / `hierarchy` / `physics_2d` / `physics_3d` / `render_2d` / `render_3d` / `subviewport`. Play mode drives these systems; exiting restores the edit-state snapshot.
- Components are registered through `reflection/`, so they can be accessed uniformly by serialization, editors, and scripts. Component creation goes through `components/component_factory.h`.

## 4. Scenes and Serialization

Scenes are JSON text (`.gesc`, currently version 2). An entity consists of `name` / `uuid` / `parent` / `enabled` / `transform` / `components`. The serializer is `scene/scene_serializer.cpp`; prefabs are `scene/prefab.cpp`.

When a multi-material model is imported, `scene/multi_material_assembly.cpp` splits submeshes into a parent entity plus child entities, and children inherit the parent's material parameters via `copy_settings_from` (see [model-import.md](model-import.md)).

## 5. Resource System

- Virtual paths `res:/` are resolved by `resources/resource_path.cpp`; the project root is defined by `project.data`.
- `assets/asset_manager.cpp` is the single entry point for models and images; `assets/async_loader.cpp` provides async loading.
- Pack formats: `resources/pak_bundle.cpp` (`.pak` v3 / v4 encrypted) and `resources/gpack_bundle.cpp` (`.gpack` v1). Encryption uses `resources/aes_gcm.cpp` (AES-256-GCM, byte-compatible with GryceGC's `encrypt`, see [toolchain.md](toolchain.md)).
- Runtime hot reload: scenes, shaders, materials, and meshes can be reloaded without restarting the process.

## 6. Render Abstraction

The core rule on the render side: per-frame state writes must go through the `RenderContext` command buffer.

- `render/render_context.h` collects commands; `render/render_thread.cpp` executes them on a dedicated thread. Under `sync_mode`, commands execute inline on the main thread (the WebGL build uses this mode, see [webgl-backend.md](webgl-backend.md)).
- Backends plug in through the `IRenderBackend` interface in `render/render.h`. Missing capabilities degrade declaratively via `RenderBackendCapabilities` (for example, without compute shaders the engine falls back to CPU culling).
- GPU resources live in `render/storage_rd/`; pipeline implementations live in `render/renderer_rd/` (`forward_clustered` / `shadow` / `effects` / `environment`).

## 7. Platform Layer

`platform/window.h` wraps the GLFW window and context; `platform/input.h` handles keyboard and mouse. The desktop build supports interactive mode (mouse-look, WASD movement). The web build has additional browser-specific handling, see [webgl-backend.md](webgl-backend.md).
