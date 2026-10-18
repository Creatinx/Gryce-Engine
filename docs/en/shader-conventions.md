# Shader Conventions

Shaders are shared source across four backends (OpenGL / Vulkan / D3D12 / WebGL), so several hard constraints apply. Violations typically surface as "a pass silently fails and the picture goes blank or garbled" without an obvious error.

## 1. Source Layout and Lookup

- GLSL sources live in `engine/render/shaders/`; the runtime lookup logic is in `render/core_shaders.cpp`.
- Variant priority is defined in `render/shader_source_resolver.cpp`: `vulkan_{name}` / `webgl_{name}` first, falling back to the base name.
- The precompiled SPIR-V in the demo project (`test/demo_project/shaders/`) is only a fallback artifact; `*.spv.hash` files are local cache fingerprints and are not committed.

## 2. Macros: One Definition Per Macro, Repository-Wide

D3D12's vulkanize (`d3d12_vulkanize`) replays **every** `#define NAME value` in the source to the top of the file, without respecting `#if` branches. The two-branch pattern below therefore produces two substitution values for the same macro:

```glsl
// Forbidden: after vulkanize replay, shaderc reports Macro redefined and the whole stage fails
#ifdef GRYCE_HAS_PCSS
#define GRYCE_HAS_PCSS 0
#else
#define GRYCE_HAS_PCSS 1
#endif
```

Backend-dependent switches must use a single `#ifndef` fallback, with the 0 value injected by a backend prefix (for example, the ESSL300 conversion in `gl_shader.cpp` injects 0 for WebGL):

```glsl
#ifndef GRYCE_HAS_PCSS
#define GRYCE_HAS_PCSS 1
#endif
```

## 3. Requirements for New Shaders

- A new shader must ship a `vulkan_*` variant (the Vulkan priority branch falls back to the base name, but SPIR-V and GLSL semantic differences require explicit variants).
- D3D12 uses only the GL source, translated by vulkanize; a missing variant fails first-run compilation, which silently kills the entire pass.
- Under WebGL the sampler count is a compile-time hard constraint; read section 5 before adding samplers.

## 4. ESSL300 Conversion Constraints (WebGL)

`to_glsl_es300()` in `gl_shader.cpp` performs four mechanical conversions at compile time, which impose corresponding requirements on shader source:

| Constraint | Reason |
| --- | --- |
| `#version` must be the first line | ESSL and ANGLE both require the version directive on the first line; even leading comments are rejected |
| Fragment shaders need precision declarations | ES 300 has no default float/int precision; the converter injects a `precision highp ...` block |
| No uniform initializers | ESSL 3.00 forbids them (ANGLE reports `cannot initialize this type of qualifier`), desktop GLSL 330 allows them. All engine uniforms are written per frame from the CPU, so stripping them does not change behavior. Project shaders relying on shader-side defaults will surface here |
| `texture2D(` must be compatible with `texture(` | The converter performs a defensive replacement |

## 5. Texture Unit Budget

- Desktop GL provides 32 fragment texture units; **real-GPU WebGL2 provides only 16** (SwiftShader reports 32, so software rasterization never exposes this, while real hardware renders black).
- Sampler counting happens at compile time: a declared and referenced sampler occupies a unit regardless of runtime `if` statements.
- pbr-family shaders trim features per backend: `pbr.frag` / `instanced.frag` 25 -> 14, `skinned_pbr.frag` 18 -> 14 (dropping PCSS raw depth, VSM/ESM, SDFGI, point-light shadows).
- Slot-to-unit mapping is tiered by `max_units` (`render/opengl/gl_slot_map.h`): 16 units use a compact table; trimmed samplers are bound to the 16+ dead zone, bound but never sampled. A single unit must not mix `sampler2D` and `samplerCube`.

## 6. Depth Resolve Convention

`depth_resolve` must use `depthTest=VK_TRUE` + `COMPARE_OP_ALWAYS` + `depthWrite=VK_TRUE`: always pass the test and write depth, resolve only, no filtering.

## 7. Shadow Bias

`slope_bias()` = `max(base*(1-N.L), base*0.3) * 2.0`. Seven receiver shaders must stay in sync: `pbr` / `instanced` / `skinned_pbr` / `vulkan_*` variants / `deferred_lighting`. When out of sync, borderline self-occlusion flips per pixel with the PCF rotation angle into 4px-periodic diagonal stripes.
