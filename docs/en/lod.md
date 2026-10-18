# Automatic LOD: Switch Distances Derived From Geometric Error

`MeshRenderer` enables automatic LOD by default: a full LOD chain is generated at import time, and at runtime a level is chosen per frame by **how many pixels the geometric error occupies on screen**, rather than by hard-coded distance bands.

Two granularities:

- **Per-object** (default, all models): one level per object. Distant and small objects rely entirely on this.
- **Chunked** (single huge models, >120k triangles): the mesh is split into 2^3~6^3 spatial chunks, each choosing its own level. It only takes over when "per-chunk totals clearly save triangles" (see section 6.3).

---

## 1. Why "Error -> Distance", Not "Distance -> Level"

Hard-coded distance bands (`40/120/320`) are all wrong when resolution, FOV, or model size changes:
the same 40u is "no visible difference" at 1080p / 60 degrees, and "obviously broken silhouettes" at 4K / 100 degrees.

So this goes the other way:

```
error / (dist * tan(fov/2)) * (height/2)  =  pixels occupied on screen
```

`error` is the **measured** geometric error of each simplification level (recorded during simplification, see section 2). Substituting it into the formula above and solving for the distance at which the error occupies exactly `lod_pixel_error` pixels gives that level's switch threshold:

```
dist_k = error_k * (height/2) / (lod_pixel_error * tan(fov/2))
```

Consequently:

- Doubling resolution -> thresholds double, degrade later (quality conserved);
- Larger FOV (wide angle) -> thresholds grow, degrade later (equally conserved);
- Scaling the model 2x -> world error 2x -> thresholds 2x, follows automatically;
- Raise `lod_pixel_error` to save more, lower it to be more accurate.

Near distances are always level 0 (the original mesh). Low levels only take effect at distances where the difference is invisible.

---

## 2. How the LOD Chain Is Generated (`assets/mesh_lod.cpp`)

Vertex clustering, not QEM edge collapse:

1. Partition vertex space with a cube grid of edge length `cell`; each cluster picks one **original vertex** as representative -- the one closest to the centroid of mass, not a recomputed centroid. Keeping original attributes (normal / UV / tangent / vertex color) never invents UV seams (synthesized vertices would place sample coordinates between two texels, showing up as texture stretching).
2. A triangle is discarded if two of its vertices map to the same cluster after mapping (degenerating to a line/point, rasterizes to no pixels).
3. `cell` is found by binary search on the target triangle count (6 iterations; triangles ~ 1/cell^2, converges fast with geometric bisection).
4. Each round records "the maximum displacement of vertices in the cluster to the representative vertex", which is this level's error upper bound; accumulated into `cumulative_error` (triangle inequality, conservative).

**Why not QEM**: photogrammetry / CAD meshes are often non-manifold, non-closed, and full of orphan triangles -- triangle soup. Edge collapse needs reliable half-edge topology and validity checks, and a priority queue over 1.4M triangles takes tens of seconds. Clustering is O(n) with two scans; measured: 1.46M triangles produce 3 levels in ~0.2s, and it **only deletes triangles, never moves connectivity**, so it can never break surfaces.

Measurements (main demo, 1.46M triangles):

| Level | Triangles | Level error | Cumulative error | Switch distance at 1080p/60deg/2px |
|------|--------|----------|----------|---------------------------|
| L0 | 469451 | -- | 0 | -- |
| L1 | 216216 | 0.2094 | 0.2094 | >= 97.9u |
| L2 | 113225 | 0.2446 | 0.4541 | >= 212.4u |
| L3 | 59328 | 0.3677 | 0.8217 | >= 384.3u |

