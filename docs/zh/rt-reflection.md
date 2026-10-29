# 硬件光追反射与画面重建（Vulkan ray query）

本文描述引擎的硬件光追反射链路（BLAS/TLAS、追踪、降噪、合成）、参数口径，以及画面重建（超分 / NVIDIA DLSS）的接入现状与步骤。

相关文档：[renderer.md](renderer.md)（整体管线与屏幕空间效果）、[performance.md](performance.md)（测量方法）、[shader-conventions.md](shader-conventions.md)（着色器变体与宏规则）。

## 1. 能力边界

| 项目 | 说明 |
| --- | --- |
| 后端 | 仅 Vulkan（`VK_KHR_acceleration_structure` + `VK_KHR_ray_query` + `VK_KHR_deferred_host_operations`）。OpenGL / D3D12 / WebGL 上全部为空操作 |
| 生效条件 | 设备支持光追扩展 **且** `rt_reflection_enabled` 为真；任一不满足时后处理链静默跳过，画面与未开启时零差异 |
| 运行时管线 | 生效位置是 `RenderPipeline`（`render_api.cpp` 实际使用的渲染器），不是 `RenderForwardClustered` |
| 着色器 | GLSL 内联为 C++ 字符串，经 shaderc 运行时编译；shaderc 缺失时通道整体置为无效并回退 SSR |

反射与 SSR 互斥使用：光追开启时建议 `ssr_enabled=false`，否则两者叠加会让反射偏亮。

## 2. 架构

### 2.1 加速结构

- **BLAS**：按 `IMesh*` 缓存，几何不变只建一次（`rt_blas_cache_`）。顶点/索引取自 `IMesh::get_ray_tracing_geometry`，不可用则该实例跳过。
- **TLAS**：每帧按本帧不透明列表重建。实例来自 `opaque_items_`（网格句柄 + 世界矩阵 + 材质粗糙度/金属度）。
- **零主机端等待**：TLAS 的实例缓冲是持久 host-visible 缓冲（按容量 1.5× 余量增长），实例数据直写，构建命令录进当帧命令缓冲（`VkTopLevelAccelerationStructure::record_build`），构建与查询之间用 GPU barrier 串序。
- 变换：引擎世界矩阵是列主序 `m[col*4+row]`，`VkTransformMatrixKHR::matrix` 是行主序 `float[3][4]`，转换在 `vk_backend.cpp` 逐元素写。

### 2.2 三段式通道（`vk_ray_query_pass`）

| 段 | 派发分辨率 | 输入 | 输出 |
| --- | --- | --- | --- |
| trace | 追踪分辨率（默认全分辨率） | TLAS、HDR 深度、HDR 颜色、环境 cube、每实例材质 SSBO | 反射色 + 混合系数；辅助图（世界法线 + 线性深度） |
| temporal | 同 trace | 本帧原始反射、辅助图、历史累积图 + 历史辅助图、上一帧 view-proj | 累积后的反射色 + 系数 |
| composite | 输出分辨率 | 累积图（或原始反射）、辅助图、HDR 颜色、深度 | 与主图混合后的全分辨率结果 |

三段共用同一 shader module 与描述符布局，用 uniform 里的 `resolutionStrength.w` 选择模式（0/1/2）。历史图双缓冲（`frame_parity_`）：本帧写 A 读 B，下帧互换；目标重建或首帧时 `history_valid_ = false`，混合系数取 1（纯当前帧）。

每帧末尾输出图转 `TRANSFER_SRC`，由 `vk_backend.cpp` 用 `vkCmdBlitImage` 写回 HDR 颜色缓冲。

### 2.3 反射内容的三条来源

1. **光滑命中（roughness < 0.15）**：命中点重投影回屏幕，4-tap 双线性采样 HDR 颜色 —— 真实场景色。
2. **粗糙命中**：采样 IBL prefilter 环境 mip（`roughness * max_mip`），并按 `smoothstep(0.15, 0.45, roughness)` 与屏幕色混合。
3. **未命中 / 命中点出屏**：采样 IBL 环境 cube（radiance）。这是屏幕空间方案永远给不出的天空/环境反射。

## 3. 精度

| 手段 | 做法 |
| --- | --- |
| 逐像素追踪 | 默认 `rt_reflection_resolution_scale = 1.0`。实测 0.5 与 1.0 的帧率差在噪声范围内（瓶颈不在光追），因此默认不牺牲精度 |
| 亚像素抖动 | 追踪 UV 按 Halton(2,3) 序列偏移 ±0.5 纹素，连续帧累积后等效超采样，反射边缘不再有阶梯 |
| 法线重建 | 全分辨率 1 纹素的**对称中心差分**（`cross(wR-wL, wU-wD)`）。邻居落在背景或跨轮廓（相对深度跳变 > 5%）时用中心点顶替，退化为半基线但不跳向 |
| 材质感知 | 实例携带 `roughness/metallic`（`instanceCustomIndex` 索引每实例 SSBO），命中做菲涅尔-史利克衰减：`F0 = mix(0.04, 1, metallic)`，掠射角增强、粗糙衰减 |
| 命中取色 | 手写 4-tap 双线性（不用最近邻），消除块状取色 |

