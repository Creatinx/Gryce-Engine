# 调研报告：接入 NVIDIA RTX（光追 / DLSS / Reflex）与 WebGL → WebGPU

> 调研日期：2026-10-27
> 口径：基于本仓库实测代码 + 官方 SDK 现状核实，未改任何源码。
> 估算单位：**单人全职人日**（1 人日 = 8 小时有效开发）。所有区间是下界~上界，不是单点估计。

---

## 0. 结论摘要

| 事项 | 结论 | 估算 |
|---|---|---|
| **NVIDIA 硬件光追（DXR + VK RT）** | 可行，但 **D3D12 侧现有 shader 链路必须另开一条 HLSL 直编译路径**（SPIRV-Cross 不产 RT shader）。Vulkan 侧是纯增量。 | **48–73 人日**（完整：反射 + 阴影 + 降噪）<br>最小可交付（仅 Vulkan ray query 反射）**约 15 人日** |
| **DLSS SR + FG + Reflex** | 可行，最大风险是**运动矢量当前不满足 FG 要求**（相机/物体两套分开、都没有上一帧 view-proj）。另需 Streamline SDK 授权物 + MinGW 兼容封装。 | **37–58 人日**（与光追共用前置时约 **28–45 人日**） |
| **WebGL → WebGPU** | **不是移植，是一次后端重写**。WebGPU 不是 GL 的子集（bind group vs 全局状态机），兼容层路线不成立。最大单项是 106 个着色器重写为 WGSL。 | **144–219 人日**（约 6–9 人月）<br>其中着色器迁移独占 **60–90 人日** |
| **三者合计（无重叠优化）** | | **约 12–18 人月** |

**量级对照（用本仓库自己的历史当尺子）**

| 已有工程 | 新增代码 | 实际耗时 |
|---|---|---|
| D3D12 后端 | ~8,000 行 / 18 文件 | 4–6 周（计划文档记录） |
| Vulkan 后端 | ~11,700 行 / 24 文件 | 同量级 |
| WebGL 后端（增量适配） | ~1,900 行 | 10–16 工作日 |
| **WebGPU 后端（估算）** | **~8,000–12,000 行 + 106 个 WGSL** | **6–9 人月** |

WebGL 当年之所以便宜，是因为 **WebGL2 ≈ GLES 3.0 ≈ 桌面 GL 的子集**，能用 734 行 shim + 105 行文本转换器兜住；WebGPU 没有任何一层这种"子集红利"可吃。

---

## 1. NVIDIA 硬件光追

### 1.1 现状：本仓库零 RT 代码

全仓 grep 确认：`RayQueryKHR` / `GL_EXT_ray_query` / `VK_KHR_acceleration_structure` / `VK_KHR_ray_tracing_pipeline` / `D3D12_FEATURE_D3D12_OPTIONS5` / 加速结构 / `.rgen|.rchit|.rmiss|.rahit|.rint|.rcall` —— **全部 0 命中**。

`RenderBackendCapabilities`（`engine/render/render.h:30-56`）14 个字段里没有任何 RT / DLSS 相关项。

### 1.2 关键障碍：D3D12 的 shader 链路走不到 RT

现有 D3D12 shader 是四跳：

```
GLSL 330/430 core
 → vulkanize_glsl()        纯文本改写为 Vulkan-GLSL   (d3d12_vulkanize.cpp, 799 行)
 → compile_glsl_to_spirv()  shaderc → SPIR-V           (glsl_compiler.cpp:226)
 → spirv_to_hlsl()          SPIRV-Cross → HLSL          (d3d12_spirv_cross.cpp:190)
 → compile_hlsl_to_dxil()   DXC → DXIL                 (d3d12_dxc.cpp:258)
```

**三处硬墙**：

1. **DXC profile 枚举最高 SM 6.0**（`d3d12_dxc.h:28-35`，映射表 `d3d12_dxc.cpp:235-245`，`build_stage` 在 `d3d12_shader.cpp:335-337` 硬编码）。RayQuery / TraceRay 需要 **SM 6.5+**、光追管线需要 **lib_6_3+**。
2. **SPIRV-Cross 的 HLSL 后端写死 SM 50**（`d3d12_spirv_cross.cpp:246-247`），且它**本身不生成 DXR 光追 shader**（无 `lib_6_3` target、无 `RayDesc`/`TraceRay` 内建映射）。改 SM 号也救不了。
3. **vulkanize 不处理 `#extension`**（`d3d12_vulkanize.cpp:728-735`），且它把裸 `uniform` 收拢进 `Globals` uniform 块 —— 而 `rayQueryEXT` / `accelerationStructureEXT` 是 opaque 类型，塞不进 std140 块。

