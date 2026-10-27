# 执行计划：NVIDIA RTX 能力 + WebGL → WebGPU

> 制定时间：2026-10-27
> 配套调研：`docs/zh/plans/rtx-webgpu-research.md`（工作量估算与依据）
> 排序原则：**先补（把地基补齐，让后续每一项都只付一次钱），再做（在补好的地基上加能力）**
> 单位：人日 = 8 小时有效开发，单人全职

---

## 0. 为什么是这个顺序

三件事（光追 / DLSS / WebGPU）看起来独立，实际共享两块地基：

```
                    ┌─────────────────────────────────────┐
                    │  P0  运动矢量 + 上一帧矩阵 + jitter   │  ← 先补，只做一次
                    │  （改一处，DLSS 与光追同时受益）      │
                    └──────────────┬──────────────────────┘
                                   │
        ┌──────────────────────────┼──────────────────────────┐
        │                          │                          │
   ┌────▼─────┐            ┌──────▼──────┐            ┌──────▼──────┐
   │ P1 DLSS  │            │ P2 硬件光追  │            │ P3 WebGPU  │
   │ SR+Reflex│            │ (先Vulkan)   │            │ 骨架+WGSL  │
   └──────────┘            └─────────────┘            └─────────────┘
        └──────────────►  P4 帧生成 / D3D12 光追 / P5 WebGL 下线
                        （依赖前面验证过的产物，再决定投入）
```

**P0 是唯一的"先补"阶段**。当前引擎的运动矢量是半成品（两套各缺一半、且物体运动其实也没实现 —— `render_forward_clustered.cpp:1004` 直接 `prev_world = world`），上一帧 view-projection 矩阵**全仓不存在**。DLSS 帧生成和光追的反射/阴影质量都依赖它。不先补，后面每一项都要重新动一次相机与渲染元素的跨帧状态。

**为什么 P1 排在 P2 前面**：DLSS SR 的投入产出比最高（不依赖 RT、只依赖 D3D12、改动面小），而且**它的 ghosting 表现是运动矢量正确性的最佳体检工具** —— 先把 MV 补对并用 SR 验证，比先做光追再回头发现 MV 错了要划算。

**为什么 P2 先只做 Vulkan**：D3D12 的 RT shader 必须另写一套 HLSL（`SPIRV-Cross` 不产 RT shader、DXC profile 最高 6.0），工作量近乎翻倍。先用 Vulkan 拿到真实画面、评估画质收益，再决定要不要投 D3D12 那 12–18 人日。

**P3 与 P1/P2 无耦合，可并行**。但它的着色器迁移是全项目最大单项，所以骨架要先立。

---

## P0 — 补齐地基：运动矢量、上一帧矩阵、jitter

**规模：4–6 人日** ｜ 依赖：无 ｜ 风险：低

### P0.1 上一帧矩阵链（1.5 人日）

**要解决的**：相机与物体的"上一帧"状态都不存在。

现状（实测）：
- `engine/components/transform.h` —— **无任何历史矩阵成员**，只有 `position/rotation/scale` 与 `local_matrix()/world_matrix()`
- `render_forward_clustered.cpp:1004` —— `math::Matrix4f prev_world = world; // 暂用当前矩阵充当上一帧`，**物体运动矢量实际是 0**
- `render_forward_clustered.cpp:562` —— `prev_mvp = view_proj * elem.prev_transform`，用的是**当前** view_proj，**相机运动丢失**
- 全仓 `prev_view` / `prev_camera` / `prev_view_proj` —— **0 命中**

**做法**：

1. `engine/components/transform.h`：加 `math::Matrix4f prev_local_matrix_;` + `bool has_prev_ = false;`，提供 `store_prev_matrix()`（每帧末调用一次）与 `prev_world_matrix(parent_prev)`。注意 `local_matrix()` 是**每次调用现算**的（`transform.h:40-44`），所以历史必须**显式存快照**而不是重算。
2. `engine/render/renderer_rd/forward_clustered/render_forward_clustered.h`：`RenderElement` 已有 `prev_transform`（`:74`），语义从"占位"改为"真实上一帧世界矩阵"；`RenderForwardClustered` 加 `math::Matrix4f prev_view_proj_;`。
3. 相机侧：`math::Camera`（`engine/math/camera.h`）加 `Matrix4f prev_view_matrix_` / `prev_projection_matrix_` + `store_prev_matrices()`。**注意跨线程**：`math::Camera` 注释写明"主线程更新位置/旋转，渲染线程使用 view/projection 矩阵" —— 快照必须在**主线程侧**（camera 组件更新处）取，不能在渲染线程回读。
4. 渲染线程侧：`RenderForwardClustered` 拿当前帧 `view_proj` 时，同时用 `prev_view_proj_` 做上一帧 → `_render_motion_vectors`（`:540`）改传两个 MVP。
5. **世界矩阵传播**：`transform.h:47` 的 `world_matrix(parent_world)` 已有 parent 参数，但 `render_forward_clustered.cpp:1001` 的注释明说"简化：暂用本地矩阵，父级传播后续补齐"。**上一帧矩阵要正确，父子传播必须同时补**（否则子物体上一帧位置全错）→ 这一项要连带修 `render_forward_clustered.cpp:998-1001` 与 `:1035-1038` 两处。

**验收**：
- 新增 `--dump-motion-vectors` 调试输出（`test/renderer_smoke.cpp` 现有 `--debug-view` 体系旁挂）：输出当前帧 MV 贴图的 min/max/mean
- 静止相机 + 移动物体：MV 非零且方向正确
- 移动相机 + 静止物体：MV 非零（**当前是 0，这是回归判据**）
- 父子层级：子物体随父运动的 MV 正确

