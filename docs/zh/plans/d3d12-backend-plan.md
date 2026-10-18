# D3D12 后端接入计划

> 状态：**已实施**（M0–M6 全部落地；2026-10-11 完成"与 OpenGL 对齐"的最后一批缺陷修复；
> 2026-10-13 修复 SSR 偶发整帧丢失（跨线程竞争，见「实施现状」）并将
> `GRYCE_ENABLE_D3D12` 默认改为 ON。本文件保留为设计记录与差异清单。）
> 制定时间：2026-10-06｜最后更新：2026-10-13
> 目标：在引擎里新增第三个渲染后端 `--api dx12`，功能对等于现有 OpenGL / Vulkan 后端。
> 估算：新增约 7,900 行，单人全职 **4–6 周**（24 个工作日中值）。

---

## 实施现状（2026-10-11）

`engine/render/d3d12/` 下已实现全部 18 个源文件（loader / iids / device / swapchain /
buffer / texture / framebuffer / mesh / shader / hlsl 翻译 / renderer2d / imgui / backend /
compute / descriptors / upload / barriers / reflect），约 8,000 行。

构建开关 `-DGRYCE_ENABLE_D3D12=ON`（默认 OFF；Windows + GCC/Clang 时探测 mingw 自带
D3D12 头或 Windows SDK 头）。本机（MSYS2 UCRT64 GCC + NVIDIA RTX 5070 Laptop）实测：

| 验证项 | 结果 |
|---|---|
| `GryceRenderSmoke.exe --api dx12 --shot x.png` | 出画面，无崩溃，无 validation **错误**（仅余 64 条"清屏值与创建值不一致"的 WARNING，GL/Vulkan 同样无关且为旧有行为） |
| 与 `--api opengl` 截图逐像素比对 | **平均 \|Δ\| = 0.06/255**（等价于逐像素一致） |
| 三种配置（默认 / `ssr_enabled=false` / `ibl_intensity=0`） | 均与 OpenGL 一致，且方向稳定 |
| `--no-gpu-cull` A/B | 与 GPU 剔除路径**逐像素一致** |
| 静态长跑 12000 帧 / 3000 帧 | 0 崩溃、0 validation 错误、无上传环告警 |
| **Play 模式长跑 1500×4 次 + 2000 帧** | **0 次设备挂起**（修复前每次都在 6~2656 帧内 DEVICE_HUNG，见缺陷 5） |
| CTest | 5/5 全绿（D3D12 不进门禁） |
| OpenGL / Vulkan 回归 | 与改动前截图逐像素一致 |
| **SSR 好/坏帧竞争修复后**（2026-10-13） | 6 连跑 + 300 帧长跑 ×2 + GPU 校验 ×2 全部正常；CBV 探针 372+ 帧零值帧 = 0 |
| **GPU 剔除 A/B**（`--instanced 200`，开/关剔除） | 差异 0.3% 像素（视觉等价）；SSR 在实例场景正常 |
| **构建默认值** | `GRYCE_ENABLE_D3D12` 默认 ON（2026-10-13 起；无 SDK/mingw 头的机器静默跳过） |

### 本轮修掉的缺陷（都是实测定位，不是推测）

1. **IBL 镜面全丢 → 金属全黑、地面偏暗 9%**
   根因在 `D3D12Shader` 的纹理绑定：`set_int(<采样器名>, slot)` 记下的"待配对名字"
   会悬挂，被之后**任意一次** `set_texture` 吃掉。引擎 `upload_ibl_textures` 的调用顺序是
   "set_texture 在前、set_int 在后"，于是 BRDF LUT 被绑到 `uPrefilterMap` 的寄存器、
   级联阴影图被绑到 `uBRDFLUT` 的寄存器 —— 完全静默。
   修法：配对只在"下一个 set_texture 的 slot 与 set_int 的 slot 相同"时成立，且无论
   成败都清掉待配对名（见 `d3d12_shader.{h,cpp}`）。

