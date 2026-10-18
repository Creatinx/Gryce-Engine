# Rendering Performance: How to Measure, Where the Time Goes, What Has Been Fixed

This document records a performance investigation from October 2026 against `test/demo_project/scenes/main.gesc` (a 3d66 photogrammetry model, 4.37M vertices / 1.46M triangles). Example numbers are measured on an RTX 5070 Laptop, 1920x1080, MSAA 4x, SSR+SSAO+IBL all on.

---

## 1. Measure First, Then Act

The smoke program has built-in segmented GPU timing; no external tools needed:

```bash
./build/GryceRenderSmoke.exe --api opengl \
  --project test/demo_project --scene test/demo_project/scenes/main.gesc \
  --bench 120 --resolution 1920x1080 --no-debug-ui --no-fps --no-vsync
```

Two blocks of output:

```
[gpu] scene                8.333    8.120   86.9%  n=118
[gpu] prepass_normal       0.870    0.849    9.1%  n=118
[gpu] ...
[gpu] total (per frame)              9.36
[cpu] frame time samples=94  mean=11.30ms(88.5 FPS)  median=...  p99=...
[cpu] phase means: RenderWorld=... EndFrame(submit+wait GPU)=...
```

Deciding who is the bottleneck:

- `EndFrame(submit+wait GPU)` clearly larger than `RenderWorld` -> **GPU-bound**, look at the `[gpu]` breakdown.
- The reverse -> CPU-bound, look at `[cpu] phase means`.
- When `[gpu] total` and `[cpu] mean` are close, both are near saturation.

### Laptops Require Alternating A/B

The same configuration can differ by 2x between two consecutive runs (this machine at 79 degrees C, SM clocks 1762/3090 MHz, power-wall fluctuations are extreme). Any "the change is faster" conclusion must come from **alternating runs within the same session**, otherwise you are measuring thermal drift:

```bash
for i in 1 2 3; do
  run A; run B;      # A/B alternating, 2~3 runs each, take the median
done
```

Likewise, `--no-xxx` style toggle comparisons must also be run alternating. Two diagnostic switches in the repository exist for this:

| Environment variable | Effect |
| --- | --- |
| `GRYCE_NO_MESH_WELD=1` | Disable import-time vertex welding (control for vertex cost) |
| `GRYCE_FORCE_PCSS=1` | Ignore the PCSS degradation criterion, force keeping the blocker search |

---

## 2. Where the Time Goes (1080p measured, before optimization)

| Item | Time | Share | Notes |
| --- | --- | --- | --- |
| `scene` forward pass | 11.0 ms | 92% | skybox + meshes + opaque/transparent objects |
| ├ PCSS shadows | 2.15 ms | 18% | 16 blocker searches + 16 PCF = 32 samples per pixel |
| ├ MSAA 4x | 1.6~2.0 ms | 15% | 4x color/depth write bandwidth |
| ├ IBL (two cubemaps + LUT) | 0.83 ms | 7% | |
| └ Geometry (1.46M triangles) | ~3.1 ms | 26% | Resolution-independent: still 3.1 ms at 640x360 |
| `prepass_normal` | 0.87 ms | 7% | redraws geometry for SSAO/SSR |
| SSAO / SSR / bloom / tonemap | < 0.4 ms | 3% | all cheap |

### A few easily misjudged observations

- **Lowering resolution barely saves geometry cost**: 1920x1080 -> 640x360 takes total time from 10.5 to 4.6 ms, but 3.1 ms of that is geometry and cannot be pushed down. To judge "is it really vertex-bound", render the heavy-model scene and a cube scene at the same low resolution: 3.29 ms vs 0.22 ms.
- **Welding vertices cannot save geometry when triangle count is unchanged**. Vertex welding cuts vertices 4x, geometry only gets 3% faster -- the bottleneck is the assembly and subpixel coverage of 1.46M triangles, not vertex fetching.
- **PCSS under this configuration may be entirely wasted work** (see below).

---

## 3. Three Things Already Done

### 3.1 Import-time vertex welding (`engine/assets/mesh_optimizer.*`)

Photogrammetry / CAD exports are often **fully de-indexed**: the index buffer degenerates to `0,1,2,3,...`, with every triangle carrying 3 independent vertices. This model's `verts/tris` is exactly 3.00.

`AssetManager` calls `optimize_imported_mesh()` uniformly after getting each format's import result:

- Rebuilds indices by **bitwise equality of all vertex attributes** (memcmp, hash only for bucketing), never with floating-point tolerance -- vertices differing in position / normal / UV / tangent / vertex color are never merged, so no UV stretching and no hard edges turned soft.
- Removes degenerate triangles on the way.
- Measured: 4.37M -> 1.08M vertices (4.1x), vertex buffer 233 MB -> 58 MB, triangle count unchanged, **pixel-identical picture (maxDiff = 0)**.
- Benefit: GPU about 3%, 175 MB VRAM, proportionally less upload time after import.

Log (one line per import):

```
MeshOptimizer[assimp]: 'xxx' vertices 1408353 -> 351633 (4.0x, 75.2MB -> 18.8MB),
                       tris 469451 -> 469451 (degenerate 0), source was fully de-indexed
```

