# D3D12 用 DXR 实现硬件光追反射（RT Reflection）实施计划

> 目标：在 D3D12 后端接入与现有 Vulkan 光追反射（`vk_ray_query_pass`）等价的硬件
> 反射链路，复用既有的公共门控、参数口径与合成合约，仅在后端填补 `apply_ray_query_reflection`
> 的真实实现（当前 D3D12 继承的是 `render.h:182` 的 no-op 默认，等于未实现）。

---

## 1. 现状接合点（Seam）

- 公共入口已经齐备，无需改动：
  - `IRenderBackend::apply_ray_query_reflection(...)`（`render.h:182`，默认 no-op）。
  - 门控 `render_pipeline.cpp::rt_reflection_active()` = `rt_reflection_enabled && backend->supports_ray_tracing()`。
  - C 侧接口 `GRender_SetRTReflection` / `IsRTReflectionEnabled` / `SetRTReflectionResolutionScale` / `SetRTReflectionDenoise`；脚本 `set_rt_reflection/is_rt_reflection`；`project.data` 的 `rt_reflection_enabled`。
  - 合成口径：反射"谁该像镜子"由前向 pass 写入 HDR alpha 的反射率 ρ 决定，compositor 按 `cover * ρ` 混合（见 `rt-reflection.md` 第 9 节）。**任何后端只要输出 `rgba16f(反射色, cover)` 并满足该合约，画面即等价。**
- 唯一需要新增代码的接缝：D3D12 backend 覆盖 `apply_ray_query_reflection`，做"收集实例 → 建/更新 TLAS → 三段 compute 派发（trace/temporal/composite）→ 拷回 HDR 颜色"。其结构与 `vk_backend.cpp:1293` 的 Vulkan 实现一一对应，可直接照骨架移植。
- D3D12 后端已有 DXC 编译链（`d3d12_dxc.cpp`）与 SPIRV-Cross / vulkanize 桥接（`d3d12_spirv_cross.cpp`、`d3d12_vulkanize.cpp`），新增 HLSL compute 走 DXC 即可，无需引入第三方依赖。

---

## 2. 技术选型：Inline Raytracing（DXR Tier 1.1），而非 Ray-Gen Pipeline

Vulkan 路径用的是 **ray query**（`rayQueryEXT`，在计算着色器内联做 BVH 遍历，无 `vkCmdTraceRays`、无 SBT）。DXR 的等价物是 **Inline Raytracing**（`TraceRayInline`，DXR Tier 1.1）：

| 维度 | Vulkan 现状 | DXR 对应 |
| --- | --- | --- |
| 遍历方式 | `rayQueryEXT` 内联于 compute | `TraceRayInline` 内联于 compute（`cs_6_5`） |
| 是否需要 Shader Table / StateObject | 否 | 否（inline 不需要 `CreateStateObject` / `DispatchRays`） |
| 加速结构 | BLAS/TLAS（`VK_KHR_acceleration_structure`） | `D3D12_RAYTRACING_ACCELERATION_STRUCTURE`（Tier 1.0+） |
| 增量更新 | `MODE_UPDATE` refit | `BUILD_FLAG_PERFORM_UPDATE` |
| 查询 API | `vkCmdBuildAccelerationStructuresKHR` | `ID3D12GraphicsCommandList4::BuildRaytracingAccelerationStructure` |

为什么不是 Ray-Gen（`DispatchRays` + SBT + `CreateStateObject`）：现有三段逻辑全是计算着色器，且 Vulkan 侧已明确"ray query 不需要 `vkCmdTraceRays` 额外 PFN"（见 `vk_ray_query_pass.h:23`）。用 Ray-Gen 会凭空引入 SBT、命中/最近命中着色器、`CreateStateObject` 等大量与现有设计不兼容的复杂度，而 inline 几乎是一比一移植。**因此目标能力定为 D3D12 Raytracing Tier 1.1 + Inline Raytracing。**

---

## 3. 分阶段实施

### Phase 0 — 能力探测与设备获取
- 在 `D3D12Backend` 初始化时取 `ID3D12Device5`（或 `ID3D12Device7`）——`CreateDevice` 后 `QueryInterface` 升级；`CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5)` 读 `RaytracingTier`。
- `supports_ray_tracing()` 返回 `RaytracingTier >= D3D12_RAYTRACING_TIER_1_1`。低于 1.1 时 `apply_ray_query_reflection` 走 no-op（与现状零差异，自动回退 SSR）。
- 命令列表取 `ID3D12GraphicsCommandList4/5`（构建 AS 所需）。