2. **整幅画面上下镜像**
   翻译层用 `FLIP_VERTEX_Y` 把管线等效成 GL 约定（这样引擎 GL 版着色器里的 V 翻转、
   屏幕空间 UV 推导全部原样成立），代价是成品相对 D3D 呈现方向镜像。这个"最后一次翻转"
   原先没人做。
   期间验证过一条错路：直接关掉 `FLIP_VERTEX_Y` 会让几何方向天然正确，但**每个全屏 pass
   都会多翻一次** → 镜像次数的奇偶随启用的 pass 变化（开/关 SSR 方向都不同），且着色器里
   那些 GL 约定的 V 翻转全部失效。
   修法：所有"画到屏幕"的 pass 先落在后端内部的 **present 目标**上，帧末由一条自带
   全屏 blit 通道翻一次写进真实 back buffer（`d3d12_backend.cpp` 的
   `ensure_present_target` / `record_present_blit`）。ImGui 是 D3D 原生约定，
   在绘制前先把 present 结果刷进 back buffer 再画（`flush_present_blit_for_overlay`），
   避免 UI 跟着被翻。

3. **上传环容量不足 → HiZ 数据每帧被丢弃**
   `UploadRing` 按 frames_in_flight 分段，单次分配超过一个分段就是永远失败。
   GPU 遮挡剔除的 HiZ 纹理→SSBO 回写是 `w*h` 字节（1080p = 2,073,600B），而旧的固定
   4MB → 每槽 1.4MB，于是每帧必然告警一次并把该帧 HiZ **静默丢弃**（遮挡剔除长期按
   旧层级工作）。修法：容量按"一帧 HiZ + 4MB 余量 × 帧槽数"给足（`D3D12Backend::init`）。

4. **切到离屏帧缓冲时对真实 back buffer 发了错误的屏障**
   旧逻辑在"屏幕 → 离屏"切换时把 back buffer 从 RENDER_TARGET 切回 PRESENT；现在屏幕
   RT 是 present 目标，back buffer 本帧一直停在 PRESENT 态 → 每帧一条调试层 ERROR。
   修法：该处不再动 back buffer（present 目标的状态由 blit 统一处理）。

5. **Play 模式随机 DEVICE_HUNG（0x887A0006）** —— 粒子实例流被"边用边销毁"
   症状：静态帧一切正常，一旦进入 Play 模式（组件 `on_update` 生效、粒子开始模拟）就在
   6~2656 帧内随机设备移除；用户日志里最先报错的是 `particle` / `particle_depth` 的
   PSO 创建（其实那只是"设备已经没了"的第一个探针）。
   定位过程：`--no-shadow` 不挂、`GRYCE_D3D12_SERIALIZE=1`（逐帧等 GPU）不挂、
   调试层加载后不挂、把上传环放大到 192MB（排除环耗尽）仍挂、禁用 ShadowAtlas 仍挂
   → 收敛到"每帧都在动的粒子实例流"；把场景里的 ParticleSystem3D 删掉后 Play 模式
   800 帧完全不挂。
   根因：`D3D12Mesh::upload_instances()` 每次上传都先 `Release()` 旧实例缓冲再重建，
   而粒子系统**每帧**都会上传（实例数还逐帧变化）。这等于销毁 GPU 仍在读的资源 ——
   D3D12 明令禁止，驱动直接判非法 → DEVICE_HUNG。是否触发纯看时序，所以帧号随机，
   且静态/串行化/带调试层时都被掩盖。
   修法：`D3D12Mesh` 三个上传路径统一走 `update_or_create_buffer()`：**容量够就原地更新**
   （不再销毁资源），容量不足才新建，并把旧资源通过
   `D3D12Backend::retire_gpu_resource()` 挂到围栏值上延迟释放（与后端的 retire_* /
   UploadSink 同一套机制）。

### 本轮顺带修掉的隐患

* **present blit 的描述符泄漏**：翻转 blit 每帧从共享 SRV 堆申请 1 个槽位却不归还，
  按 8192 容量算约 8000 帧后堆被撑满 → `DescriptorAllocator` 会"重建堆 + 拷贝描述符"，
  已录进在飞命令列表的 GPU 句柄（绝对地址）立刻悬空 → 设备移除。现在改成**按帧槽固定复用**
  3 个槽位（`begin_frame` 已经等过该槽围栏值，复用安全）。
* **上传环容量**按"一帧 HiZ + 4MB"给足（见缺陷 3 的后续）：1080p 下 17MB / 3 槽。

### 采样器模式按着色器声明生成（GPU 未定义行为，已修）

`commit_textures` 原来按**纹理对象**的 `dxgi_filter()` 决定采样器模式，但同一个纹理
会同时以"比较采样器"（阴影 `uShadowMap`）和"普通采样器"（PCSS 的原始深度
`uShadowMapDepthN`）被同一个 shader 绑定 —— 模式只跟纹理走必然有一边不匹配。
GPU 校验层（`GRYCE_D3D12_GPU_VALIDATION=1`）直接报：