**结论**：D3D12 的 RT shader **无法复用 Vulkan 的 GLSL**，必须另写一套 HLSL，并新增一条独立于 `IShader::build_stage` 的编译路径。这是光追工作量的最大重复来源。

### 1.3 Vulkan 侧：纯增量，无历史包袱

需要新增：3 个 device extension（`VK_KHR_acceleration_structure` / `VK_KHR_ray_query` 或 `VK_KHR_ray_tracing_pipeline` / `VK_KHR_deferred_host_operations`）+ 改 `vk_device.cpp:272-280` 的硬编码字符串探测 + API 版本可能升 1.3。

RT shader 用 GLSL 450 + `GL_EXT_ray_query`，可复用现有 shaderc 编译链（`glsl_compiler.cpp`），并落进既有的 `spirv/vulkan_<name>.<stage>.spv` 三级缓存。

### 1.4 正面资产（显著降低工作量）

| 资产 | 位置 | 对 RT 的价值 |
|---|---|---|
| **真几何简化 LOD** | `engine/assets/mesh_lod.{h,cpp}`（757 行），顶点聚类，误差有界，6 级，**导入期生成** | **直接作 BLAS 代理几何** —— 低模 + 有界误差 + 现成顶点/索引。这是本次调研最有价值的正面发现，把 AS 构建的复杂度压下来了 |
| **G-Buffer** | `postprocess_pass.cpp:1005-1065`（4 RT：albedo+metallic / normal+rough / emissive+ao / depth） | RT 反射/遮蔽的输入与 SSR/SSAO 同源，可共用；SSR/SSAO 天然成为 RT 不可用时的降级路径 |
| **HiZ 金字塔** | `shaders/ssr_hiz.frag`（2×2 min）+ `shaders/hiz_reduce.frag`（2×2 max），两套互为镜像 | 不能替代 BLAS/TLAS，但可作 ray query 的层次遍历加速结构 |
| **法线/深度预通道** | `postprocess_pass.cpp:1087-1227` | RT 的第一击中着色可复用 |
| **IRenderBackend 扩展范式** | `render.h` 纯虚 31 个 / 总 78 个，采用"**默认空实现 + capabilities 声明**"渐进扩展（compute 就是这么加的，`render.h:134-197`） | 加 RT 能力是**低成本接口改动**（5 个虚函数 + 2 个 capabilities 字段），GL 后端留空即可 |

**缺口**：`IMesh`（`render/render/mesh.h:59-90`）**不暴露 GPU 资源句柄**，AS 构建需要拿到顶点/索引 → 需新增 getter。

### 1.5 建议路线（最小可交付优先）

| 阶段 | 内容 | 人日 |
|---|---|---|
| A0 | 补齐运动矢量（与 DLSS FG 共用，见 §2.2）：合并相机+物体两套、引入 prev view-proj 缓存 | 3–5 |
| A1 | **仅 Vulkan**：扩展探测 → AS 构建（BLAS/TLAS，用 LOD 1–2 作代理）→ ray query 管线 | 10–15 |
| A2 | **D3D12**：新增 HLSL 直编译路径（DXC `lib_6_3`+）、`ID3D12Device5/6`、RT feature 检测、RT root signature、RTPSO 构建 | 12–18 |
| A3 | 光追反射 pass（与 SSR 并存可切换） | 8–12 |
| A4 | 光追阴影 + 降噪（NRD 或自研时域降噪） | 10–15 |
| A5 | `IRenderBackend` 扩展 + 能力降级路径 + 桌面三后端回归 | 5–8 |
| | **合计** | **48–73** |

**建议先只做 Vulkan 拿到一个能看的画面，再决定是否投入 D3D12 那 12–18 人日**（D3D12 侧重复度最高）。

---

## 2. DLSS（SR + 帧生成）+ NVIDIA Reflex

### 2.1 SDK 现状核实（官方）