### P0.2 合并运动矢量（1.5 人日）

**要解决的**：两套 MV 各缺一半、格式不一致（套 A RG16F / 套 B RGBA16F），且套 A（`shaders/motion_vectors.frag`）**只有加载者**是 `renderer_rd/effects/motion_blur.cpp:28`，与主路径完全脱节。

**做法**：
1. **统一到主路径**：`shaders/forward_clustered/scene_motion_vectors.{vert,frag}` 改为主路径唯一 MV 源，输出改为**像素空间**（DLSS 要求：`mvecScale` 随空间而变，像素空间则 `mvecScale = {1/w, 1/h}`；NDC 空间则 `{1,1}`）。当前 frag 输出的是 `(cur_ndc - prev_ndc) * 0.5`（半 NDC、映射到 [0,1]），**这不是 DLSS 认可的任一空间**，必须改。
   - 顺带删掉 `clamp(motion, -0.1, 0.1)` —— 这个钳制是为 TAA 防越界加的，**会砍掉快速运动的真实矢量**，DLSS 会因此产生拖影。要保留就得只对 TAA 用途钳制、对 DLSS 用途给未钳制版本。
2. `shaders/motion_vectors.frag`（套 A，深度重建）**保留但不再作为 DLSS 输入** —— 它的用途是 TAA/运动模糊的相机运动代理；改名为 `motion_vectors_camera.frag` 之类明确语义，避免以后再被误接。
3. `motion_blur.cpp:28` 的加载点改指新目标，或明确标记为仅 TAA 用。
4. 格式统一为 RG16F（DLSS 认这个；当前套 B 是 RGBA16F，浪费带宽且 DLSS 不按 RGBA 读）。

**验收**：
- MV 贴图在**纯相机旋转**、**纯相机平移**、**纯物体平移**、**纯物体自转**、**物体+相机同时动**五种场景下的数值符合解析预期
- 快速运动（>0.1 NDC/帧）不被错误钳制
- `motion_blur.cpp` 的既有行为不退化（motion blur 视觉 A/B 截图）

### P0.3 jitter 与曝光基础设施（1 人日）

**现状**：`FSR2_RD::get_jitter()`（`renderer_rd/effects/fsr2.cpp:79-87`）已有 Halton 2/3 的 8 项序列（与 Godot TAA 一致），但它是 **FSR2 的静态成员函数**，且 **FSR2 整体是个空壳** —— `render()` 的 EASU/RCAS 全部被注释掉（`fsr2.cpp:63-77`），`fsr2_easu_shader_` / `fsr2_rcas_shader_` 从未被加载。管线里 `fsr2_enabled` 分支（`render_forward_clustered.cpp:932`）能走到，但 `render()` 直接 return（`fsr2.cpp:61` 的 `!initialized_` 或 `fsr2_enabled == 0`）。

**做法**：
1. 把 jitter 抽到**管线级**（不属 FSR2）：`RenderForwardClustered` 持有 `frame_index_`（已有），提供 `get_frame_jitter(w, h, &jx, &jy)`，Halton 序列扩到 **16 项**（8 项在 4K 下会重复感明显）。
2. **jitter 必须真正应用到投影矩阵** —— 当前 FSR2 的 jitter 只是算出来传给一个空实现，**没有进过投影矩阵**。DLSS 的 jitter 是**像素空间**偏移且需与相机投影同步，缺了这步 DLSS 会持续闪烁。这是 P0 里最容易被漏掉、但直接决定 DLSS 成败的一点。
3. 曝光：`_render_auto_exposure()`（`render_forward_clustered.cpp:921`）已产出 `exposure_tex_`，但 FSR2 分支里有个可疑写法（`:939`）：`exposure_tex_` 无效时**回退到 `fb_.color_tex`**（拿颜色当曝光贴图喂进超分）。这个 fallback 要清掉 —— DLSS 需要的是**正确的曝光值**，不是颜色纹理。
4. **camera reset flag**：NVIDIA 清单第 9 条要求场景切换/视角跳变时传 camera reset。本引擎有 `Camera2D` / `Camera` / 编辑器自由相机多套，相机"跳变"点在 `math::Camera::set_position/set_yaw/set_pitch/look_at`（`math/camera.h:35-37, 49`）—— 这几处都要打 reset 标记，供 P1 消费。

**验收**：
- jitter 序列连续 16 帧无重复
- **投影矩阵确实含 jitter**（用一张高频棋盘图纹理验证：静止相机下纹理不因 jitter 产生持续爬行）
- 自动曝光输出的是标量曝光贴图，不是颜色纹理

### P0.4 后处理链插桩点（0.5 人日）

**要解决的**：后处理链的顺序决定了 DLSS SR 必须插在哪。当前顺序（`render_forward_clustered.cpp:880-948`）：

```
1. SSR → 2. SSIL → 3. Bloom → 4. Bokeh DOF → 5. AutoExposure
→ 6. FSR2 → 7. TAA → 8. ToneMap
```

DLSS SR 的官方要求是**尽可能靠后处理链早期接入**（在 Bloom 之前，否则 Bloom 会在低分辨率上做再上采样，产生光晕伪影）。而当前 UI 是**独立的 overlay pass**（`begin_overlay_pass` / `end_overlay_pass`，`render2d.h:219-222`），天然在最后 —— **UI 不会经过超分**，NVIDIA 清单里"HUD 走不超分路径"这条**天然满足**，省事。

**做法**：
- 在管线里预留明确的插入位：`tonemap 前 / bloom 前` 两个候选锚点，加注释说明 DLSS SR 应插在 bloom 前。
- 同时确认 `fb_.color_tex` 是 HDR（RGBA16F）—— DLSS SR 支持 HDR 输入，但引擎 `ToneMap` 在最后，**DLSS SR 若插在 tonemap 前则输入是 HDR，需要确认 Streamline 的 HDR 模式配置**。这是 P1 的一个决策点。