```
ERROR: GPU-BASED VALIDATION: Draw, Sampler mode differs from that expected by shader:
  Sampler Mode Expected: SAMPLER_MODE_COMPARISON, Sampler Mode In Descriptor: SAMPLER_MODE_DEFAULT,
  ... Root Parameter Index: [3], Shader Stage: PIXEL ...
```

这是 **GPU 层面的未定义行为**（普通调试层完全不报），表现为"同一批命令有时采样正确、
有时采到垃圾"。现在改为以**着色器自己的声明**为准：扫 HLSL 里
`SamplerComparisonState ... : register(sN)` 得到"期望比较模式的寄存器集合"
（`comparison_sampler_regs_`），比较位/ComparisonFunc 全部与之一致。修后 GPU 校验
从 30+ 条报错降到 0。

### SSR 偶发整帧丢失 —— 已修复（2026-10-13，跨线程竞争）

**症状**：金属（Chrome）物体偶发失去屏幕空间反射，退化成纯 IBL（唯一色数 ~2900 → ~590，
与 `--no-ssr` 基线一致）。仅 D3D12；GL / Vulkan 从不复现。

**定位过程**（全部实证，见下）：
1. 纹理探针（`GRYCE_DX12_PROBE_SSR=1`，把 SSR 链四张纹理拷回 CPU 打内容哈希）：
   坏帧 `trace_out` **全零**（nonzero=0.000），`composite_out` = `hdr_in` 场景色 +
   alpha=1（SSR 贡献为零）；好帧 `trace_out` 有正常内容。`hdr_in` 好坏帧哈希一致
   → 丢失发生在 composite 之前，即 **ssr_trace 输出全零**。
2. CBV 探针（`GRYCE_DX12_PROBE_CBV=1`，打每次 commit 的影子内存哈希 + 关键 uniform
   解码值）：坏帧 ssr_trace 的 uniform **除 uView 外全零**（near=0 far=0
   maxRough=0 …），而 uView 正常（0.795,-0.176,0.580）。零值帧在好/坏两次运行里
   **都间歇出现**（帧号不规则，约 40% 帧）。
3. `uSSRMaxRoughness = 0` → ssr_trace.frag 第一道门 `roughness >= uSSRMaxRoughness`
   对所有像素成立 → 每像素早退输出 0 → 整帧 SSR 丢失。机制闭环。

**根因**：`IShader::set_post_process_params` 是 RenderContext 命令表里**唯一缺失**的
直接调用 —— 管线各特效（SSR/GTAO/Bloom/Tonemap/TAA/… 共 14 处）在**主线程录制期**
直接写 D3D12 的 uniform 影子内存；而影子内存由**渲染线程**按帧复位
（`D3D12Shader::on_begin_frame`，`ensure_frame` 惰性触发）。命令缓冲是三缓冲、
`present()` 不等待，主线程可领先数帧 —— 于是"主线程写 pp 参数"与"渲染线程帧复位"
构成跨线程数据竞争：复位落在写入之后 → pp 参数被擦成 GLSL 默认值 0。
uView / uScreenSize 因为是走命令缓冲的（`set_uniform_mat4`，渲染线程在复位**之后**
按序执行）所以始终正确 —— 这正是"uView 正常、uSSR* 全零"这个特征图案的由来。
也解释了此前的全部观测：仅 DX12 复现（只有它有影子内存 + 帧复位）、时序相关
（`SERIALIZE=1` 逐帧等 GPU → 主线程无法领先 → 竞争窗口消失；GPU 校验拖慢 GPU →
主线程领先更多 → 稳定复现）、GPU 校验零报错（CPU 侧竞争，不违反任何 API 规则）、
带调试 UI 才复现（UI 使主线程每帧工作量变化 → 领先量变化）。

**修复**：把 pp 参数改走命令缓冲，与其它 uniform 写入同一条路：
* `render_commands.h`：新增 `RenderCommandType::SetPostProcessParams`（参数块用
  `shared_ptr<const PostProcessParams>` 承载，避免内联 60+ 字段撑大每条命令）；
* `render_context.{h,cpp}`：新增 `RenderContext::set_post_process_params()`（值 +
  shared_ptr 两个重载）；
* `render_thread.cpp`：派发该命令（不进 CommandStateCache，总是执行）；
* 14 个调用点从 `ctx->shader(h)->set_post_process_params(p)` 改为
  `ctx->set_post_process_params(h, p)`。