- **NVIDIA Streamline** 最新 **2.14.1**，开源（`NVIDIA-RTX/Streamline`），集成 DLSS / DLAA / NIS / NRD / Reflex。
- **二进制产物不在 GitHub 仓库**：自 **SL 2.7.32 起**，DLSS 功能 DLL（`nvngx_dlss*.dll`）与 Streamline DLL 需从 **GitHub Releases 下载 zip**（需登录 NVIDIA 开发者账号）。这意味着**发行物不能进 Git，得走构建期下载 + 运行时探测**。
- **DLSS-G 插件只有预编译 DLL**，不可从源码重编（SL 2.0+ 唯一例外）。
- **Vulkan 支持**：官方页面仍标注 "Vulkan support coming soon"，GitHub README 早期版本明说 "does not support Vulkan properly"。→ **本仓库的 Vulkan 后端短期接不了 DLSS**。
- **DLSS-G 硬性环境要求**：Win10 2004 (19041) 以上 + **硬件加速 GPU 调度（HAGS）**。不满足则 DLSS-G 不可用。
- **Streamline 官方 FAQ 明确**：Streamline 核心可跑在非 NVIDIA 硬件上（NIS / NRD / Reflex 跨厂商），但 **DLSS 插件只在 NVIDIA 硬件上有效**。

### 2.2 硬阻塞：运动矢量不满足 DLSS 帧生成

引擎现有**两套互不覆盖**的运动矢量：

| | 位置 | 含相机运动 | 含物体运动 | 格式 |
|---|---|---|---|---|
| 套 A | `shaders/motion_vectors.frag`（深度重建） | 有 | **无** | RG16F |
| 套 B | `shaders/forward_clustered/scene_motion_vectors.*` | **无**（`render_forward_clustered.cpp:562` 的 `prev_mvp` 用的是**当前** view_proj） | 有 | RGBA16F |

而 **DLSS FG 需要相机 + 物体的合并运动矢量**，且需要**上一帧的 view-projection 矩阵**。全仓 grep `prev_view` / `prev_camera` / `prev_view_proj` —— **0 命中**。

后果：直接接 FG 会因相机运动导致**抖动 / 鬼影**。

NVIDIA 的 DLSS SR 集成清单还额外要求：mip-map bias、正确的 jitter（Halton 序列）、每帧送曝光值、场景切换时传 camera reset flag、**HUD 必须走不经过超分的独立路径**（否则 UI 会被超分糊掉/重影）。

**"不经过超分的 HUD 颜色"这条对本仓库有额外含义**：引擎有**内置 UI 轨 + ImGui 轨**两套（见项目 UI 规范），且编辑器有大量面板 —— 需要把 UI 合成挪到 DLSS 之后。

### 2.3 Reflex

Reflex 需要在 present 前后插 latency marker（`eSimulationStart/End`、`eRenderSubmitStart/End`、`ePresentStart/End`）+ `slReflexSleep` + `slGetNewFrameToken`。

引擎侧**已有可复用的 GPU 分段计时**：
- D3D12：`d3d12_backend.cpp:2834+`（`ID3D12QueryHeap(TIMESTAMP)`）
- Vulkan：`vk_backend.cpp:388+`（`VkQueryPool` + `vkCmdWriteTimestamp`）

present 钩子位置也清晰：D3D12 `record_present_blit()`（`d3d12_backend.cpp:726`）→ barrier → `Present`（`d3d12_swapchain.cpp:296`）；Vulkan `vk_swapchain.cpp:526`。

### 2.4 工具链冲突

本仓库 D3D12 后端走 **MinGW + 全运行时 LoadLibrary、零 MSVC 导入库**（`d3d12_loader.h:1-17`；历史上已踩过 3 次 MinGW 链 MSVC 库失败，见 `d3d12_dxc.h:6-7`、`d3d12_backend.cpp:334-338`、`engine/CMakeLists.txt:436-437`）。

Streamline 的 `sl.*.dll` + `nvngx_dlss*.dll` 也必须走同样的 **LoadLibrary 运行时加载**封装，不能链接导入库。

另外 **D3D12 后端有"present 目标 + 帧末 blit"结构**（`d3d12_backend.cpp:483-551` 造 present target，`:726-814` 帧末翻转 blit）—— 颜色不直接写 back buffer。DLSS 通常要求直接在 swapchain 上操作，需调整为"DLSS 输出 → present 目标"或让 DLSS 接管 blit 目标。这是接线时的结构性摩擦点。

### 2.5 建议路线