**验收**：无（纯插桩）。

### P0 出口条件

- [ ] 相机运动的 MV 非零且正确（当前为 0）
- [ ] 物体运动的 MV 真实（当前恒为 0）
- [ ] MV 空间与格式符合 DLSS 约定（像素空间 RG16F）
- [ ] jitter 进到了投影矩阵
- [ ] 曝光贴图不是颜色纹理
- [ ] 桌面三后端（GL / Vulkan / D3D12）截图逐像素无回归
- [ ] `ctest -j1` 54/54 全绿

---

## P1 — Streamline 骨架 + DLSS SR + Reflex（仅 D3D12）

**规模：19–30 人日** ｜ 依赖：P0 ｜ 风险：中（工具链 + 授权物）

### P1.1 Streamline 集成骨架（8–12 人日）

**要解决的**：
- Streamline 二进制**不入 Git**（自 SL 2.7.32 起，DLSS 功能 DLL 需从 GitHub Releases 下载 zip，需登录 NVIDIA 开发者账号）
- 本仓库 D3D12 走 **MinGW + 全运行时 LoadLibrary、零 MSVC 导入库**（`d3d12_loader.h:1-17`），历史上踩过 3 次 MinGW 链 MSVC 库失败（`d3d12_dxc.h:6-7`、`d3d12_backend.cpp:334-338`、`engine/CMakeLists.txt:436-437`）→ Streamline 必须走同样的 LoadLibrary 路线
- Streamline 需要 `slInit` / `slSetFeature` / `slEvaluateFeature` / `slSetTag` / `slReflexSleep` / `slGetNewFrameToken`，且**要求 appId**（否则 DLSS 的 OTA 更新与未来硬件支持失效 —— NVIDIA 清单第 1 条）

**做法**：
1. 新增 `engine/render/d3d12/d3d12_streamline.h/.cpp`：
   - **LoadLibrary 封装** `sl.*.dll`（`sl.dlss.dll` 等）与 `nvngx_dlss.dll`，本地声明所需 C++ API 的最小子集（照 `d3d12_loader.h` 的既有范式）
   - `StreamlineContext` 单例：init / shutdown / feature 查询（`slGetFeatureFlags` / `slEvaluateFeature`）
   - **运行时缺失即降级**：`slInit` 失败 → 特性全部关闭，不崩、不影响既有渲染（这是硬要求 —— 发行版可能不带 SL）
2. **构建期产物获取**：
   - 新增 CMake option `GRYCE_ENABLE_STREAMLINE`（默认 OFF）
   - 提供 `cmake/streamline/` 脚本：从 Releases 下载 zip → SHA 校验 → 解压到 `third_party/streamline/`（**不入 Git**，加 `.gitignore`）
   - 环境变量 `GRYCE_STREAMLINE_ROOT` 指向已有目录（CI / 本地开发）
   - **未提供时**：编译期 `GRYCE_HAS_STREAMLINE=0`，源码 `#ifdef` 桩化 —— 与 `GRYCE_HAS_VULKAN`（`engine/CMakeLists.txt:287`）的既有范式一致
3. **纹理/资源 tagging**：Streamline 要求把 color / depth / MV / exposure 资源 tag 给它（`slSetTag`），D3D12 侧要拿到 `ID3D12Resource*`。
   - **障碍**：`IRenderBackend` 与 RHI 句柄（`render/rhi_handle.h`）是**不透明整数**，后端内部才持有资源指针。D3D12 后端内部有 `copy_texture_to_buffer()`（`d3d12_backend.cpp:2600`）这类能拿到原生资源的地方 → 需要在 D3D12 后端内部开一个"取原生资源指针"的通道，或让 tagging 直接发生在后端内部。
   - 这也意味着 **Streamline 集成天然只能落在 D3D12 后端**（与"DLSS 官方不支持 Vulkan"一致）。
4. **DXGI Factory 代理**：`sl.interposer.dll` 会 hook DXGI factory v-table；Streamline FAQ 提到可选 `sl::PreferenceFlags::eUseDXGIFactoryProxy`。D3D12 后端当前的 swapchain 创建路径（`d3d12_swapchain.cpp`）需要检查是否需要走代理。

**验收**：
- 未安装 SL 时，引擎行为与改动前**逐像素一致**（`--api dx12` 截图 A/B）
- 安装 SL 后，`slInit` 成功、`slGetFeatureFlags` 正确报告 DLSS 可用性
- SL 缺失/版本不匹配/初始化失败 → 特性关闭且有明确日志，不崩

### P1.2 DLSS 超分辨率（6–10 人日）

**要解决的**（NVIDIA 官方清单逐条对应）：
| 清单条目 | 本引擎现状 | 做法 |
|---|---|---|
| 1. 专用 appId | 无 | 配置化 appId，随发行版区分 |
| 2. **SR 尽可能早于后处理接入** | 链序：SSR→SSIL→Bloom→DOF→Exposure→FSR2→TAA→Tonemap | 插在 **Bloom 之前**（P0.4 已留锚点） |
| 3. mip-map bias | 材质未接 | 材质系统加 bias 开关，DLSS 开启时设偏置 |
| 4. **MV 准确** | P0 已解决 | 传像素空间 MV + 正确 `mvecScale` |
| 5. 静态场景 resolve + 兼容 jitter | P0.3 已解决（但 jitter 必须真进投影） | — |
| 6. **每帧送曝光值** | P0.3 已修（清掉颜色纹理 fallback） | — |
| 7. 模式可查询、可用户选择 | 引擎有 ImGui 调试面板的渲染开关体系 | 加 DLSS 模式枚举（关闭/质量/平衡/性能） |
| 8. 发行版用**非水印** `nvngx_dlss.dll` | — | 构建脚本区分 dev/production |
| 9. **camera reset flag** | P0.3 已打标记 | 接入 |
| 10. 不用时清理 NGX 资源 | — | `slFreeResources` + shutdown 路径 |