渲染线程的执行序因此变成：帧复位（本帧第一次状态写）→ … → pp 参数命令 → uView 命令
→ draw → commit_uniforms 读影子内存，全部值就位。GL（pp_dirty_ + bind 时应用）与
Vulkan（push 块）两条路径语义不变，仅调用线程从主线程变为渲染线程（本就是
`ctx->shader()` 注释要求的"必须在渲染线程使用"）。

**修复验证**（RTX 5070 Laptop，`--api dx12 --w 640 --h 360`，默认调试 UI）：
* 6 连跑 + 300 帧长跑 ×2 + GPU 校验（此前的稳定复现条件）×2：**全部 GOOD**，
  372+ 帧 CBV 探针**零值帧 = 0**，trace_out 哈希与好帧一致；
* GPU 校验日志零 D3D12 ERROR/WARNING；
* GL / Vulkan 回归：A/B（开/关 SSR）反射均在，运行正常；CTest 5/5 全绿。

**留存的调试探针**（env 门控，关闭时零开销，静态 bool 缓存）：
* `GRYCE_DX12_PROBE_SSR=1` + `GRYCE_DX12_PROBE_FRAME=<N>`（默认 60）：SSR 链
  纹理（hdr_in / depth_in / normal_in / trace_out / blur_out / composite_out）
  内容统计（均值 / 非零占比 / FNV-1a 哈希）；
* `GRYCE_DX12_PROBE_CBV=1`（+ `GRYCE_DX12_PROBE_CBV_NAME=<子串>`，默认 ssr_trace）：
  每次 commit 的 CBV GPU 地址 / Ring 偏移 / 影子哈希 / 关键 uniform 解码值；
* `GRYCE_DX12_PROBE_RING=1`：UploadRing 每次分配的流水（帧号 / 偏移 / 数据哈希）；
* 既有：`GRYCE_DX12_TRACE_FRAME=<N>`（逐 draw CBV + uModel）、
  `GRYCE_DX12_TRACE_BIND=1`（采样器↔纹理映射）、`GRYCE_TRACE_HDR=1`（HDR 站点）。

---

## 0. 范围

**做**：D3D12 设备/交换链/资源/着色器/帧缓冲，接通 `RenderPipeline` 全链路（阴影、IBL、后处理、GPU 剔除、间接绘制），`--api dx12` 能在 `test/demo_project` 上跑出与 OpenGL 一致的画面。

**不做**（明确排除，避免范围膨胀）：Agility SDK / 光追 / 网格着色器 / MSAA（当前 `IFramebuffer` 抽象里就没有 MSAA 字段，见 `engine/render/framebuffer.h`）、DXR、硬件探针优化。

**不做**：把 DX12 冒烟测试放进 CTest。按项目 2026-10-05 约定，CTest 只放确定性数值核对；`--api dx12` 只做人工验证。

---

## 1. 已核实的前置条件

以下都是在本机 / 代码里实测确认的，不是推测。

| 前提 | 状态 | 证据 |
|---|---|---|
| GLFW 无 OpenGL 上下文的窗口模式**已存在** | 是 已有 | `engine/platform/window.h:25-27` `WindowContextType::NoApi`；`window.cpp:95-97` 置 `GLFW_CLIENT_API = GLFW_NO_API` |
| 后端拿到的 `native_window` 是 `GLFWwindow*` | 是 | `render_context.cpp:157` → `vk_backend.cpp:87` `static_cast<GLFWwindow*>(native_window)` |
| HWND 取得方式 | 是 | `glfwGetWin32Window()`，`platform_api.cpp:423-431` / `platform_api.cpp:172,210` 已在用 |
| `RenderAPI` 枚举已有 DX11/DX12 位 | 是（工厂未接线） | `engine/render/render.h:233-238`，`create_render_backend` 对 DX11/DX12 返回 nullptr |
| ImGui DX12 后端 | 是 已 vendored | `third_party/imgui/backends/imgui_impl_dx12.cpp` |
| shaderc 动态加载先例 | 是 可照抄 | `engine/render/glsl_compiler.cpp`（运行时 `LoadLibrary` shaderc，缺失降级 `.spv`） |
| 管线层与后端解耦 | 是 | `render_pipeline.cpp` 里 GL 字样只在注释（7 处）；后处理 / IBL / material 里 0 处 |
| 着色器规模 | 204 个 GLSL（98 vert / 105 frag / 1 comp） | `engine/render/shaders`；无 imageStore/imageLoad/subgroup，仅 1 处 atomicAdd |
| **MinGW 的 DX 头** | 否 **一个都没有** | `ls include \| grep -ci "d3d\|dxgi"` = 0（CLion 自带 GCC 15.2.0） |
| **Windows SDK** | 否 **未安装** | `C:\Program Files (x86)\Windows Kits` 不存在 |
| `__uuidof` 在 MinGW 下 | 需特殊处理 | 实测：GCC 忽略 `__declspec(uuid)`，直接用会链接失败；特化 MinGW 的 `__mingw_uuidof<T>()` 后取值正确 |
| 其它依赖 | 是 不受影响 | GLFW/GLEW/Box2D/Jolt 走 FetchContent（`cmake-build-debug/_deps` 已有缓存），不需要 MSYS2 |