| 阶段 | 内容 | 人日 |
|---|---|---|
| B1 | 补齐合并运动矢量 + prev view-proj 缓存 + Halton jitter（若 A0 未做） | 3–5 |
| B2 | Streamline 集成骨架：`slInit` / `slSetFeature` / `slEvaluateFeature` / `slReflexSleep`，LoadLibrary 封装 + 运行时下载产物探测 + 降级（无 SL → 关特性） | 8–12 |
| B3 | **DLSS SR**：tag 资源（color/depth/MV/exposure）、mip bias、camera reset、UI 不经过超分的合成路径 | 6–10 |
| B4 | **DLSS FG**：完整 MV 依赖、present 时序、帧 pacing、跨 API 互操作（本仓库无 Vulkan 支持，故只做 D3D12） | 10–15 |
| B5 | **Reflex**：4 个 latency marker + sleep mode + present 同步 | 5–8 |
| B6 | UI 开关（ImGui 调试面板已有渲染开关体系）+ 三后端回归 + A/B 截图 | 5–8 |
| | **合计** | **37–58** |

**优先级建议**：**B2 + B3（DLSS SR）+ B5（Reflex）性价比最高** —— SR 是纯收益（不需 MV 精确到 FG 级别）、Reflex 改动小、且都能在 D3D12 单后端先落地。**B4（帧生成）依赖 B1，且要求 HAGS + 仅 D3D12，投入产出比最低**。

---

## 3. WebGL → WebGPU

### 3.1 最重要的判断：兼容层路线不成立

**不能用"把 GLBackend 包一层喂 WebGPU"**。原因是模型层不兼容，不是工作量问题：

| 维度 | GL / WebGL2 | WebGPU |
|---|---|---|
| 状态模型 | 全局可变状态机（`glBindTexture` / `glActiveTexture` / `glUseProgram`） | 不可变 bind group + 编译期固定 `@group/@binding` |
| 采样器 | 与纹理同类型的 uniform，值 = 纹理单元号 | **独立资源类型** `var s: sampler`，无 uniform 值可写 |
| 采样器参数 | `glTexParameteri(tex, pname, param)`（59 处调用），纹理与参数绑死 | `createSampler` 独立对象 + bind group |
| 顶点数组 | `glBindVertexArray`（17 处） | 无 VAO 对象，`setVertexBuffer(slot, buf, offset)` |
| 光栅化模式 | `glPolygonMode` | **完全不支持** |
| 同步查询 | `glGetIntegerv`（19 处） | 无同步查询 API |

规模数据：GLBackend 共 **5,540 行 / 21 文件 / 126 个唯一 GL API / 约 529 处调用点**；其中约 400 处（资源创建 + 绑定 + 提交）必须按 WebGPU 模型重写。

**另有 15 处 GL 调用泄漏在 `opengl/` 目录之外** —— `engine/render/renderer2d_impl.cpp:2033-2066`（shadow 贴图 compare mode、`glDrawBuffer(GL_NONE)`、1×1 白贴图创建、`glDeleteTextures`、`glBindTextureUnit`），迁移时必须一并处理。

**结论**：必须新写 `engine/render/webgpu/` 目录。好消息是 `IRenderBackend` 抽象**已经就绪且成熟**（31 纯虚 + 47 默认实现），这是干净的扩展点，不是破坏性重构。

### 3.2 着色器迁移是最大单项，且不可自动化

`engine/render/shaders/` 共 **214 个文件 / 12,475 行**：106 个逻辑着色器（GL 基名 46 + `vulkan_` 变体 55 + 独有 9），`.vert` 92 / `.frag` 96 / `.comp` 2。`webgl_` 变体只有 1 个（`webgl_instanced.vert`）。

**GLSL → WGSL 不存在可行的文本转换**，差异是语义级的：

