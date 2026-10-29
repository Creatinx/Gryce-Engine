# Hardware Ray-Traced Reflections and Frame Reconstruction (Vulkan ray query)

This page covers the engine's hardware ray-traced reflection chain (BLAS/TLAS, tracing, denoising, compositing), its parameters, and the current state of frame reconstruction (super resolution / NVIDIA DLSS).

See also: [renderer.md](renderer.md) (overall pipeline and screen-space effects), [performance.md](performance.md) (measurement methodology), [shader-conventions.md](shader-conventions.md) (backend variants and macro rules).

## 1. Scope

| Item | Notes |
| --- | --- |
| Backend | Vulkan only (`VK_KHR_acceleration_structure` + `VK_KHR_ray_query` + `VK_KHR_deferred_host_operations`). No-op on OpenGL / D3D12 / WebGL |
| Preconditions | Device supports ray tracing **and** `rt_reflection_enabled` is true. Otherwise the post chain is silently skipped and the image is identical to before |
| Host pipeline | Hooked into `RenderPipeline` (the renderer `render_api.cpp` actually runs), not `RenderForwardClustered` |
| Shader | Inline GLSL compiled at runtime through shaderc. If shaderc is missing the pass disables itself and SSR takes over |

Ray-traced reflections and SSR are meant to be used exclusively: enable RT with `ssr_enabled=false`, otherwise reflections are double-blended and look too bright.

## 2. Architecture

### 2.1 Acceleration structures

- **BLAS**: cached per `IMesh*`, built once (`rt_blas_cache_`). Vertices/indices come from `IMesh::get_ray_tracing_geometry`; instances without CPU geometry are skipped.
- **TLAS**: rebuilt every frame from the opaque render list (`opaque_items_`: mesh handle, world matrix, material roughness/metallic).
- **No host-side waits**: the instance buffer is persistent host-visible memory (1.5× capacity headroom), instance data is written directly, and the build command is recorded into the frame command buffer (`record_build`). Build and query are ordered by GPU barriers only.
- Transforms: the engine uses column-major world matrices (`m[col*4+row]`) while `VkTransformMatrixKHR::matrix` is row-major `float[3][4]`; the conversion is element-wise in `vk_backend.cpp`.

### 2.2 Three-stage pass (`vk_ray_query_pass`)

| Stage | Resolution | Inputs | Outputs |
| --- | --- | --- | --- |
| trace | trace resolution (full res by default) | TLAS, HDR depth, HDR color, env cube, per-instance material SSBO | reflection color + blend factor; aux (world normal + linear depth) |
| temporal | same as trace | raw reflection, aux, history color + history aux, previous view-proj | accumulated reflection + factor |
| composite | output resolution | accumulated (or raw) reflection, aux, HDR color, depth | full-res result blended with the main image |

All three stages share one shader module and descriptor layout; the uniform field `resolutionStrength.w` selects the mode (0/1/2). History image pairs are ping-ponged (`frame_parity_`): write A, read B, swap next frame. On target recreation or the first frame, `history_valid_ = false` makes the blend factor 1 (current frame only).

At the end of each frame the output image is transitioned to `TRANSFER_SRC` and blitted back into the HDR color buffer by `vk_backend.cpp`.

### 2.3 Where reflection content comes from

1. **Smooth hit (roughness < 0.15)**: the hit point is reprojected to screen and sampled from HDR color with 4-tap bilinear — real scene color.
2. **Rough hit**: samples the IBL prefiltered environment mip chain (`roughness * max_mip`) and blends with the screen color using `smoothstep(0.15, 0.45, roughness)`.
3. **Miss / off-screen hit**: samples the IBL environment cube (radiance) — content a screen-space method can never produce.

## 3. Precision

| Technique | Implementation |
| --- | --- |
| Per-pixel tracing | `rt_reflection_resolution_scale = 1.0` by default. Measured difference between 0.5 and 1.0 is within noise (ray tracing is not the bottleneck), so precision is not sacrificed |
| Sub-pixel jitter | Trace UVs are offset by a Halton(2,3) sequence within ±0.5 texel; accumulated over frames this is equivalent to supersampling |
| Normal reconstruction | Symmetric central differences over full-resolution 1-texel neighbors (`cross(wR-wL, wU-wD)`). Neighbors that land on background or jump across a silhouette (>5% relative depth change) are replaced by the center sample |
| Material awareness | Instances carry `roughness/metallic` indexed by `instanceCustomIndex`; hits apply a Schlick Fresnel (`F0 = mix(0.04, 1, metallic)`) — grazing angles strengthen, roughness attenuates |
| Hit sampling | Hand-written 4-tap bilinear instead of nearest-neighbor |