> 法线不要用「逐轴挑深度更连续的一侧」的写法：在斜面/透视渐变下这种选择会随亚像素位置周期翻转，重建法线跟着抖，镜面地板上表现为**规则的横向条纹**（本项目踩过）。

## 4. 降噪

`rt_reflection_denoise`（默认开）为真时派发 temporal 段：

1. **重投影**：用当前帧的深度重建世界坐标，乘上一帧的 view-proj 得到历史 UV。
2. **几何校验**：比较当前线性深度与历史线性深度（相对差 `> 18%` 视为遮挡/去遮挡），不可信的位置权重归零，混合系数退化为 1。
3. **方差裁剪**：取当前帧 3×3 邻域的 min/max 色域，把历史色夹进 `[min - 0.15·span, max + 0.15·span]`，抑制鬼影。
4. **混合**：`out = mix(history, current, α)`，α 默认 0.12（约 8 帧收敛）。

`composite` 段用**深度引导的双边上采样**（2×2 双线性权重 × 深度相似权重）替代纯双线性：轮廓两侧（深度断开）的纹素权重塌陷，物体边缘不会被低分辨率的方块拖糊。

已知限制：

- 相机快速切换（瞬移、过场）时历史无效一帧，会有一次可见的延迟收敛；不接受就按相机位移阈值清历史。
- 运动的**被反射物体**在反射里会轻微拖影，由方差裁剪限幅；需要更干净的效果时把 α 调大（0.2~0.3）。

## 5. 画面重建（超分）与 DLSS

### 5.1 模式

| 模式 | 值 | 说明 |
| --- | --- | --- |
| `off` | 0 | 原生分辨率，不做重建；等价于关闭反射的时域累积 |
| `temporal` | 1 | 引擎内置时域重建：抖动 + 重投影累积 + 深度引导双边上采样。无需任何第三方运行时，**默认** |
| `dlss` | 2 | NVIDIA DLSS。运行时缺失时自动降级为 `temporal` 并打一条 WARN 日志 |

### 5.2 运行时探测

`engine/render/super_resolution.{h,cpp}` 负责探测，只做文件与符号检查，不改变设备创建路径：

- `sl.interposer.dll` 可加载且 `slInit` / `slSetFeatureLoaded` / `slEvaluateFeature` 可解析 → Streamline 运行时就位；
- `nvngx_dlss.dll`、`sl.dlss.dll` 在搜索路径内 → NGX 与插件就位；
- 搜索路径：可执行文件目录 → 其父目录 / 祖父目录 → `third_party/streamline/bin/x64` → 工作目录。

探测结果进程内缓存，启动时打一条 INFO（`SuperResolution: <mode> — <detail>`）；`GRender_GetSuperResolutionStatus` 可取同一段文本给 UI 展示。

头文件已在仓库内：`third_party/streamline/include`（SL 2.14.16）。**运行时不在仓库**（体积与许可都不适合入库）。

### 5.3 让 DLSS 真正生效还差什么

按依赖顺序，缺一不可：

1. **运行时**：把 DLSS SDK 的 `sl.interposer.dll`、`sl.dlss.dll`、`nvngx_dlss.dll`（或游戏/驱动的对应文件）放到可执行文件同目录。放好后重启即可被探测到，日志会从「未找到」变成「运行时齐备」。
2. **设备经 interposer 代理创建**：这是当前唯一的硬缺口。Streamline 必须在 `VkInstance` / `VkDevice` 创建时接管（用 `sl.interposer.dll` 导出的同名 `vkCreateInstance` / `vkCreateDevice` 代理，而不是 loader 的原函数），否则拿不到内部资源句柄，`slEvaluateFeature` 必然失败。`VulkanInstance` / `VulkanDevice` 目前直连 loader；接线时建议：
   - 在 `vk_instance.cpp` 里先探测 interposer，存在则全部 Vulkan 入口（`vkCreateInstance`、`vkCreateDevice`、`vkGetInstanceProcAddr`、`vkGetDeviceProcAddr`）改走代理；
   - `slInit` 用 `sl::Preferences` 传 `logMessageCallback`，失败要能把原因打出来；
   - 探测到代理生效后把 `SuperResolutionCaps::device_proxied` 置真，`dlss_usable` 才会成立。