**额外要注意**：
- **DLSS 输出不能直接进 Bloom**（链序已在 Bloom 前，天然规避）
- **TAA 与 DLSS 的关系**：TAA 在链上位于 FSR2 之后。若同时开 TAA + DLSS SR，两者都会做时域累积 → 需要明确互斥或顺序（建议：开 DLSS SR 时关 TAA，或反过来）
- **D3D12 的 present 目标 + 帧末 blit 结构**（`d3d12_backend.cpp:483-551` 造 present target、`:726-814` 帧末翻转 blit）：颜色**不直接写 back buffer**。DLSS 通常要求直接在 swapchain 目标上操作 → 需调整为"DLSS 输出 → present 目标"，让 present blit 搬 DLSS 的结果。这是接线时最可能踩的坑。

**验收**：
- 1080p → 4K，DLSS 开/关两态截图对比
- 静止场景 100 帧无闪烁（jitter 正确性的端到端验证）
- 相机快速移动时无明显鬼影（MV 正确性的端到端验证）
- `--api dx12` 与 `--api opengl` 在**都关闭 DLSS** 时逐像素一致（回归）

### P1.3 Reflex（5–8 人日）

**要解决的**：Reflex 需要在 present 前后插 latency marker + `slReflexSleep`。好消息是引擎**已有可复用的 GPU 分段计时**：
- D3D12：`d3d12_backend.cpp:2834+`（`ID3D12QueryHeap(TIMESTAMP)`，`GetTimestampFrequency` 在 `:2848`）
- Vulkan：`vk_backend.cpp:388+`（`VkQueryPool` + `vkCmdWriteTimestamp`）

**做法**：
1. D3D12 后端加 4 个 marker 点：`eSimulationStart/End`、`eRenderSubmitStart/End`、`ePresentStart/End`。
   - `eSimulationStart` 在游戏逻辑更新处（**主线程**，不是渲染线程）
   - `eRenderSubmitStart/End` 在命令提交处
   - `ePresentStart/End` 包围 `Present`（`d3d12_swapchain.cpp:296`）
2. `slReflexSleep` 放在 `eSimulationStart` 之前的模拟阶段
3. `slGetNewFrameToken` 在帧开始取
4. Reflex 模式可配（关 / 开 / 开+boost）

**注意**：Reflex 是**跨厂商**的（Streamline 官方 FAQ 明确 NIS / NRD / Reflex 支持非 NVIDIA 硬件），所以它不依赖 DLSS 插件，可独立于 DLSS 开关。

**验收**：
- 端到端延迟可测量下降（需外部工具或自测：输入时间戳 → present 时间戳）
- Reflex 开/关不影响画面（**纯延迟特性，画面应逐像素一致** → 这是好判据）
- 三个后端行为一致（Vulkan 侧 Streamline 不支持 Vulkan → Vulkan 上 Reflex 也用不了，需在 UI 里如实禁用）

### P1 出口条件

- [ ] 未装 Streamline 时桌面三后端逐像素无回归
- [ ] DLSS SR 可开关，模式可枚举
- [ ] 静止/运动场景无闪烁与鬼影
- [ ] Reflex 开关不影响画面
- [ ] Streamline 缺失时明确降级 + 日志
- [ ] `ctest -j1` 54/54 全绿

---

## P2 — 硬件光追：先只做 Vulkan ray query 反射

**规模：18–28 人日**（不含 D3D12）｜ 依赖：P0（P2.1 可并行）｜ 风险：中

### P2.1 加速结构构建（6–8 人日）

**正面资产**：**LOD 是真几何简化**（`engine/assets/mesh_lod.{h,cpp}`，757 行，顶点聚类，误差有界，6 级，**导入期一次性生成**）→ LOD level 1–2 天然就是 BLAS 的理想输入（低模 + 有界误差 + 现成顶点索引）。这把 AS 构建从"要自己简化几何"降级成"挑现成的低模"。

**障碍**：`IMesh`（`engine/render/mesh.h:59-90`）**不暴露 GPU 资源句柄**，也**不暴露 CPU 侧顶点/索引数据** → 需新增 getter。而 `LodChain` / `ChunkedLod` 存的是 `shared_ptr<const MeshData>`（`mesh_lod.h:45, 118`），CPU 侧数据是有的，只是没暴露。

**做法**：
1. `IMesh` 新增 `const MeshData* cpu_mesh_data() const`（或等价 getter），暴露顶点/索引数组
2. 新增 `engine/render/vulkan/vk_acceleration_structure.{h,cpp}`：
   - BLAS 构建：走 `VK_KHR_acceleration_structure` + `VK_KHR_deferred_host_operations`（用 `vkCmdBuildAccelerationStructuresKHR` 异步，避免同步卡顿）
   - TLAS 构建：每 mesh 一个 BLAS + 实例变换 → 每帧更新 instance buffer
   - **几何来源**：用 LOD level 1（或按屏幕距离动态选级）