### 3.2 PCSS automatically falls back to PCF under degenerate parameters (`RenderPipeline::pcss_effectively_enabled`)

PCSS's blocker search serves exactly one quantity: penumbra width. And the penumbra is ultimately clamped to `[shadow_filter_radius, uPCSSMaxRadius]`, whose lower bound is the screen-space-consistent radius used by PCF.

When `pcss_light_size * pcss_blocker_scale < 1`, computing a penumbra wider than 1 texel requires "receiver depth greater than 1/(light_size*blocker_scale) times the occluder depth", which in CSM almost only happens near the near clip plane. That is, the blocker search result gets thrown away directly.

The demo's `pcss_light_size = 0.3` is in this class: **PCSS on vs off screenshots are pixel-identical (meanDiff = 0, maxDiff = 0) yet cost 2.79 ms more**. The engine now recognizes such settings, falls back to PCF automatically, and logs the reason; to actually see contact hardening, raise `pcss_light_size` to 10~100.

### 3.3 22 "uniform not found" warnings per frame

The forward shader unconditionally writes a batch of uniforms belonging only to the deferred pipeline (`uGIMode`, `uShadowAtlasEnabled`, `uUseSSIL`, `uVSMTexture*` ..., 14 kinds in total). Each `set_*` constructs a `std::string`, queries a hash table, formats a log line, and takes a lock to write to disk:

```
2760 warnings in 124 frames ~ 22 per frame
```

Now warnings are emitted once per `(setter, uniform)`, with the conclusion spelled out ("not declared by this shader (ignored)"), the log is immediately clean, and the GPU segmented timing output is no longer sliced apart by async logging (previously the `total` line was often swallowed and unreadable).

---

## 4. Current Watermark

| Config | GPU total | Relative |
| --- | --- | --- |
| Before optimization | 11.98 ms | -- |
| Vertex welding | 11.7 ms | -3% |
| + PCSS automatic degradation | 9.36 ms | **-22%** |
| + MSAA off (reference) | 7.3 ms | -39% |

The `scene` pass went from 11.04 ms down to 8.33 ms.

---

## 5. Not Done Yet (by benefit)

1. ~~**LOD / triangle culling**~~: per-object automatic LOD is integrated (see [lod.md](lod.md)). A full LOD chain is generated at import time, and runtime switch distances are derived from "how many pixels the geometric error occupies". Main demo measurements: `lod_pixel_error` 2 -> 10 -> 20 -> 40 corresponds to GPU total 8.00 -> 6.60 (-17.5%) -> 4.80 (-40%) -> 4.17 ms (-47.9%). At the default 2px this scene does not degrade (camera 47.5u, L1 error is exactly 4.1 pixels at that distance); raise the budget to gain performance.
   ~~**Chunked LOD**~~ is also integrated (`assets/mesh_lod.cpp::build_chunked_lod`): submeshes over 120k triangles are cut into 2^3~6^3 spatial chunks, each with its own chain and level selection, with boundary vertices pinned against cracks. **Key criterion**: it only takes over when "per-chunk total triangles are clearly fewer than the whole model's current level" -- at distance the whole model dropping one level suffices and chunking is instead more (main demo far view at 40px: whole-model L3 59k vs chunked 206k).
   Cell count is not "more is better": main demo near view at 16px, 2^3/3^3/4^3 correspond to GPU 6.33/6.15/6.48ms (4^3 regresses because denser grid lines pin more seam vertices); the default derives 3^3 from "about 20k triangles per chunk".
   Close view (camera hugging the model, whole-model criterion can only pick L0) measured at a 16px budget **6.17 vs per-object 6.84ms (-9.8%), vs constant L0 7.25ms (-14.9%)** (4 alternating rounds, minimum), picture vs L0 mean abs(diff) 0.816; at the default 2px no chunk geometry is uploaded at all, with VRAM and batches identical to pure per-object. Build is one-time ~0.96s / 1.46M triangles.
   Tried and ineffective: opaque draw near->far sorting (early-Z friendly). This scene is triangle-assembly bound, not fill bound, so it saves no shading and instead disrupts material binding order -- measured 4% slower (6.50 vs 6.26ms), reverted.
2. **Depth prepass reuse**: `prepass_normal` (0.87 ms) currently draws geometry separately, but its depth output is not the same buffer as the main pass's MSAA depth, so the main pass still pays full overdraw. Unifying prepass depth with the main pass's depth (or having the main pass use a `depth ==` test) could remove the duplicate shading. (The prepass now follows the current LOD level, no longer paying high-detail geometry for nothing.)
3. **MSAA downshift**: 4x -> 2x saves 0.4 ms, 4x -> off saves 2.0 ms. The visual difference is real; defaults are unchanged; for frame rate in heavy scenes use `--msaa 2`.
4. **When PCSS is really needed**: the blocker search can go from 16 to 8 samples (penumbra is an average, insensitive to sample count), saving about 0.5 ms more. The default currently uses PCF, so there is no need yet.