Each level roughly halves the count, and cumulative error grows per level (because it clusters again on the previous level's representative vertices).

---

## 3. Runtime Selection (`components/mesh_renderer.cpp::update_lod`)

```
dist = distance to the nearest point of the bounding sphere, floored at 0.5 x distance to center
```

- **Why nearest point**: when a 50u-scale building is shot up close, center distance and nearest-point distance differ by a radius; using the center would classify close-ups as distant (a facade that should be L0 gets dropped to L2, visibly blurry).
- **Why a floor**: for huge models like whole cities / terrain that "contain the camera inside the bounding sphere", nearest-point distance is always 0 -> LOD permanently dead, no matter how `lod_pixel_error` is tuned. The floor is "half the distance to center": the closer to the center (truly standing inside the model), the more conservatively high detail is kept; when merely enclosed by the bounding box while actually looking at a distant surface, real-distance degradation is allowed.

**Hysteresis band of 8%**: no switching within +/-8% of the threshold, suppressing oscillation when the camera nudges the boundary. Forced levels (`GRYCE_FORCE_LOD`) skip hysteresis -- they are not boundary behavior by definition.

**Uploads are asynchronous**: switching levels only posts a "build mesh + upload" command, executed on the render thread. Until ready, `draw_mesh_handle()` falls back to the highest-resolution ready level (rather draw two extra frames of high detail than draw nothing on the switch frame).

**All draw passes must use the same level**: the main pass, shadow pass, and normal prepass all use `draw_mesh_handle()`. If the prepass writes L0 depth while the main pass draws L2, depth and geometry are not the same surface (pixels blocked by fine triangles in the prepass get judged as occluded in the main pass), showing up as edge flicker and holes; it also pays for high-detail geometry an extra time for nothing.

**Bounding sphere uses L0**: frustum and occlusion culling criteria always use the `gpu_mesh_handle()` (original mesh) bounding sphere, so culling does not jitter when LOD switches.

---

## 4. Switches and Tuning

| Means | Effect |
|------|------|
| `MeshRenderer::lod_enabled = false` | Disable per object (always draw L0) |
| `lod_pixel_error` (default 2.0) | Allowed screen error in pixels. **This is the main quality/performance knob** |
| `lod_bias` (default 1.0) | Threshold scaling. >1 switches to low levels later (protect quality), <1 earlier (protect performance) |
| `lod_distances` (default empty) | When non-empty, switch to explicit world-unit thresholds, hard bands, ignoring error derivation |
| `GRYCE_NO_LOD=1` | Global off, all objects draw L0 (control runs) |
| `GRYCE_FORCE_LOD=k` | Force all objects to level k (A/B screenshots) |
| `GRYCE_LOD_PIXEL_ERROR=x` | Override the `lod_pixel_error` default (sensitivity sweeps) |
| `GRYCE_LOD_DEBUG=1` | Print each selection's distance / radius / per-level thresholds / result (first 12) |
| `GRYCE_NO_CHUNK_LOD=1` | Disable only chunked LOD, keep per-object (A/B control) |
| `GRYCE_CHUNK_LOD_MIN_TRIS=N` | Chunking threshold (default 120000; submeshes below it are not chunked) |
| `GRYCE_CHUNK_NO_PIN=1` | Do not pin chunk boundary vertices (verify crack sources, measure pin cost) |
| `GRYCE_CHUNK_PIN_STATS=1` | Print the pinned vertex percentage |

This can be written directly in the scene file:

```json
{ "mesh_path": "res:/models/x.fbx",
  "lod_pixel_error": 4.0,
  "lod_distances": [40, 120, 320] }
```

---

## 5. Measurements

### 5.1 Quality and cost per level (main demo, camera [0,34,46], 1080p, MSAA 4x)

Using `GRYCE_LOD_PIXEL_ERROR` to drop all three submeshes to the same level, alternating two rounds in the same session and taking the median (`--no-debug-ui --no-fps`, see "laptops require alternating A/B" in performance.md).

| Config | Triangles | Mean pixel abs(diff) vs L0 | Share >2 pixels | GPU total | scene | vs L0 |
|------|--------|-----------------------|-------------|----------|-------|-------|
| L0 (`B=2`, default) | 1,457,686 | -- | -- | 8.00ms | 6.97ms | -- |
| L1 (`B=10`) | 738,326 (-49%) | 2.81 | 20.9% | 6.60ms | 5.73ms | **-17.5%** |
| L2 (`B=20`) | 361,430 (-75%) | 4.35 | 30.0% | 4.80ms | 4.09ms | **-40.0%** |
| L3 (`B=40`) | 186,194 (-87%) | 5.87 | 36.8% | 4.17ms | 3.49ms | **-47.9%** |

L3 keeps only 12.7% of the triangles and the picture still has no broken surfaces (clustering only deletes triangles, never moves connectivity); distant silhouettes and near facades are acceptable, with differences concentrated in high-frequency detail like roof tiles and railings.

### 5.2 Error budget -> actual level (this demo's camera)

`center_dist=47.5u`, `radius=62.4u` -> `dist = max(-14.9, 23.75) = 23.75u`, so each level's "error in pixels" at that distance decides whether to degrade:

| `lod_pixel_error` | Actual level | GPU total |
|---|---|---|
| 2 (default) | L0 | 8.00ms |
| 8 | only submesh 1 (smallest error) drops to L1 | 91.9 vs 92.1 mean |
| 10 | all L1 | 6.60ms (-17.5%) |
| 20 | all L2 | 4.80ms (-40%) |
| 40 | all L3 | 4.17ms (-47.9%) |

At the default 2px this scene **does not degrade**, which is correct: at camera distance 47.5u, L1's 0.2094u error occupies exactly 4.1 pixels at 1080p, over budget. To gain performance here, raise the budget to 10~20 (measured -17.5% / -40%), or accept "no degradation up close".

### 5.3 Long distance (verifying the automatic selection chain)

`--cam-pos` / `--cam-look` take three separate values (not a comma string):

```bash
./build/GryceRenderSmoke.exe --api opengl --project test/demo_project \
  --cam-pos 0 120 320 --cam-look 0 10 0 --frames 12
```

`dist=272u` -> automatically selects **L2** (thresholds 97.9 / 212.4 / 384.3), no forced switch needed.

### 5.4 Three-backend consistency

Under `GRYCE_FORCE_LOD=3`, all three backends degrade correctly (L0 -> L3 picture means):

| Backend | L0 | L3 | L0 vs L3 mean pixel abs(diff) |
|------|----|----|------------------------|
| OpenGL | 92.1 | 89.7 | 5.87 |
| Vulkan | 94.3 | 94.1 | 7.04 |
| D3D12 | 94.3 | 91.0 | -- |

(Backend names are `--api opengl|vulkan|dx12`; `d3d12` is rejected.)

---

## 6. Chunked LOD (single huge model)

### 6.1 Why it is needed

**When a single huge model fills the screen, per-object LOD cannot help.** The main demo is a 90u-wide city block with the camera standing at its edge; any per-object criterion (nearest point / to center / screen share) concludes "this object fills the screen -> use L0". This is not a criterion bug, it is the inherent granularity of per-object LOD:
near facades need L0, distant blocks could use L2, but one object can only have one level.

`ChunkedLod` in `assets/mesh_lod.h` splits it into spatial chunks: each chunk gets its own LOD chain, selects its own level by its own distance, and draws itself.

### 6.2 Construction (`build_chunked_lod`)

1. Model AABB -> `n^3` grid, `n = choose_chunk_cells(tris)`, i.e. `cbrt(tris / 20000)` clamped to `[2, 6]`.
2. **Triangles are assigned wholly by centroid**, no triangle is cut -- cutting would create self-made boundaries.
3. Each chunk rebuilds a compact mesh (vertices deduplicated by global index, chunks **share the same batch of vertex positions**), then runs the ordinary `build_lod_chain`.

**More cells is not better** (measured on the main demo, 1.46M triangles, close camera, 16px budget):

| Cells per axis | Chunks | Triangles/chunk | Per-chunk total triangles | GPU total (min of 4 rounds) |
|---|---|---|---|---|
| 2 | 8 | 200k | 1025k | 6.33ms |
| **3** | **20~21** | **23k** | **802k** | **6.15ms** |
| 4 | 40~45 | 11k | 920k | 6.48ms |

The reason is that **denser grid lines pin more seam vertices** (every internal grid face must be pinned, and all are non-mergeable vertices), eating the savings. So the default derives the cell count from "about 20k triangles per chunk".

**Cracks**: adjacent chunks share vertices that "fall near the chunk grid planes". If these vertices are merged at some level, the two sides no longer connect (T-junctions). So at construction time, vertices within the `pin.eps` band of grid lines are **pinned** at all levels (cluster representative = itself, using bit 63 as an exclusive key), both sides keep the same batch of positions, and the seam closes naturally.

`pin.eps = max(longest cell edge x 2%, diagonal x 1e-5)`. Measured on the main demo, `eps=0.899` pins only 4.3% of vertices, with build time unchanged.

> **Pitfall encountered**: after adding pinning, build time went from 231ms to 2579ms (10x). Not "too many pins" (`GRYCE_CHUNK_PIN_STATS` shows only 4.3%), but `std::round` being a non-inlined libm call placed inside the binary search inner loop. Switching to `static_cast<int>(t + 0.5f)` and caching pin decisions per input mesh in Scratch brought the cost to zero (217ms vs 231ms).

### 6.3 Runtime: **Take Over Only When Triangles Are Actually Saved**

Each frame, compute the whole-model (per-object) level first, then per-chunk levels, then compare total triangles:

```
use_chunks = chunk_tris + 2% < whole_tris
```

- **Far view**: the whole model can already drop to L2/L3, while chunking "drops one level per chunk" yields more in total, plus dozens of extra draws and per-chunk L0 VRAM -> always draw whole-model.
- **Close view** (camera hugging / standing inside the model): the whole-model criterion can only pick L0, and only chunking can trade near and far -> chunking takes over.

This criterion also solves "paying the cost for nothing in default configs": in the main demo with default `lod_pixel_error=2`, all three chunks are L0, `chunk_tris == whole_tris`, so **not one chunk geometry is uploaded**, and VRAM and draw batches match pure per-object exactly.

Chunk geometry is **uploaded on demand** (`request_chunk_upload`, including L0). The readiness criterion is "each chunk has at least one level available", not "each chunk's target level is ready": the latter would make the 1~2 frames of one chunk's level switch fall back to whole-model drawing, flickering L0 whenever the camera moves. While the target level is still uploading, `draw_mesh_handle_at` falls back per chunk to the **highest currently ready level** -- the same geometry, just a coarser level, neither redrawing nor missing geometry. Only when a chunk has no level at all does it fall back to one whole-model draw (mixing half chunks + whole model would draw the same geometry twice).

> **Pitfall encountered**: `chunks_ready()` required "each chunk's current level uploaded", while `update_lod` wrote `if (st.active > 0)` before posting and `request_chunk_upload` returned directly for `level <= 0` -- L0 chunks never uploaded, readiness was permanently false, and chunking never took over (symptom: "the level-selection histogram is entirely correct but the picture is pixel-identical to L0").

### 6.4 Measurements (main demo, 1080p, MSAA 4x, alternating 4 rounds in-session, minimum)

Camera close to the model (`--cam-pos 0 24 26 --cam-look 0 12 0`, model center 25.7u), 16px budget:

| Config | GPU total | vs L0 |
|---|---|---|
| `GRYCE_NO_LOD=1` (constant L0) | 7.25ms | -- |
| Per-object LOD (`GRYCE_NO_CHUNK_LOD=1`) | 6.84ms | -5.7% |
| **Chunked LOD (default 3^3)** | **6.17ms** | **-14.9%** |

Chunking is another **-9.8%** faster than per-object. Quality is also better: chunked picture mean 87.0, L0 is 87.1, 2^3 chunking is 86.8 -- chunking saves triangles far away while keeping L0 up close.

Level distribution (3^3): sub0 `L0x9 L1x7 L2x4`, sub1 `L0x10 L1x6 L2x4`, sub2 `L0x12 L1x6 L2x3`; per-chunk totals 231k + 291k + 280k = 802k, where the whole-model criterion can only give 469k / 309k / 446k (1224k total).

At lower budgets the whole model wins (chunking does not take over when it has no advantage):

| `lod_pixel_error` | Per-object GPU total | Chunked GPU total | Decision |
|---|---|---|---|
| 4 | 7.51ms | 7.54ms | some submeshes whole-model |
| 8 | 7.57ms | 7.61ms | same |

Far view (default camera, model center 47.5u): at 2px, chunking and per-object are both L0 (8.5 vs 8.6ms, within noise); at 40px the whole model wins (chunking ~150k triangles vs whole-model L3 59328), 3.90 vs 4.09ms -- **no regression**.

At 16px the picture's mean pixel abs(diff) vs L0 is only **0.816** (>8 difference 3.25%); no detail loss is visible at a 16px budget.

**Three-backend consistency**: same position at 16px, GL / VK / DX12 have line-identical level selection, per-chunk triangle counts, and takeover decisions (sub0 `L0x9 L1x7 L2x4` 231280 vs whole-model 469451; sub1 291278 vs 308620; sub2 `L0x12 L1x6 L2x3` 279904 vs 445717), picture means 87.0 / 94.5 / 93.9 -- differences come only from each backend's tone mapping; LOD decisions are backend-independent.

### 6.5 Seam verification

At the same budget, A/B against `GRYCE_CHUNK_NO_PIN=1`, counting pixels that are "model at L0, sky after degradation" (a hole proxy): pinned = 1537 pixels, unpinned = 2981 pixels (**-48%**). Net of normal silhouette shrinkage, pinning basically plugs the seams; no grid-line cracks are visible in the picture.

### 6.6 Cost

The main demo's three submeshes (1.46M triangles) chunk-build in about **0.96s** total (one-time, `build 308 + 327 + 329ms`, of which chain generation is ~60%), small next to the 92MB FBX import time (~12s). 3^3 is even faster than 2^3 (1.23s -> 0.96s): smaller chunks simplify faster. VRAM only grows on demand when chunking actually takes over (equal to the drawn chunks' geometry); zero in default configs.