3. **输入标量**：DLSS 需要 jitter、深度、**运动矢量**。
   - jitter：`vk_ray_query_pass` 已有 Halton 序列，可作为全帧抖动的来源（当前只作用于反射追踪）。
   - 深度：HDR 深度缓冲现成。
   - 运动矢量：当前只具备**相机运动**的等价物（由当前深度 + 前后 view-proj 反投影得到），静态几何精确、运动物体不精确。要完整支持需要在几何 pass 输出逐像素 MV（上一帧裁剪坐标 − 当前帧裁剪坐标），这是一个独立改动。
4. **输出分辨率**：DLSS 是「低分辨率渲染 → 高分辨率输出」，需要把 3D pass 的渲染分辨率与呈递分辨率解耦（`PostProcessParams::dlss_render_width/height` 已预留字段）。

在上述 1~4 全部就位前，`dlss_usable` 保持 `false`，引擎不会把未验证的分支接进渲染路径 —— 这是刻意的：宁可如实降级，也不让画面在真机上莫名变糊。

## 6. 配置与接口

`project.data`：

```json
{
  "rt_reflection_enabled": true,
  "rt_reflection_max_distance": 100.0,
  "rt_reflection_resolution_scale": 1.0,
  "rt_reflection_denoise": true,
  "super_resolution": "temporal"
}
```

C API（`engine/api/render_api.h`）：

| 接口 | 说明 |
| --- | --- |
| `GRender_SetRTReflection(bool)` / `GRender_IsRTReflectionEnabled()` | 光追反射总开关 |
| `GRender_SetRTReflectionResolutionScale(float)` | 追踪分辨率档 0.25~1.0 |
| `GRender_SetRTReflectionDenoise(bool)` / `GRender_IsRTReflectionDenoiseEnabled()` | 时域降噪开关 |
| `GRender_SetSuperResolutionMode(GRenderSuperResolutionMode)` | `GRENDER_SR_OFF / TEMPORAL / DLSS` |
| `GRender_GetSuperResolutionMode()` | 实际生效模式（请求 DLSS 但不可用时返回 TEMPORAL） |
| `GRender_GetSuperResolutionStatus(char* buf, int cap)` | 探测结论一行文本，返回写入长度 |

脚本侧（`engine.render.*`）：`set_rt_reflection` / `is_rt_reflection` / `set_ssr` / `is_ssr`。脚本绑定层经 `AppHostServices` 转到 C API，**不链接渲染器**；新增渲染能力时按同样方式加虚函数，不要在 `js_app_bindings.cpp` 里直接调 `GRender_*`。

## 7. 实测数据

环境：RTX 5070 Laptop（驱动 621.01）+ Vulkan，`test/demo_project` 的 `rt_showcase.gesc`，300 帧 × 3 轮取最小值。

| 配置 | 平均帧率 |
| --- | --- |
| 反射关闭（基线） | 243.9 |
| 反射开 / 全分辨率追踪 / 降噪开 | 244.1 |
| 反射开 / 半分辨率追踪 / 降噪关 | 243.7 |

光追开销在噪声范围内（<1%）：本场景的瓶颈是几何与后处理，不是 ray query。降噪多一次半分辨率以上的派发，实测无可测差异。

## 8. 排障

| 现象 | 根因与修法 |
| --- | --- |
| 开关打开但画面零变化、无报错 | 调用点只写在 `RenderForwardClustered`。运行时走的是 `RenderPipeline`，确认 `render_pipeline.cpp` forward-HDR 路径 SSR 之后有调用 |
| 首帧大量 `VulkanBuffer: failed to create buffer` | VMA 分配器缺 `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`，而 BLAS/TLAS 缓冲要 `SHADER_DEVICE_ADDRESS` |
| 首次派发即访问违例（`-1073741819`） | `vkCreateComputePipelines` 漏了，`pipeline_` 恒为 `VK_NULL_HANDLE` |
| 整屏黑、日志干净、fps 正常 | 多段共用 UBO 时区域偏移必须按 `minUniformBufferOffsetAlignment`（NVIDIA 256）取整；未对齐的描述符被驱动静默回零，shader 一进来就被 `px >= res` 早退 |
| 镜面地板出现规则横向条纹 | 法线重建用了逐轴切换差分方向的写法（见第 3 节）；改对称中心差分 |
| 反射等于原色（"跑通但看不出效果"） | 重建法线朝内 → 反射射线自命中。法线翻向相机、起点沿法线外推、`tmin` 抬高 |
| GLSL 编译失败 `texelFetch` / `rayQueryGetIntersectionWorldRay*` | 需要 `GL_EXT_samplerless_texture_functions`；`rayQuery*WorldRayOriginEXT` 在 GLSL 里不存在，命中点用 `origin + rdir * t` 自己算 |
| 想开 validation 定位问题 | `GRYCE_VK_VALIDATION=1` 环境变量 |

调试这类「崩在驱动里 / 静默黑屏」的问题，用关键节点日志逐点收敛最快；定位后必须把临时探针清干净再提交。