### Phase 1 — 加速结构（对照 `vk_acceleration_structure.cpp`）
新增 `d3d12_acceleration_structure.{h,cpp}`，镜像 Vulkan 的 BLAS/TLAS 接口：
- **BLAS**：`D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC`，`GEOMETRY_TYPE_TRIANGLES`，`RAYTRACING_GEOMETRY_FLAG_OPAQUE`，顶点格式 `R32G32B32_FLOAT` + `uint32` 索引。结果缓冲带 `RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS` 与 `RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE`；配套 scratch 缓冲。
- **TLAS**：`INSTANCE_DESC`（4×3 transform + instance id + BLAS GPU VA）。同样持久缓冲 + 帧内构建。
- **Refit（关键性能项）**：首建带 `RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE`；每帧若仅有实例变换变化，用 `PERFORM_UPDATE`；实例集合/数量/容量变化才整树重建。判定逻辑与 Vulkan 一致——按实例顺序的 BLAS GPU VA 拓扑签名（顺序敏感），签名不变即 refit。
- BLAS 按 `IMesh*` 缓存（几何静态只建一次），与 `rt_blas_cache_` 同口径；获取 CPU 几何走 `IMesh::get_ray_tracing_geometry`（Vulkan 已接好）。
- 构建命令记录在主命令列表上（与当帧工作一起排队），靠 `UAV`/`AS` barrier 串序，无主机端 `WaitForFences`——对齐 Vulkan 的"零主机端等待"设计。

### Phase 2 — 三段式 compute 着色器端口（HLSL inline）
把 `vk_ray_query_pass.cpp` 内联 GLSL 的 trace / temporal / composite 三段端口为 HLSL `cs_6_5`，用 `TraceRayInline` 替换 `rayQueryEXT`：
- **trace 段**：从深度重建世界坐标 + G-buffer 法线（或无则深度重建）；`RayDesc{Origin, Direction, TMin=0.02, TMax=maxDist}`；`InlineRaytracingStateDesc` 设 `RAY_FLAG_FORCE_OPAQUE`（几何全不透明，等价于 Vulkan 的 `gl_RayFlagsOpaqueEXT`）；`TraceRayInline` 后读 `CommittedIntersection` 取 `T`、命中世界点、屏幕内/外判定。反射率门控（HDR alpha ρ ≤ 0.02 跳遍历）、天空/远平面早退、环境 cube / prefilter 采样——全部原样搬。
- **temporal 段**：3×3 方差裁剪 + 重投影（用 `prevViewProj` 与线性深度几何校验），逻辑与 GLSL 一致。
- **composite 段**：以线性深度为引导的 2×2 双边上采样，按 `cover * ρ` 与 HDR 颜色混合。
- 绑定模型：深度/HDR/env cube/prefilter → `SRV`；反射/辅助/历史图 → `UAV`；每实例材质 → `StructuredBuffer`（对应 Vulkan SSBO）；UBO（三段共用，256 字节步长）→ `ConstantBuffer` 或 root constants；TLAS → `SRV`（`DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE`）。
- 编译：DXC 以 `cs_6_5` + `-fdk-`/raytracing 库链接产出；落地到现有 shader 加载/缓存体系（与 `d3d12_dxc` 对齐）。

### Phase 3 — D3D12 后端接线 `apply_ray_query_reflection`
- 骨架直接移植 `vk_backend.cpp:1293`：遍历 `instances` → 建/取 BLAS → 填 `INSTANCE_DESC` → 拓扑签名 → TLAS build/refit → 三段 compute dispatch（先结束当前 render pass，在主命令列表上派发，必要时恢复 forward/swapchain pass，与 Vulkan `仿 dispatch_compute` 同思路）→ 将 composite 输出拷回 HDR 颜色（`CopyResource` / `ResolveSubresource`，等价 Vulkan 的 `vkCmdBlitImage`）。
- 材质参数 SSBO（`mat_params`）同样持久 host-visible，按实例数增长重建，与 Vulkan `rt_params_buffer_` 同口径。