---

## 2. 关键设计决策

### 2.1 为什么是 D3D12 而不是 D3D11
D3D11 能省掉资源屏障、根签名、描述符堆，工作量约为 D3D12 的 40–50%。但项目已确立 GPU-driven 路线（compute 剔除 + 间接绘制，D3D12 是一等公民，D3D11 只有弱支持），做 D3D11 属于一次性投入换不来长期收益。**直接做 D3D12。**

### 2.2 不链接 MSVC 导入库，运行时动态加载
照 `engine/render/vulkan/vk_loader.cpp` 的先例：`LoadLibrary("d3d12.dll")` + `GetProcAddress("D3D12CreateDevice")`，`dxgi.dll` 取 `CreateDXGIFactory2`。收益：链接期零依赖、绕开 MinGW 链 MSVC COFF `.lib` 的坑（项目已踩过 vulkan-1.lib）、`GryceRenderer.dll` 不因缺 SDK 而加载失败。

代价：需要自己声明用到的 COM 接口方法（从 SDK 头读签名，自己写调用），因此**仍需要 SDK 头作为编译期依赖**（见 2.3）。

### 2.3 Windows SDK 头：构建期可选探测，不入仓
SDK 头有微软许可，不适合 vendor 进 AGPL 仓库。采用与 Vulkan 相同的模式：

- `engine/CMakeLists.txt` 里 `find_path(D3D12_INCLUDE_DIR d3d12.h HINTS "$ENV{WindowsSdkDir}Include/...")` + 探测 `dxgi1_6.h`
- 命中 → `set(GRYCE_HAS_D3D12 TRUE)` 并编译后端；未命中 → 静默关闭（与 `GRYCE_HAS_VULKAN` 同一套写法，`engine/CMakeLists.txt:216-220`）
- 头只加 include 路径，**不需要 lib**

CI：`windows-latest` 镜像自带 Windows SDK，可编译；后端开关不影响 CTest。

### 2.4 IID / GUID：自己定义
MinGW 没有 `dxguid.lib`。新建 `d3d12_iids.cpp` 用 `DEFINE_GUID` 定义用到的 IID（约 15–20 个），并为每个接口特化 MinGW 的钩子模板，使 SDK 代码里的 `IID_PPV_ARGS` 原样可用：

```cpp
struct __declspec(uuid("...")) ID3D12Device;            // SDK 头带来（属性被 GCC 忽略，无妨）
static const GUID IID_ID3D12Device = { /* ... */ };
template<> const GUID& __mingw_uuidof<ID3D12Device>() { return IID_ID3D12Device; }
```
用宏批量生成，避免手写出错。

### 2.5 着色器链：GLSL → SPIR-V → HLSL → DXIL
复用 Vulkan 侧已有的 shaderc 动态加载得到 SPIR-V，再经 **SPIRV-Cross**（vendor 源码，MIT）转 HLSL，最后用 **DXC**（`dxcompiler.dll`，运行时动态加载 + 缺失降级）编成 DXIL。

降级链（与 Vulkan 的 `.spv` 兜底同思路）：
1. 运行时 GLSL→SPIR-V→HLSL→DXIL（首选，可热重载）
2. 预编译 DXIL 入库（离线批量产出，CI/无 SDK 环境可用）
3. 都没有 → 该 shader 加载失败，走 CPU 回退或明确报错

**分批**：先只做 demo_project 实际用到的最小 shader 集（目标是 1.5–2 周出第一帧），跑通后再批量覆盖剩下 204 个。

### 2.6 D3D12 没有 push constants → 用 root constants 模拟
`IShader` 的接口是 **按名字** `set_int/set_float/set_vec4/set_mat4/...`（`engine/render/shader.h:197-216`），Vulkan 后端内部把每帧数据（uView、接触阴影参数等）塞进 push constants。D3D12 无 push constants，等价方案是 **root constants**（32-bit 条目，上限 64KB），一个 `mat4` = 16 个 DWORD，够用。根签名按着色器实际用到的名字生成（名字 → 槽位映射表在 shader 反射时建）。