3. `vk_device.cpp:318` 加扩展探测（`VK_KHR_acceleration_structure` / `VK_KHR_ray_query` / `VK_KHR_deferred_host_operations`），`vk_device.cpp:272-280` 的硬编码字符串比对逻辑要改造成可扩展形式
4. `vk_instance.cpp:62` 的 `VK_API_VERSION_1_2` 可能要升 1.3
5. **IRenderBackend 扩展**（`render.h`）：按既有"默认空实现 + capabilities 声明"范式（compute 就是这么加的，`render.h:134-197`）加 5 个虚函数 + 2 个 capabilities 字段。GL 后端留空即可。

**验收**：
- Vulkan 设备支持时能建出 BLAS/TLAS，无 validation 错误
- 不支持时（老驱动 / 无 RT 硬件）明确降级 + 日志
- AS 显存占用可接受（每 mesh BLAS 尺寸统计）

### P2.2 ray query 管线与反射 pass（8–12 人日）

**做法**：
1. **着色器**：GLSL 450 + `GL_EXT_ray_query`，可复用现有 shaderc 编译链（`glsl_compiler.cpp:226`），并落进既有 SPIR-V 三级缓存（`vk_spirv_cache.cpp:80-120`，路径 `spirv/vulkan_<name>.<stage>.spv`）
   - 新增 shader 变体：`vulkan_rt_trace.comp`（dispatch）+ `vulkan_rt_reflect.frag`（写回）
   - **注意 `d3d12_vulkanize.cpp:728-735` 不处理 `#extension`** —— 这条链路是 D3D12 用的，Vulkan 侧不走它，但若将来 D3D12 也做光追，这里是坑
   - **注意 `GRYCE_HAS_*` 宏纪律**：D3D12 的 vulkanize 会把源码里**所有** `#define NAME 值` 重放到文件开头 → 同名宏出现两个替换值会让 shaderc 报 `Macro redefined`（见 `docs/zh/shader-conventions.md` §2 与 `docs/zh/webgl-backend.md` §9 末条）。新增的光追 shader 若有后端差异开关，必须写 `#ifndef F / #define F 1 / #endif`
2. **C++ 侧**：TLAS 绑定到 descriptor set、dispatch 封装、结果写入 G-Buffer 或独立 RT
3. **管线编排**（`render_pipeline.h`，1549 行 / 77 个 RHI 成员）：RT 反射与 SSR **并存可切换**，RT 可用时优先、不可用时回退 SSR
   - 两者输入同源（深度 + 法线 + 粗糙度）→ 降级路径天然具备
4. **降噪**：光追反射噪声大。首选 **Streamline 的 NRD**（跨厂商、免费、专门做 RT 降噪）；若不想再引入 SL 依赖，则需自研时域降噪（**额外 8–10 人日**，建议走 NRD 路线）

**验收**：
- 镜面材质上 RT 反射与 SSR 反射视觉可对比，RT 正确（反射物体而非环境）
- RT 不可用时自动回退 SSR，画面无突变
- Vulkan 逐像素回归无变化（未开 RT 时）

### P2.3 评估与决策点（1–2 人日）

**这是一个明确的决策闸门。** 做完 P2.2 后评估：
- RT 反射相对 SSR 的**画质收益是否值得**（屏幕空间反射的固有限制：出屏即失效）
- 若收益显著 → 投 D3D12 那 12–18 人日（另写一套 HLSL RT shader + 新编译路径）
- 若收益一般 → **停在 Vulkan**，D3D12 光追不进计划

### P2 出口条件

- [ ] Vulkan RT 反射可用且正确
- [ ] 无 RT 硬件时明确降级
- [ ] Vulkan 桌面回归无变化
- [ ] 决策点结论记录到 `docs/zh/plans/`

---

## P3 — WebGPU：骨架到可出画面

**规模：38–57 人日**（不含 WGSL 迁移）｜ 依赖：无（可与 P1/P2 并行）｜ 风险：高

### P3.1 探针与骨架（8–12 人日）

**关键前提核实（好消息）**：Emscripten ≥ 4.0.18 起 WebGPU 走 **`--use-port=emdawnwebgpu`**（Dawn 的 C API 绑定，实现 `webgpu.h` 并映射到浏览器 JS WebGPU API）。这是 Emscripten **内置的 remote port**，自动下载 pinned 版本 —— **只需一个 flag**：

```cmake
target_compile_options(target PUBLIC "--use-port=emdawnwebgpu")
target_link_options(target PUBLIC "--use-port=emdawnwebgpu")
```

需要 `--closure=1` 压体积。`webgpu.h`（webgpu-native 标准）算稳定；`webgpu_cpp.h` 与 Dawn 特有部分不稳定。ImGui 的 `imgui_impl_wgpu.cpp` **已在 `third_party/imgui/backends/`**（成本低）。

**做法**：
1. **探针先行**：新建 `test/webgpu_probe/`（照 `test/webgl_probe/` 的结构），先在浏览器里跑通 adapter/device/surface/纹理/管线，把 Dawn API 的实际行为摸清
2. `engine/CMakeLists.txt` 加 `GRYCE_ENABLE_WEBGPU` option；`EMSCRIPTEN` 分支加 port flag
3. `engine/render/webgpu/` 新目录：`webgpu_backend.{h,cpp}` 实现 `IRenderBackend`
   - **注意**：Dawn 的 `webgpu.h` 是 **C API**，而 `IRenderBackend` 是 C++ 类 → 需要一层 C++ 封装（`webgpu_cpp.h` 可用，但不稳定 → 建议自己包一层薄封装）
4. **平台层**（`engine/platform/window.cpp`）：
   - `window.cpp:303-311` 的 GLFW 上下文创建（`GLFW_CLIENT_API, GLFW_OPENGL_ES_API`）→ **Emscripten 的 GLFW 端口（内置与 contrib.glfw3）都没有 WebGPU 支持** → 改走 NoApi 路径（与 Vulkan/D3D12 同类）
   - `window.cpp:73-212` 的 canvas 适配（140 行 EM_JS + 浏览器视口逻辑）→ WebGPU 的 surface 从 DOM canvas 取（`getContext('webgpu')`），且需显式 `configure()`，尺寸语义与 `glfwSetWindowSize` 那套不同
   - **GLFW 的窗口与输入层可保留**（pointer lock、WASD、Esc 释放鼠标那套逻辑可复用）