| GLSL | WGSL | 陷阱 |
|---|---|---|
| `layout(location=N) uniform sampler2D uTex;` | `@group(0) @binding(3) var uTex: texture_2d<f32>;` + 独立 `var uTexS: sampler;` | 一个 GLSL uniform → **两个** WGSL 资源 |
| `texture(uTex, uv)` | `textureSample(t, s, uv)` | **仅在 uniform control flow 中合法** —— 引擎 PBR/SSR/GTAO 里大量基于屏幕坐标的动态分支（阴影早退 `proj.x<0||proj.x>1`、深度早退）在 GLSL 合法、在 WGSL 是硬错误，必须改写成 `textureSampleLevel` 或重构控制流 |
| `gl_FragCoord`（原点左下） | `@builtin(position)`（原点左上） | 全局替换 + 语义翻转 |
| 裁剪空间 z ∈ [-1,1] | z ∈ [0,1] | **投影矩阵要改 z 行**，不是 shader 能改的 |
| `mod(x,y)` = floor | `%` = trunc（符号跟 x） | **静默出错** |
| `c ? a : b` | `select(falseVal, trueVal, c)` | **参数顺序反转**，静默出错 |
| `#define GRYCE_HAS_PCSS 0` | 无预处理器 | 现有的后端特性开关机制在 WGSL 里不存在 |
| `uniform float uExposure = 1.0;` | `var<uniform> uExposure: f32;` + CPU 侧写 | 引擎有 78 处 uniform 带初始化器 |

**naga / tint 自动转换可行性**：

- **naga**：GLSL 前端**只吃 GLSL 440+ 且 Vulkan 语义**。引擎 GL 侧是 101 个 `#version 330 core` + 6 个 `#version 430 core` → 必须先升到 Vulkan 语义，等价于**再写 102 个 `vulkan_` 变体**的规模。且 naga 的 WGSL **后端**本身是"次级支持"、已知有 bug（gfx-rs/wgpu#10037）。
- **tint**：**没有 GLSL 输入前端**（只有 GLSL writer / validator）。只能走 GLSL → SPIR-V → WGSL 两段式。
- **仓库已有 `compile_glsl_to_spirv()`（`glsl_compiler.cpp:226`，shaderc 运行时加载）** → 真实可行路径是 **GLSL → 现有 SPIR-V 编译 → SPIR-V→WGSL 转换器 → 人工逐个修正**。SPIR-V 是强类型 IR，比文本转换可靠。但**它省的是"手抄语法"的时间，不是"设计资源布局"的时间**，且前置的 Vulkan 语义化改造使总工作量反而更大。

**结论：手写 WGSL，但照抄已有 102 个 `vulkan_` 变体的 binding 号**（Vulkan 的 `layout(set=0, binding=N)` → WGSL 的 `@group(0) @binding(N)`，vulkan_ 变体已经把 binding 语义梳理清楚了）。这是最大的省力点。

行数分布高度集中：`pbr.frag` 800 行、`instanced.frag` 775 行、`skinned_pbr.frag` ~700、`deferred_lighting.frag` ~500 —— 这 4 个文件占总量约 22%，但难度远高于行数占比（10–20 倍于简单后处理 pass）。

### 3.3 被低估的两项隐藏工作量

**① bind group 化改造**

GL 的"逐纹理散装绑定"模型必须改成"**按 pass 分组的资源集**"。这会侵入：
- `engine/render/render_pipeline.cpp`（**全仓最大的 .cpp**，318 KB）
- `engine/render/render_context.cpp`（48 KB）
- `engine/render/render_thread.cpp`（21 KB）

bind group 绑定 pipeline layout，资源重建时 bind group 必须失效重建 → 需要缓存与失效策略。

**② 平台层：GLFW 无法创建 WebGPU context**

`engine/platform/window.cpp:303-311` 用 `glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API)` 让 Emscripten 的 GLFW 端口建 WebGL2 context。**Emscripten 的 GLFW 端口（内置与 contrib.glfw3）都没有 WebGPU 支持**。

但 GLFW 的**窗口与输入层可以保留** —— WebGPU 的 surface 从 DOM canvas 取（官方 Dawn/Emscripten 示例即如此）。需要改的：
- `window.cpp:303-311` 上下文创建 → 改成 NoApi 路径（与 Vulkan/D3D12 同类），canvas 自行管理
- `window.cpp:73-212` 的 canvas 适配 namespace（140 行 EM_JS + 浏览器视口逻辑）→ swapchain 需显式 `configure()`，尺寸语义与 `glfwSetWindowSize` 那套不同
- `test/webgl_shell.html`（7.3 KB）→ 加 `navigator.gpu.requestAdapter()` + device + context.configure()
- `engine/api/render_api.cpp:1218-1224` 的 "WebGL 强制 sync_mode=1" → WebGPU 同样需要（device 与主线程绑定）

### 3.4 依赖引入：比预期便宜（重要修正）