### 2.7 资源屏障：显式状态跟踪
Vulkan 后端有 `VkImageMemoryBarrier` 显式记录；D3D12 需要**自己**做资源状态跟踪（D3D12 隐式 promotions/decay 不可靠，尤其 RT 与深度复用）。策略：
- 后端维护每资源当前状态（`D3D12_RESOURCE_STATES`），在 `bind_framebuffer` / `set_texture` / `draw` 等状态切换点推导屏障
- 同一 command list 内做状态合并，批末统一 `TransitionResource`
- 用 `D3D12_RESOURCE_STATE_PRESENT` 隐式处理交换链

---

## 3. 代码改动清单

### 3.1 平台 / API 层（小改，但**有 3 个隐藏陷阱**）

| 文件 | 改动 | 陷阱说明 |
|---|---|---|
| `engine/api/render_api.cpp:104-111` | `to_internal_api()` 把 `GRYCE_RENDER_API_DX12` 映射到 `RenderAPI::DX12` | **当前 DX11/DX12 都 fall-through 到 `RenderAPI::OpenGL`** —— 也就是说现在传 DX12 会静默跑 OpenGL，不报错。接线时必须先改这里 |
| `engine/api/platform_api.cpp:199-206` | `create_embedded_window()` 的 `api == VULKAN` 判断改为 `!= OPENGL`（DX12 也要 NO_API） | 否则外部 HWND 嵌入模式会给 DX12 建 OpenGL 窗口 |
| `engine/api/platform_api.cpp:258-281` | `GWindow_RecreateClientApi()`：DX12 时**不要**调 `glfwMakeContextCurrent`；日志里的 api 名字补全 | 现逻辑是"非 Vulkan 就 makeContextCurrent"，DX12 走进来会去找一个不存在的 GL 上下文 |
| `test/renderer_smoke.cpp:731-732` | 上下文选择加 DX12：`use_vulkan \|\| use_dx12 ? NoApi : OpenGL`；`--api` 解析加 `dx12` | 冒烟程序是唯一的窗口创建者 |
| `engine/platform/window.*` | **不改** | `WindowContextType::NoApi` 已就绪 |

### 3.2 后端工厂
- `engine/render/render.cpp`：`create_render_backend()` 加 `case RenderAPI::DX12:` → `new D3D12Backend()`，整段用 `#if GRYCE_HAS_D3D12` 保护，未启用时 `GLOG_INFO` + 返回 nullptr（与 Vulkan 缺失时同款）
- `engine/render/render.h:229-238`：更新分层注释（"DX11/DX12 预留"→ 说明 D3D12 已实现/可探测）

### 3.3 新目录 `engine/render/d3d12/`（约 7,900 行）

| 文件 | 预估行数 | 职责 | 参照 |
|---|---|---|---|
| `d3d12_loader.{h,cpp}` | 250 | 动态加载 d3d12.dll / dxgi.dll；`D3D12CreateDevice`、`CreateDXGIFactory2`、`D3D12GetDebugInterface` | `vulkan/vk_loader.cpp` |
| `d3d12_iids.cpp` | 120 | `DEFINE_GUID` + `__mingw_uuidof` 特化 | 新增 |
| `d3d12_device.{h,cpp}` | 400 | 设备、命令队列、命令分配器/列表、上传堆、RTV/DSV 堆、fence、在途帧 | `vulkan/vk_device.cpp` (372) |
| `d3d12_swapchain.{h,cpp}` | 500 | `CreateSwapChainForHwnd`、present、resize、vsync（对应 GL 的 MAILBOX 策略）、帧同步 | `vulkan/vk_swapchain.cpp` (586) |
| `d3d12_barriers.{h,cpp}` | 350 | 资源状态跟踪 + 屏障合并 | 新增（D3D12 特有） |
| `d3d12_buffer.{h,cpp}` | 250 | `IBuffer`：vertex/index/instance/SSBO/indirect/upload | `vulkan/vk_buffer.cpp` (178) |
| `d3d12_texture.{h,cpp}` | 800 | `ITexture`：DXGI 格式映射、mipmap、BC 压缩上传、SRV 描述符 | `vulkan/vk_texture.cpp` (1187) |
| `d3d12_framebuffer.{h,cpp}` | 280 | `IFramebuffer`：RTV/DSV、附件绑定、resize | `vulkan/vk_framebuffer.cpp` (274) |
| `d3d12_mesh.{h,cpp}` | 250 | `IMesh`：顶点布局语义、输入布局、实例流、间接绘制 | `vulkan/vk_mesh.cpp` (82) |
| `d3d12_shader.{h,cpp}` | 1200 | `IShader`：DXC 编译、根签名生成、描述符堆、root constants、`set_*` by name | `vulkan/vk_shader.cpp` (1892) |
| `d3d12_hlsl_translator.{h,cpp}` | 300 | SPIRV-Cross 调用 + 磁盘缓存 | `vulkan/vk_spirv_cache.cpp` (120) |
| `d3d12_renderer2d.{h,cpp}` | 1800 | `IRenderer2D`（文字/字体/批处理） | `vulkan/vk_renderer2d.cpp` (2259) |
| `d3d12_imgui_backend.{h,cpp}` | 200 | 包一层 `imgui_impl_dx12` | `vulkan/vk_imgui_backend.cpp` (163) |
| `d3d12_backend.{h,cpp}` | 1200 | `IRenderBackend` 全部虚方法 + 能力查询 + 截图/回读 | `vulkan/vk_backend.cpp` (1462) |
| **合计** | **≈ 7,900** | | |