5. `engine/api/render_api.cpp:1218-1224` 的 "WebGL 强制 sync_mode=1" → WebGPU 同样需要（device 与主线程绑定）

**验收**：
- 浏览器里跑通一个三角形（探针）
- `file://` 与 `http://localhost` 行为差异明确记录（`navigator.gpu` 在非 secure context 下**静默 undefined**，不报错）

### P3.2 wgpu 后端核心（30–45 人日）

**做法**：
1. device / queue / surface 初始化与能力查询
2. swapchain（surface texture → present）
3. 渲染目标：depth、HDR color、**G-Buffer 4 个 RT**（`postprocess_pass.cpp:1005-1065`）
4. pipeline 缓存（PSO 编译是异步的，需与首帧时序协调 —— 现有 `IShader` 是同步 load 的，WebGPU 要改）
5. **bind group 框架**（这是最大的架构改动）
6. 命令编码与提交

**必须正视的 bind group 化改造**（这是 P3 最大的隐藏工作量，报告里已强调）：

GL 的模型是"逐纹理散装绑定"（`set_texture` + `set_int` 配对），WebGPU 的模型是"**按 pass 分组的不可变资源集**"（bind group + 编译期固定 `@group/@binding`）。改造会侵入：
- `engine/render/render_pipeline.cpp`（**全仓最大的 .cpp**，318 KB）
- `engine/render/render_context.cpp`（48 KB）
- `engine/render/render_thread.cpp`（21 KB）

且 **bind group 绑定 pipeline layout，资源重建时 bind group 必须失效重建** → 需要缓存与失效策略。

**另外 15 处 GL 调用泄漏在 `opengl/` 目录之外** —— `engine/render/renderer2d_impl.cpp:2033-2066`（shadow 贴图 compare mode、`glDrawBuffer(GL_NONE)`、1×1 白贴图、`glDeleteTextures`、`glBindTextureUnit`）。迁移时必须一并处理，否则会静默出错。

**验收**：
- 一个完整 pass（几何 + 纹理 + uniform）跑通
- 资源重建（resize / 热重载）后画面正确

### P3.3 验证链改造（8–12 人日）

**必须改的两处**（照 `test/webgl_probe/run_headless.sh`）：

1. **`file://` 下 `navigator.gpu` 静默 undefined** → WebGPU 要求 secure context（HTTPS 或 localhost）。改 `http://localhost:<port>`，或加 `--unsafely-treat-insecure-origin-as-secure=file://`
2. **`--disable-gpu` 会阻止 adapter 发现**（`requestAdapter()` 返回 null）→ 去掉，改 `--enable-unsafe-webgpu` + SwiftShader 路由
3. **headless Chrome 的 WebGPU canvas presentation 永远到不了合成器**（上游限制，无 flag 组合可修）→ `--screenshot` **会拿到黑图**
   - **应对**：改用**像素回读**（`copyTextureToBuffer` + `mapAsync`），这恰好与引擎现有 `capture_frame_rgba()` 机制天然对应（GL 版用 `glReadPixels`，`gl_backend.cpp:851-881`）

**验证强度反而会提升**：WebGL2 因 **16 纹理单元上限**（真实 GPU，ANGLE/D3D11 上报；SwiftShader 上报 32 所以软件光栅永不暴露）被迫把 `GRYCE_HAS_PCSS / _VSM / _GI / _POINT_SHADOW` 注入 0（`gl_shader.cpp:171-190`），砍掉了 4 个特性；WebGPU 无此限制 → A/B 可做**全 9 通道无删减**对比。

**验收**：
- headless 下通过像素回读拿到与桌面 GL 一致的画面
- 9 个 debug 通道全部对齐（WebGL 时代只有 4/9 逐字节相同）

### P3 出口条件

- [ ] 浏览器里跑出与桌面 GL 视觉一致的完整场景
- [ ] headless 像素回读验证链可用
- [ ] 桌面三后端零回归

---

## P4 — WGSL 着色器迁移（工程最大头）

**规模：60–90 人日** ｜ 依赖：P3.2 ｜ 风险：高

### P4.1 不可自动化，只能手写

`engine/render/shaders/` 共 **214 个文件 / 12,475 行**：106 个逻辑着色器（GL 基名 46 + `vulkan_` 变体 55 + 独有 9），`.vert` 92 / `.frag` 96 / `.comp` 2。

**naga / tint 都不能真正省事**：
- **naga** 的 GLSL 前端**只吃 GLSL 440+ 且 Vulkan 语义** → 引擎 GL 侧是 101 个 `#version 330 core` + 6 个 `#version 430 core`，必须先升到 Vulkan 语义 = 等于**再写 102 个变体**的规模。且 naga 的 WGSL **后端**是次级支持、已知有 bug。
- **tint** **没有 GLSL 输入前端**（只有 GLSL writer / validator）。
- 真实可行的自动化路径是 **GLSL → 现有 SPIR-V 编译（`glsl_compiler.cpp:226`，shaderc 运行时加载）→ SPIR-V→WGSL 转换器 → 人工逐个修正**。SPIR-V 是强类型 IR，比文本转换可靠。但**它省的是"手抄语法"的时间，不是"设计资源布局"的时间**，且前置的 Vulkan 语义化改造使总工作量反而更大。