Emscripten ≥ 4.0.18 起，WebGPU 走 **`--use-port=emdawnwebgpu`**（Dawn 的 C API 绑定，实现 `webgpu.h` 并映射到浏览器 JS WebGPU API）。这是一个 **Emscripten 内置的 remote port**，会自动下载 pinned 版本 —— **只需一个 flag，编译期与链接期各一次**：

```cmake
target_compile_options(target PUBLIC "--use-port=emdawnwebgpu")
target_link_options(target PUBLIC "--use-port=emdawnwebgpu")
```

关键细节：
- 需要 `--closure=1` 压缩体积
- `webgpu.h`（webgpu-native 标准 C API）算稳定；`webgpu_cpp.h` 与 Dawn 特有部分**不稳定**
- 可选 `cpp_bindings=false` 关掉 C++ 绑定
- 官方文档明确：API 稳定部分来自 `webgpu-headers` 仓库

**这意味着第三种依赖范式不是必需的** —— 之前担心的"Rust/Cargo 桥接"只在想要**同一份代码同时跑 native + web**（用 wgpu-native 预编译二进制）时才需要。建议：**先只做 web，用 emdawnwebgpu 的 C API**。

ImGui 侧**成本低**：`third_party/imgui/backends/imgui_impl_wgpu.cpp` **已经在仓库里**（只需把 `IMGUI_IMPL_OPENGL_ES3` 换成 `IMGUI_IMPL_WGPU` + `IMGUI_IMPL_WEBGPU_BACKEND_DAWN`）。

### 3.5 验证链必须改两处

现有 `test/webgl_probe/run_headless.sh` 用 `file://` + `--disable-gpu` + `--enable-unsafe-swiftshader`。换 WebGPU 后：

1. **`file://` 下 `navigator.gpu` 是 undefined，且不报错不警告** —— WebGPU 要求 secure context（HTTPS 或 localhost）。必须改成 `http://localhost:<port>`，或加 `--unsafely-treat-insecure-origin-as-secure=file://`。
2. **`--disable-gpu` 会阻止适配器发现**（`requestAdapter()` 返回 null）。必须去掉，改用 `--enable-unsafe-webgpu` + SwiftShader 路由。
3. **headless Chrome 的 WebGPU canvas presentation 永远到不了合成器**（上游限制，无 flag 组合可修）—— `--screenshot` **会拿到黑图**。但 **rendering / compute / 像素回读在 headless 下都能工作**。

**应对**：改用**像素回读**（`copyTextureToBuffer` + `mapAsync`）代替截图 —— 这恰好与引擎现有的 `capture_frame_rgba()` 机制天然对应（GL 版用 `glReadPixels`，`gl_backend.cpp:851-881`）。

**验证强度反而会提升**：WebGL2 因 16 纹理单元上限被迫把 `GRYCE_HAS_PCSS / _VSM / _GI / _POINT_SHADOW` 注入 0（`gl_shader.cpp:171-190`），**砍掉了 4 个特性**；WebGPU 无此限制 → A/B 对比可做**全 9 通道无删减**。

### 3.6 建议路线

| 阶段 | 内容 | 人日 |
|---|---|---|
| C0 | 探针（adapter/limits/features）+ 骨架：`emdawnwebgpu` 接入、canvas configure、平台层换 NoApi | 8–12 |
| C1 | wgpu 后端核心：device/queue/surface、swapchain、depth/HDR RT、pipeline 缓存、**bind group 框架**、命令编码 | 30–45 |
| C2 | **着色器迁移：106 个 WGSL**（照抄 `vulkan_` 变体 binding 号；pbr/instanced/skinned_pbr/deferred_lighting 四个大头优先） | 60–90 |
| C3 | 2D 管线（`renderer2d_impl`）+ ImGui wgpu 后端 | 10–15 |
| C4 | 后处理/特效全量迁移：bloom / tonemap / TAA / SSR / GTAO / SSIL / FSR2 / 阴影 / SSAO / 体积光 / 粒子 | 25–40 |
| C5 | 验证链改造（secure context、headless 像素回读、9 通道 A/B）+ 桌面零回归 | 8–12 |
| C6 | `toolchain` webpack 单文件分发适配 WGSL payload | 3–5 |
| | **合计** | **144–219** |