---

## 7. Known Boundaries

- Chunking is a **uniform cube grid**, not adaptive to geometry distribution. "Thin" models like cities / terrain rely on `min_cell = diagonal x 1e-3` on degenerate axes and may produce empty chunks (empty chunks are not generated).
- Chunk level switching has **no transition**: when adjacent chunks differ by two levels, a detail pop is visible at the boundary (no geomorph / cross-fade). Currently suppressed by "pinned boundary vertices + 8% hysteresis".
- Per-chunk culling only does frustum culling, no HZB occlusion culling -- back-face chunks are still drawn.
- Each chunk's independent LOD chain **does not reuse** cross-chunk information, so total triangles are 10~20% above the theoretical optimum (this is the root of "chunking does not always win", avoided by the section 6.3 criterion).
- Chunks below `min_triangles=512` degenerate to a single-level L0-only chain (dropping them would leave holes).

---

## 8. Troubleshooting

| Symptom | Look at |
|------|------|
| LOD has no effect at all | `GRYCE_LOD_DEBUG=1`: is `cam_dist` clamped very small (camera inside bounding sphere); are `thresholds` far larger than actual distance |
| Silhouette jumps when switching levels | `lod_pixel_error` too small / `lod_bias` outside the hysteresis band; or adjacent levels' errors differ too much (lower `ratio_per_level`) |
| Model disappears for the two frames of a switch | The `draw_mesh_handle()` fallback logic is broken (should fall back to the highest ready level, not return an invalid handle) |
| Edge flicker / holes | Some pass does not use `draw_mesh_handle()` (prepass / shadow / main pass must agree) |
| Shadow finer / coarser than the body | Same: the shadow pass also uses the current level |
| Small objects have no LOD | Below `min_triangles=512` they are skipped directly (not worth generating a chain) |
| Chunk level histogram correct but picture equals L0 | `chunks_ready()` permanently false: some chunk's current level not uploaded (L0 must also upload, see 6.3). `GRYCE_LOD_DEBUG=1` to see `ready=` and `uploads=` |
| Chunking slower than per-object | Check the log `tris whole L?(N) vs chunked(M)`: chunking should only take over when M is clearly smaller than N (the 6.3 criterion) |
| Grid-line cracks at chunk boundaries | Compare with `GRYCE_CHUNK_NO_PIN=1`; `pin.eps` must cover vertex offsets of triangles crossing grid lines |
| Slow chunk build | `GRYCE_CHUNK_PIN_STATS=1` for the pinned percentage; `assemble / chain` are timed separately in the build log |