### P4.2 真正的语义级障碍（必须逐个处理）

| GLSL | WGSL | 陷阱 |
|---|---|---|
| `layout(location=N) uniform sampler2D uTex;` | `@group(0) @binding(3) var uTex: texture_2d<f32>;` + **独立** `var uTexS: sampler;` | 一个 GLSL uniform → **两个** WGSL 资源 |
| `texture(uTex, uv)` | `textureSample(t, s, uv)` | **仅在 uniform control flow 中合法** —— 引擎 PBR/SSR/GTAO 里大量基于屏幕坐标的动态分支（阴影早退 `proj.x<0\|\|proj.x>1`、深度早退）在 GLSL 合法、**在 WGSL 是硬错误** → 必须改 `textureSampleLevel` 或重构控制流。**这是 P4 最大的工作量来源** |
| `gl_FragCoord`（原点左下） | `@builtin(position)`（原点左上） | 全局替换 + 语义翻转 |
| 裁剪空间 z ∈ [-1,1] | z ∈ [0,1] | **投影矩阵要改 z 行**，不是 shader 能改的 |
| `mod(x,y)` = floor | `%` = trunc（符号跟 x） | **静默出错** |
| `c ? a : b` | `select(falseVal, trueVal, c)` | **参数顺序反转**，静默出错 |
| `#define GRYCE_HAS_PCSS 0` | 无预处理器 | **现有的后端特性开关机制在 WGSL 里不存在** → 需重新设计（生成时注入 or 拆文件） |
| `uniform float uExposure = 1.0;` | `var<uniform> uExposure: f32;` + CPU 侧写 | 引擎有 78 处 uniform 带初始化器 |

### P4.3 做法

1. **先做 4 个大头**（占总量约 22% 但难度远高于行数占比）：
   `pbr.frag`（800 行）、`instanced.frag`（775 行）、`skinned_pbr.frag`（~700 行）、`deferred_lighting.frag`（~500 行）
2. **照抄已有 102 个 `vulkan_` 变体的 binding 号** —— Vulkan 的 `layout(set=0, binding=N)` → WGSL 的 `@group(0) @binding(N)`。vulkan_ 变体已经把 binding 语义梳理清楚了，**这是最大的省力点**
3. **逐 pass 迁移**：几何 → 阴影 → 后处理 → 2D → ImGui
4. **`textureSample` 的 uniform control flow**：优先策略 —— 能用 `textureSampleLevel` 就用（显式 LOD）；不能则把早退改成"分支内改用 level 0 的 `textureSampleLevel`"
5. **投影矩阵 z 翻转**：在 C++ 侧（投影矩阵构造）统一处理，不在 shader 里

### P4.4 建议顺序

| 批次 | 内容 | 人日 |
|---|---|---|
| W1 | 骨架 shader（skybox / hdr_copy / tonemap）+ 简单全屏 pass | 8–12 |
| W2 | `pbr` / `instanced` / `skinned_pbr`（**最难批次**） | 25–35 |
| W3 | 阴影族（shadow_map / shadow_vsm / shadow_esm / point_shadow_map / shadow_atlas） | 10–15 |
| W4 | 后处理族（bloom / TAA / SSR / GTAO / SSIL / FSR2 / 水面 / 体积光） | 12–18 |
| W5 | 2D 管线 + ImGui（`imgui_impl_wgpu.cpp` 已在 third_party） | 5–10 |
| | **合计** | **60–90** |

---

## P5 — 收尾：帧生成、D3D12 光追、WebGL 下线

**这三项都是"看前面结果再决定"的，不进基线计划。**

### P5.1 DLSS 帧生成（10–15 人日，仅 D3D12）

**依赖**：P0 全部 + P1.1/P1.2。

**额外约束**：
- **DLSS-G 需 Win10 2004 (19041) 以上 + 硬件加速 GPU 调度（HAGS）** —— 不满足则不可用（NVIDIA 官方明说）
- **Streamline 官方不支持 Vulkan** → 只做 D3D12
- 需要 jitter 与 MV 达到 **FG 级别精度**（比 SR 更严），P0 的实现若只满足 SR，FG 可能仍需返工
- **HUD 走不超分路径** —— 本引擎 UI 是独立 overlay pass，天然满足

### P5.2 D3D12 硬件光追（12–18 人日）

**依赖**：P2 的评估结论为"值得做"。

**必须新写的**（无法复用 Vulkan 的 GLSL）：
1. **独立的 HLSL RT 编译路径** —— 绕开 `vulkanize_glsl()` → SPIRV-Cross → DXC 三跳。`SPIRV-Cross` 的 HLSL 后端写死 SM 50（`d3d12_spirv_cross.cpp:246-247`）且**本身不生成 DXR 光追 shader**（无 `lib_6_3` target、无 `RayDesc`/`TraceRay` 内建映射）；DXC profile 枚举最高只到 6.0（`d3d12_dxc.h:28-35`，映射表 `d3d12_dxc.cpp:235-245`，`build_stage` 在 `d3d12_shader.cpp:335-337` 硬编码）→ RayQuery/TraceRay 需 SM 6.5+，光追管线需 `lib_6_3+`
2. DXC 参数表（`d3d12_dxc.cpp:292-297`）要加 ray tracing 相关开关
3. `ID3D12Device` → `ID3D12Device5/6`（拿 `CreateRaytracingAccelerationStructure`）
4. `D3D12_FEATURE_D3D12_OPTIONS5` / `D3D12_RAYTRACING_SUPPORT` 特性检测
5. RT root signature（`D3D12_ROOT_SIGNATURE_FLAG_RAYTRACING_SHADER`）+ RTPSO 构建
6. HLSL 版 RT shader 一套（8–14 个文件）