**建议**：**不要一步到位替换**。虽然目标是替换 WebGL 后端，但工程上应「**新增 WebGPU 后端 → 两条并存做 A/B → 验证通过后再下线 WebGL**」。理由：WebGL 那条 A/B 对比链（桌面 GL vs WebGL 逐 debug 通道对比，4/9 通道逐字节相同）是目前**唯一的跨后端正确性基准**；直接替换会把这个基准一起拆掉，出问题无法定位。

---

## 4. 三者的关系与整体建议

### 4.1 共享的前置工作

**运动矢量补齐（A0 / B1）同时是光追与 DLSS FG 的前置**。当前引擎两套运动矢量各缺一半、且没有上一帧 view-proj 缓存 —— 这是唯一一处"改一处、受益两处"的地方，建议**第一个做**（3–5 人日）。

### 4.2 三者的技术耦合

| | RTX 光追 | DLSS/FG/Reflex | WebGPU |
|---|---|---|---|
| 互相耦合 | **DLSS Ray Reconstruction（RR）以光追输入为前提** —— 若要上 RR，需先有光追 | 需要 RT 时，DLSS 的 denoiser 模式与自研降噪二选一 | WebGPU **没有** RT 路径（浏览器 WebGPU 至今无 ray query），web 端光追不可得 |
| 共同依赖 | 深度 + 运动矢量 + 曝光 + jitter（这四项正好是 DLSS 清单要求的） | 同左 | 深度/法线/MV 的输出格式要与 WebGPU 的 storage texture 语义对齐 |
| 后端限制 | Vulkan 与 D3D12 都要做，工作量翻倍 | **DLSS 只有 D3D12 可用**（Vulkan 支持未就绪），且需 HAGS | 只影响 web |

### 4.3 建议的推进顺序

```
第 0 步（3–5 人日）  补齐合并运动矢量 + prev view-proj + Halton jitter
                     → 同时解锁 DLSS FG 与光追的输入侧

第 1 步（8–12 人日） Streamline 骨架 + DLSS SR + Reflex（仅 D3D12）
                     → 投入产出比最高，且不依赖光追
                     → 同时验证"运动矢量到底准不准"（SR 的 ghosting 是最好的体检工具）

第 2 步（10–15 人日）仅 Vulkan 的 ray query 反射（用 LOD 作代理几何）
                     → 拿到第一个真实光追画面，评估画质收益
                     → 此时再决定要不要投 D3D12 那 12–18 人日

第 3 步（8–12 人日）WebGPU 骨架 + 探针
                     → 独立轨道，与上面无耦合，可并行

第 4 步（60–90 人日）WGSL 着色器迁移（工程最大头）
```

### 4.4 主要风险清单

| 风险 | 影响 | 缓解 |
|---|---|---|
| **D3D12 RT shader 必须另写 HLSL** | 光追工作量在两个后端间几乎翻倍 | 先只做 Vulkan；或接受"光追只在 Vulkan 可用" |
| **Streamline 二进制不入库**（SL 2.7.32 起需从 Releases 下载） | 构建需登录 NVIDIA 账号下载，且发行物不能进 Git | 构建期下载 + SHA 校验 + 运行时探测缺失即降级 |
| **DLSS 只有 D3D12 可用**（Vulkan 支持未就绪） | Vulkan 后端用户拿不到 DLSS | 明确只支持 D3D12；或用 Streamline 的 NIS（非 NVIDIA 也可用）作 Vulkan 侧超分 |
| **DLSS-G 需 HAGS + Win10 2004+** | 部分用户环境不满足 | 运行时能力查询 + 降级到 SR |
| **运动矢量当前不可用** | 直接接 FG = 鬼影 | 已排在第 0 步 |
| **UI 必须走不经过超分的路径** | 引擎双 UI 轨（内置轨 + ImGui）+ 编辑器大量面板需重排合成顺序 | 在 B3 阶段一并处理 |
| **WGSL 的 `textureSample` uniform control flow 硬约束** | 着色器迁移可能触发语义级重构，工作量上浮 | 优先用 `textureSampleLevel` + 手工 blend 替代分支 |
| **WebGPU 的 bind group 化改造侵入 `render_pipeline.cpp`**（全仓最大 cpp） | 可能连带影响三个桌面后端的回归 | 后端抽象层内收口，不改上层绘制组织语义 |
| **headless Chrome 的 WebGPU 截图不可用** | 现有验证链的截图方式失效 | 改像素回读（与现有 `capture_frame_rgba` 对应） |
| **`file://` 下 `navigator.gpu` 静默 undefined** | 本地调试"什么都跑不起来且无报错" | 验证脚本改 `http://localhost` |