### Phase 4 — 与现有 compositor 合约对齐
- 输出严格为 `rgba16f(反射色, cover)`，由 `render_pipeline` 里既有的合成逻辑按 `cover*ρ` 与 HDR 混合。**不要**在 DXR 着色器里决定反射强弱（强弱已在合成段由 ρ 决定，见 `rt-reflection.md` 第 4/9 节），否则会与 Vulkan/GL 画面口径不一致。
- `rt_reflection_resolution_scale` 默认 1.0（与 Vulkan 一致；半分辨率路径在 GL/VK 尚未收敛，DXR 同样先锁 1.0，待 `rt-reflection.md` 提到的半分辨率一致性问题解决后再放开）。

### Phase 5 — 验证与性能
- 门禁：在有 Tier 1.1 GPU 的 CI/本地跑既有 demo 场景，对比 DXR 与 Vulkan 反射画面（相同 `project.data`、相同相机）像素级一致性（允许 float16 误差）。
- 性能：验证 refit 生效（绝大多数帧走 `PERFORM_UPDATE`，GPU 时间应接近 Vulkan refit 路径），用 PIX / `build_p*.log` 同款手段抓 AS 构建耗时。
- 回归：关掉 RT（`rt_reflection_enabled=false`）画面与现状零差异；无 Tier 1.1 设备自动回退 SSR。

---

## 4. 必须与 Vulkan 路径对齐的合约（跨后端一致性）

1. 输出 `rgba16f(反射色, cover)`；合成由公共逻辑按 `cover*ρ` 加权（ρ = HDR alpha）。
2. 反射率门控阈值 0.02、天空/远平面早退、环境 cube/prefilter 采样口径、temporal 混合系数（默认 0.12≈8 帧收敛）、composite 双边引导参数——全部沿用 `vk_ray_query_pass.cpp` 的常量，避免三后端各一套数值。
3. 命中点在屏幕内取 HDR 颜色、屏幕外退 prefilter 最粗 mip、完全未命中取环境 cube——与 Vulkan 三段语义完全一致。
4. 半分辨率（scale<1.0）的命中覆盖率一致性问题（`rt-reflection.md` 已知未收敛）对 DXR 同样存在，先强制 1.0。

---

## 5. 风险与未决项

- **DLSS / 超分**：`rt-reflection.md` 第 5 节已指出 Vulkan 路径 `dlss_usable=false`（缺 Streamline interposer 代理创建 + 逐像素 MV）。DXR 路径同样缺失，**本期不接入 DLSS**，reflection 默认走内置 temporal；若要做，需先在 D3D12 后端补齐 Streamline 接入与 MV 输出。
- **运动矢量**：temporal 重投影依赖 `prevViewProj` + 深度推导，不依赖逐像素 MV，因此现有 compositor 可直接复用；若后续要做更激进的时域累积，需要真正的 MV。
- **inline raytracing 精度**：`TraceRayInline` 的 `Candidate/CommittedIntersection` 与 `rayQueryEXT` 的语义基本同构，但浮点累加顺序可能带来亚像素级差异；通过 Phase 5 的像素级对比把关。
- **资源状态/屏障**：DXR AS 构建前后需要正确的 `RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE` ↔ `UNORDERED_ACCESS` 转换，以及 compute 读写 UAV 的 `RESOURCE_STATE_UNORDERED_ACCESS` 过渡；D3D12 显式屏障比 Vulkan layout 转置更易漏，需在接线时逐资源核对。

---

## 6. 工作量估计（粗）

| 阶段 | 关键产出 | 估计 |
| --- | --- | --- |
| Phase 0 | 设备5 + Tier 探测 + `supports_ray_tracing` | 0.5 天 |
| Phase 1 | `d3d12_acceleration_structure`（BLAS/TLAS + refit） | 2 天 |
| Phase 2 | 三段 HLSL compute（trace/temporal/composite）+ root signature | 3 天 |
| Phase 3 | `apply_ray_query_reflection` 接线 + 拷回 HDR | 2 天 |
| Phase 4–5 | 合约对齐 + 像素级验证 + refit 性能确认 | 2 天 |
| 合计 |  | ~9–10 天 |

最小可跑子集：Phase 0→1→2→3 即可在 D3D12 上点亮反射（先不做半分辨率与 DLSS），后续再补 Phase 4–5 的精细对齐。