### P5.3 WebGL 下线（3–5 人日）

**依赖**：P3 + P4 全部完成且验证通过。

**建议不要一步到位替换**：即使目标是替换，工程上也应「**新增 WebGPU → 两条并存做 A/B → 验证通过后再下线 WebGL**」。理由：**WebGL 那条 A/B 对比链（桌面 GL vs WebGL 逐 debug 通道对比，4/9 通道逐字节相同、其余 0.1/255）是目前唯一的跨后端正确性基准**。直接替换会把这个基准一起拆掉，出问题无法定位。

下线时要清理的：
- `engine/render/opengl/gl_webgl_shim.h`（734 行）
- `engine/render/opengl/gl_shader.cpp` 的 `to_glsl_es300()`（181 行）
- `engine/render/opengl/gl_slot_map.h` 的 16 单元分档
- `engine/render/shaders/webgl_instanced.vert`
- `cmake/web/GL/glew.h`（12 行替身）
- `CMakeLists.txt` / `engine/CMakeLists.txt` 的 EMSCRIPTEN 分支
- `test/webgl_probe/` / `test/webgl_demo/`
- `docs/zh/webgl-backend.md`（423 行实施记录）

**注意**：这些清理**必须最后做** —— shim 与 ES300 转换器是 WebGL 独有的，但 `gl_slot_map.h` 与 `gl_shader.cpp` 是**桌面 GL 也在用的**，不能整文件删。

---

## 全局里程碑与验收门禁

| 里程碑 | 累计人日 | 关键交付 | 门禁 |
|---|---|---|---|
| **M0 地基补齐** | 4–6 | 正确运动矢量 + 上一帧矩阵 + jitter 进投影 | 桌面三后端逐像素无回归；ctest 54/54 |
| **M1 DLSS SR + Reflex** | +19–30 | D3D12 上可开可关的 DLSS SR + Reflex | 关 DLSS 时 dx12 vs opengl 逐像素一致 |
| **M2 光追首个画面** | +18–28 | Vulkan ray query 反射 + 决策结论 | Vulkan 回归无变化；无 RT 硬件时降级 |
| **M3 WebGPU 出画面** | +38–57 | 浏览器里跑出与桌面 GL 一致的场景 | headless 像素回读验证链可用 |
| **M4 WebGPU 全量** | +60–90 | 106 个 WGSL 着色器 | 9 debug 通道全对齐 |
| **M5 可选增强** | +25–38 | DLSS FG / D3D12 光追 / WebGL 下线 | 逐项独立验收 |

**全程通用门禁**（每阶段都必须过）：
- `ctest -j1` **54/54 全绿**（grycegc 并行会互相干扰，必须 `-j1`）
- `--api opengl` / `--api vulkan` / `--api dx12` 三后端截图逐像素一致（未开新特性时）
- 新增 shader 必须补 `vulkan_*` 变体
- shader 里同一宏全仓只允许一处定义（按后端变的开关写 `#ifndef F / #define F 1 / #endif`）
- 每个 `.cpp` 带模块头注释，注释不写调试残留

---

## 关键风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| **P0 的 jitter 改投影矩阵可能影响 TAA**（TAA 也在链上，`:945`） | TAA 画面抖动 | jitter 做成管线级统一来源，TAA 与 DLSS 共用同一序列，不各算各的 |
| **P0 改世界矩阵传播**（`render_forward_clustered.cpp:998-1001` 现用本地矩阵，父子传播未做） | 影响面比预期大（所有物体渲染） | 单独提交、单独验证；先只加历史矩阵不动传播逻辑，用测试场景确认差异 |
| **Streamline 二进制需登录 NVIDIA 账号下载** | 构建可复现性 | 下载脚本 + SHA 校验 + `GRYCE_STREAMLINE_ROOT` 环境变量；未提供时 `GRYCE_HAS_STREAMLINE=0` 桩化 |
| **MinGW 与 Streamline 的兼容性未知**（历史已踩 3 次 MinGW 链 MSVC 库失败） | P1.1 可能超期 | 一开始就按 LoadLibrary 封装，不尝试链接导入库 |
| **DLSS 输出与 D3D12 的 present target + blit 结构冲突**（`d3d12_backend.cpp:483-551`/`:726-814`） | P1.2 接线卡住 | P1.1 阶段就摸清 DLSS 目标与 present 目标的关系，别留到 P1.2 |
| **WGSL 的 `textureSample` uniform control flow 硬约束** | P4 工作量上浮 | 先做 4 个大头，评估真实返工率后再决定剩余批次的人日 |
| **bind group 化改造侵入 `render_pipeline.cpp`（全仓最大 cpp）** | 可能连带影响三个桌面后端回归 | 改造收口在后端抽象层内，不改上层绘制组织语义 |
| **headless Chrome 的 WebGPU 截图拿黑图** | 现有验证链失效 | 改像素回读（与 `capture_frame_rgba` 对应） |
| **P0/PI/P2/P3 跨阶段依赖被低估** | 整体延期 | 每阶段出口条件写死，不满足不进下一阶段 |

---

## 需要用户决策的三个点

| 决策点 | 选项 | 建议 |
|---|---|---|
| **P2.3 之后是否投 D3D12 光追** | 投（+12–18 人日）/ 不投（光追只在 Vulkan 可用） | 先看 Vulkan 画面收益再定，别预先投 |
| **P5.3 WebGL 下线的时点** | P4 完成后立刻下线 / 先并存一段时间 | 并存 —— WebGL A/B 链是唯一的跨后端正确性基准 |
| **DLSS 帧生成是否做** | 做（需 HAGS + Win10 2004+）/ 不做 | 后置到 P5，先确保 SR 质量达标 |