---

## 5. 关键文件索引

**光追相关**
```
engine/render/render.h                              # IRenderBackend(:63-295) / Capabilities(:30-56)
engine/render/d3d12/d3d12_shader.cpp                 # build_stage :235
engine/render/d3d12/d3d12_dxc.h                     # DxcProfile 最高 6.0 (:28-35)
engine/render/d3d12/d3d12_spirv_cross.cpp           # HLSL SM 写死 50 (:246-247)
engine/render/d3d12/d3d12_vulkanize.cpp             # #extension 未处理 (:728-735)
engine/render/d3d12/d3d12_backend.cpp               # present target :483 / blit :726
engine/render/d3d12/d3d12_swapchain.cpp             # Present :296
engine/render/glsl_compiler.cpp                     # GLSL→SPIR-V :226
engine/render/vulkan/vk_device.cpp                  # 扩展探测 :272-318
engine/render/vulkan/vk_shader.cpp                  # 三级缓存 :175
engine/render/vulkan/vk_swapchain.cpp               # present :526
engine/render/postprocess_pass.cpp                  # G-Buffer :1005 / prepass :1087 / SSAO :471
engine/render/renderer_rd/effects/ssr.cpp           # SSR :190
engine/assets/mesh_lod.h / .cpp                     # LOD 顶点聚类（BLAS 代理来源）
engine/render/mesh.h                                # IMesh :59-90（缺 GPU 资源 getter）
```

**DLSS / Reflex 相关**
```
engine/render/shaders/motion_vectors.frag                          # 套A：仅相机
engine/render/shaders/forward_clustered/scene_motion_vectors.*    # 套B：仅物体
engine/render/renderer_rd/forward_clustered/render_forward_clustered.cpp  # MV :540 / prepass :503
engine/render/renderer_rd/effects/fsr2.cpp                         # FSR2 空壳（渲染体被注释 :63-87）
engine/render/d3d12/d3d12_backend.cpp                             # GPU 计时 :2834
engine/render/vulkan/vk_backend.cpp                               # GPU 计时 :388
engine/render/renderer2d_impl.cpp                                 # 泄漏的 15 处 GL 调用 :2033-2066
```

**WebGPU 相关**
```
engine/render/opengl/gl_webgl_shim.h                # 734 行 shim（WebGL 专用）
engine/render/opengl/gl_shader.cpp                 # to_glsl_es300() :153
engine/render/opengl/gl_slot_map.h                 # 136 行槽位表（39 case）
engine/render/opengl/gl_backend.cpp                # GLBackend（5540 行/ 21 文件总量）
engine/platform/window.cpp                         # EMSCRIPTEN 段 :73-212 / GLFW ctx :303-311
engine/render/shader_source_resolver.cpp           # 变体前缀机制 :98-104
engine/render/render.h                             # RenderAPI 枚举 :304-310
engine/render/render.cpp                           # 后端工厂 :14-39
engine/api/types.h                                 # GRYCE_RENDER_API_* :24-29
engine/api/render_api.cpp                          # 后端白名单 / sync_mode 强制
CMakeLists.txt                                     # EMSCRIPTEN 块 11 处（payload 在 405-468）
engine/CMakeLists.txt                              # EMSCRIPTEN 块 2 处（:19-26 / :475-487）
test/webgl_probe/run_headless.sh                  # 验证脚本（需改 2 处）
test/webgl_shell.html                             # 壳（需加 navigator.gpu 初始化）
```

---

## 6. 数据来源

**仓库内（实测）**：全部行数/文件数由 `wc -l` / `find | xargs wc -l` / grep 实测得出；代码引用均给到文件与行号。

**外部（官方）**：
- NVIDIA Streamline SDK 2.14.1 —— `developer.nvidia.com/rtx/streamline/get-started`、`github.com/NVIDIA-RTX/Streamline`
- Emscripten emdawnwebgpu —— `dawn.googlesource.com/dawn/+/main/src/emdawnwebgpu/pkg`
- Emscripten contrib.glfw3 —— `codeberg.org/pongasoft/emscripten-glfw`
- NVIDIA DLSS 集成清单（mip bias / MV 精度 / jitter / exposure / camera reset / HUD 不超分）—— Streamline Programming Guide