> Do not pick "the neighbor with the smaller depth delta" per axis: on sloped surfaces the choice flips with sub-pixel position, the reconstructed normal jitters, and mirror floors show **regular horizontal stripes** (a bug we hit in this project).

## 4. Denoising

When `rt_reflection_denoise` is enabled (default), the temporal stage runs:

1. **Reprojection**: reconstruct the world position from the current depth and multiply by the previous view-proj.
2. **Geometry validation**: compare current and historical linear depth (relative difference > 18% is treated as occlusion/disocclusion); the weight goes to zero and the blend factor degrades to 1.
3. **Variance clipping**: a 3×3 neighborhood min/max bounds the history color to `[min - 0.15·span, max + 0.15·span]`, which suppresses ghosting.
4. **Blend**: `out = mix(history, current, α)` with α = 0.12 by default (about 8 frames to converge).

The composite stage uses a **depth-guided bilateral upsample** (2×2 bilinear weights × depth similarity) instead of plain bilinear, so texels on the far side of a silhouette stop contributing — object edges are not smeared by the trace grid.

Known limitations:

- Fast camera cuts invalidate the history for one frame; a brief convergence artifact is visible. Add a displacement threshold if that is unacceptable.
- Moving reflected objects leave a slight smear, bounded by the variance clamp; raise α (0.2–0.3) when a cleaner result is needed.

## 5. Frame reconstruction (super resolution) and DLSS

### 5.1 Modes

| Mode | Value | Notes |
| --- | --- | --- |
| `off` | 0 | Native resolution, no reconstruction; also disables reflection temporal accumulation |
| `temporal` | 1 | Built-in temporal reconstruction: jitter + reprojection accumulation + depth-guided bilateral upsample. No third-party runtime, **default** |
| `dlss` | 2 | NVIDIA DLSS. Falls back to `temporal` with a WARN log when the runtime is missing |

### 5.2 Runtime probing

`engine/render/super_resolution.{h,cpp}` probes files and symbols only; it does not change the device creation path:

- `sl.interposer.dll` loads and `slInit` / `slSetFeatureLoaded` / `slEvaluateFeature` resolve → Streamline runtime present;
- `nvngx_dlss.dll` and `sl.dlss.dll` found in the search path → NGX and plugin present;
- Search order: executable directory → its parent / grandparent → `third_party/streamline/bin/x64` → working directory.

The result is cached per process and logged once at startup (`SuperResolution: <mode> — <detail>`); `GRender_GetSuperResolutionStatus` returns the same text for UI display.

Headers ship with the repo: `third_party/streamline/include` (SL 2.14.16). The runtime binaries do **not** (size and licensing).

### 5.3 What is still missing for real DLSS output

In dependency order:

1. **Runtime binaries**: drop `sl.interposer.dll`, `sl.dlss.dll`, `nvngx_dlss.dll` next to the executable. Restart and the log flips from "not found" to "runtime ready".
2. **Device creation through the interposer proxy**: the only hard gap today. Streamline must intercept `VkInstance` / `VkDevice` creation (call the proxy `vkCreateInstance` / `vkCreateDevice` exported by `sl.interposer.dll` instead of the loader's originals), otherwise it cannot obtain internal resource handles and `slEvaluateFeature` will always fail. `VulkanInstance` / `VulkanDevice` currently talk to the loader directly. To wire it up:
   - probe the interposer in `vk_instance.cpp` and route all Vulkan entry points (`vkCreateInstance`, `vkCreateDevice`, `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`) through the proxy;
   - call `slInit` with a `sl::Preferences` that installs `logMessageCallback` so failures are visible;
   - set `SuperResolutionCaps::device_proxied` once the proxy is active — that is what makes `dlss_usable` true.
3. **Inputs**: DLSS needs jitter, depth, and **motion vectors**.
   - Jitter: the Halton sequence in `vk_ray_query_pass` can feed full-frame jitter (today it only affects reflection tracing).
   - Depth: the HDR depth buffer is available.
   - Motion vectors: only the **camera motion** equivalent exists today (current depth + previous/current view-proj). Exact for static geometry, wrong for moving objects. Full support requires per-pixel MVs from the geometry pass (previous clip minus current clip) — a separate change.
4. **Output resolution**: DLSS renders low and presents high, so the 3D pass resolution must be decoupled from the present resolution (`PostProcessParams::dlss_render_width/height` are already reserved).

Until 1–4 are all in place, `dlss_usable` stays `false` and the engine keeps the unverified branch out of the render path on purpose: an honest fallback beats a mysteriously blurry image on a real machine.

## 6. Configuration and API

`project.data`:

```json
{
  "rt_reflection_enabled": true,
  "rt_reflection_max_distance": 100.0,
  "rt_reflection_resolution_scale": 1.0,
  "rt_reflection_denoise": true,
  "super_resolution": "temporal"
}
```

C API (`engine/api/render_api.h`):

| Function | Notes |
| --- | --- |
| `GRender_SetRTReflection(bool)` / `GRender_IsRTReflectionEnabled()` | Master switch |
| `GRender_SetRTReflectionResolutionScale(float)` | Trace resolution scale, 0.25–1.0 |
| `GRender_SetRTReflectionDenoise(bool)` / `GRender_IsRTReflectionDenoiseEnabled()` | Temporal denoiser switch |
| `GRender_SetSuperResolutionMode(GRenderSuperResolutionMode)` | `GRENDER_SR_OFF / TEMPORAL / DLSS` |
| `GRender_GetSuperResolutionMode()` | Effective mode (returns TEMPORAL when DLSS was requested but is unavailable) |
| `GRender_GetSuperResolutionStatus(char* buf, int cap)` | One-line probe result, returns the length written |

Script side (`engine.render.*`): `set_rt_reflection` / `is_rt_reflection` / `set_ssr` / `is_ssr`. Bindings route through `AppHostServices` to the C API and must **never** link the renderer; add a virtual there when exposing new render capabilities instead of calling `GRender_*` from `js_app_bindings.cpp`.

## 7. Measurements

Environment: RTX 5070 Laptop (driver 621.01) + Vulkan, `rt_showcase.gesc` in `test/demo_project`, 300 frames × 3 runs, minimum taken.

| Configuration | Average FPS |
| --- | --- |
| Reflections off (baseline) | 243.9 |
| Reflections on / full-res trace / denoise on | 244.1 |
| Reflections on / half-res trace / denoise off | 243.7 |

Ray-tracing cost is within noise (<1%): this scene is limited by geometry and post processing, not by ray queries. The denoiser adds one dispatch at or above half resolution with no measurable difference.

## 8. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| Toggle on, image unchanged, no errors | The call site existed only in `RenderForwardClustered`; confirm the call after the SSR block in the `render_pipeline.cpp` forward-HDR path |
| Many `VulkanBuffer: failed to create buffer` on the first frame | VMA allocator missing `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` while BLAS/TLAS buffers request `SHADER_DEVICE_ADDRESS` |
| Access violation (`-1073741819`) on the first dispatch | `vkCreateComputePipelines` was never called, so `pipeline_` stayed `VK_NULL_HANDLE` |
| Black image, clean log, normal FPS | Multi-region UBO offsets must be rounded to `minUniformBufferOffsetAlignment` (256 on NVIDIA). Misaligned descriptors silently read zeros and the shader early-outs on `px >= res` |
| Regular horizontal stripes on mirror floors | Normal reconstruction flipped difference direction per axis; use symmetric central differences (section 3) |
| Reflections equal the surface color | Reconstructed normal points inward, so rays self-hit. Flip the normal toward the camera, offset the origin, raise `tmin` |
| GLSL failures on `texelFetch` / `rayQueryGetIntersectionWorldRay*` | Requires `GL_EXT_samplerless_texture_functions`; `rayQuery*WorldRayOriginEXT` does not exist in GLSL — compute `origin + rdir * t` yourself |
| Need Vulkan validation | Set `GRYCE_VK_VALIDATION=1` |

For "crashes inside the driver" or "silent black image" issues, converge with logging at key checkpoints — and remove the probes before committing.