### 3.4 第三方
- `third_party/spirv-cross/`：vendor 源码（MIT）
- `dxcompiler.dll` / `dxil.dll`：**不入仓**。运行时 `LoadLibrary`，缺失降级到预编译 DXIL
- SDK 头：**不入仓**，构建期探测

### 3.5 CMake（`engine/CMakeLists.txt`）
```
find_path(D3D12_INCLUDE_DIR d3d12.h HINTS <WindowsSdkDir>/Include/<ver>/um/winsdk ...)
find_path(DXGI_INCLUDE_DIR  dxgi1_6.h HINTS 同上)
if(两者都找到) set(GRYCE_HAS_D3D12 TRUE) endif()
if(GRYCE_HAS_D3D12) add_library(GryceD3D12 STATIC d3d12/*.cpp) endif()
```
与现有 `GRYCE_HAS_VULKAN` 写法对称；不需要任何 `.lib`。

---

## 4. 着色器迁移（204 个）

**自动翻译能覆盖的**：绝大多数（`layout(location)` → semantics、uniform → cbuffer、SSBO → StructuredBuffer）。

**必须人工处理的清单**：
1. `layout(binding=N)` 的 SSBO/UBO → `RWStructuredBuffer` / `cbuffer`（22 个文件用到了 binding）
2. 顶点 `layout(location)` → `TEXCOORDn`/`POSITIONn` 语义，并据此生成 `D3D12_INPUT_ELEMENT_DESC`（170 个文件用到）
3. `gl_FragCoord` 的 Y 翻转（24 个文件用到）—— 用一个后端 flag 传给着色器，或在生成的 HLSL 头部统一注入
4. `cull_instances.comp`（唯一的 compute）：`atomicAdd` / SSBO 读写 / `gl_DispatchCompute` 分组
5. 整数顶点属性（`VertexType::Int/UInt/Int4/UInt4`）走 `InputAssembler` 整数格式，语义与 float 分开

**推进方式**：先只做 demo_project 依赖的最小集 → 出第一帧 → 再按使用频率批量补齐剩余。

---

## 5. 平台差异清单（易踩坑，逐条要在实现时验证）

| 项 | GL / Vulkan | D3D12 | 处理 |
|---|---|---|---|
| 裁剪空间 Z | `[-1, 1]` | `[0, 1]` | 改 `Matrix4f::perspective`（`engine/math/math.cpp`，列主序）加后端 remap，或后端侧矩阵后处理；**注意 `math_camera` 的 CTest 断言不能被改坏** |
| 深度约定 | near→0 / far→1，`LESS` | 同 | 保持不变（项目已有统一约定） |
| 渲染目标 Y 方向 | 与 GL 一致 | 与 GL 相反 | RT 采样与 `gl_FragCoord` 双重翻转；Vulkan 那边已有 `uHizCfg.w` 编码 0/1/2 处理同类问题，可参考 |
| 纹理 slot 上限 | 受单元数限制（`texture_unit_for_slot` 压缩高位槽位） | 描述符表，不受限 | `capabilities.max_texture_slots` 直接给足，`texture_unit_for_slot` 返回原值 |
| push constants | 有 | 无 | root constants 模拟（见 2.6） |
| 资源同步 | 显式 barrier | 显式 barrier（+ 状态跟踪） | 见 2.7 |
| 截图 / 回读 | `capture_frame_rgba` | 需 `CopyResource` 到 staging + `MapRead` | `ITexture` 无 readback 是既有缺口，DX12 侧要单独实现 |
| 校验层 | Vulkan validation layer | D3D12 调试层（需 Graphics Tools 可选功能） | `set_validation_enabled` 对接；开发机装 Graphics Tools |

---

## 6. 里程碑（每阶段都有可验证产物）

| 里程碑 | 内容 | 工作日 | 验证方式 |
|---|---|---|---|
| **M0 环境探针** | 装 Windows SDK；写独立 `d3d12_clear.exe`（MinGW 编 + SDK 头 + 运行时加载 + `__mingw_uuidof` 特化），清一屏颜色 | 1–2 | 程序能跑出纯色窗口 |
| **M1 设备 + 清屏** | 后端骨架：loader / iids / device / swapchain；`--api dx12` 出一个纯色窗口 | 3 | 截图人工确认 |
| **M2 网格 + 着色器通路** | buffer/mesh/shader/translator；画一个立方体（1 个 shader 打通 SPIRV-Cross→DXC） | 5–6 | 截图看到有光照的立方体 |
| **M3 帧缓冲 + 纹理** | framebuffer/texture/上传/mipmap；跑通离屏 RT | 4 | 截图 |
| **M4 接 RenderPipeline** | 打通 demo_project：PBR、阴影 atlas、IBL、后处理链 | 5–7 | 截图与 OpenGL 并排比对 |
| **M5 2D / UI / 截图** | renderer2d、imgui、`capture_frame_rgba` | 3–4 | UI 显示正常 + 截图文件正确 |
| **M6 GPU 剔除（可选）** | compute 剔除 + 间接绘制 + 存储缓冲 | 2–3 | `--no-gpu-cull` A/B 对照 |
| **合计** | | **23–29** | |

**每个里程碑的验证方式统一为**：`./GryceRenderSmoke.exe --api dx12 --shot dx12.png`，人工看图。
**不进 CTest**（无 GPU / 无头环境不可靠），CI 只验证"能编译"。

---

## 7. 验收标准

1. `--api dx12` 加载 `test/demo_project` 跑通，无崩溃、无 validation 错误
2. 画面与 OpenGL 后端**人工并排比对**一致（几何、材质、阴影、UI）
3. 后端热切换往返（Vulkan → DX12 → OpenGL）无资源泄漏、无句柄失效
4. **CTest 仍 5/5 全绿**（D3D12 不影响门禁）
5. OpenGL / Vulkan 现有行为**零回归**
6. 未安装 Windows SDK 的机器：CMake 配置通过、D3D12 静默关闭、其余功能正常

---

## 8. 风险登记

| 风险 | 影响 | 缓解 |
|---|---|---|
| MinGW + SDK 头摩擦（GCC15 vs MSVC 扩展、`__uuidof`、SAL 标注） | 高，可能卡死 | M0 探针先验证；失败则退回"自写最小 COM 声明"（工作量大但不依赖 SDK） |
| 204 个着色器翻译的边缘 case | 中高 | 分批；先最小集出画面；保留 GLSL 原文以便回退 |
| root constants / 根签名与 `set_*` by-name 模型不匹配 | 中 | 建名字→槽位映射表，参照 Vulkan 后端 `set_uniform_*` 路由 |
| 资源状态跟踪写错 → 画面错乱 / 调试层报错 | 中 | 统一在 barrier 模块做，其它文件不直接发屏障；开调试层跑 |
| 裁剪空间 Z 改错 → 深度全错，且会连带影响 CTest | 中 | 改在 `Matrix4f::perspective` 单一入口 + 后端 flag；改完必跑 CTest |
| 2D 渲染器工作量大（VK 版 2259 行） | 中 | 可延后到 M5；先 3D 主链路 |
| 打包体积（若带 DXC） | 低 | DXC 缺失降级到预编译 DXIL；或用系统自带 FXC |

---

## 9. 一句话结论

上层架构已经为这一天准备好了（NoApi 窗口、DX12 枚举位、动态加载先例、后端无关的管线），
真正的工作量集中在**一个新目录的 7,900 行 + 204 个着色器迁移 + 环境（本机无 Windows SDK）**。
建议先做 M0 探针（1–2 天）把最大不确定性消掉，再决定是否全量投入。
