# D3D12 后端接入计划

> 状态：**待执行**（调研 + 计划已完成，未写一行后端代码）
> 制定时间：2026-10-06
> 目标：在引擎里新增第三个渲染后端 `--api dx12`，功能对等于现有 OpenGL / Vulkan 后端。
> 估算：新增约 7,900 行，单人全职 **4–6 周**（24 个工作日中值）。

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
| GLFW 无 OpenGL 上下文的窗口模式**已存在** | ✅ 已有 | `engine/platform/window.h:25-27` `WindowContextType::NoApi`；`window.cpp:95-97` 置 `GLFW_CLIENT_API = GLFW_NO_API` |
| 后端拿到的 `native_window` 是 `GLFWwindow*` | ✅ | `render_context.cpp:157` → `vk_backend.cpp:87` `static_cast<GLFWwindow*>(native_window)` |
| HWND 取得方式 | ✅ | `glfwGetWin32Window()`，`platform_api.cpp:423-431` / `platform_api.cpp:172,210` 已在用 |
| `RenderAPI` 枚举已有 DX11/DX12 位 | ✅（工厂未接线） | `engine/render/render.h:233-238`，`create_render_backend` 对 DX11/DX12 返回 nullptr |
| ImGui DX12 后端 | ✅ 已 vendored | `third_party/imgui/backends/imgui_impl_dx12.cpp` |
| shaderc 动态加载先例 | ✅ 可照抄 | `engine/render/vulkan/vk_glsl_compiler.cpp`（运行时 `LoadLibrary` shaderc，缺失降级 `.spv`） |
| 管线层与后端解耦 | ✅ | `render_pipeline.cpp` 里 GL 字样只在注释（7 处）；后处理 / IBL / material 里 0 处 |
| 着色器规模 | 204 个 GLSL（98 vert / 105 frag / 1 comp） | `engine/render/shaders`；无 imageStore/imageLoad/subgroup，仅 1 处 atomicAdd |
| **MinGW 的 DX 头** | ❌ **一个都没有** | `ls include \| grep -ci "d3d\|dxgi"` = 0（CLion 自带 GCC 15.2.0） |
| **Windows SDK** | ❌ **未安装** | `C:\Program Files (x86)\Windows Kits` 不存在 |
| `__uuidof` 在 MinGW 下 | ⚠️ 需特殊处理 | 实测：GCC 忽略 `__declspec(uuid)`，直接用会链接失败；特化 MinGW 的 `__mingw_uuidof<T>()` 后取值正确 |
| 其它依赖 | ✅ 不受影响 | GLFW/GLEW/Box2D/Jolt 走 FetchContent（`cmake-build-debug/_deps` 已有缓存），不需要 MSYS2 |

---

## 2. 关键设计决策

### 2.1 为什么是 D3D12 而不是 D3D11
D3D11 能省掉资源屏障、根签名、描述符堆，工作量约为 D3D12 的 40–50%。但项目已确立 GPU-driven 路线（compute 剔除 + 间接绘制，D3D12 是一等公民，D3D11 只有弱支持），做 D3D11 属于一次性投入换不来长期收益。**直接做 D3D12。**

### 2.2 不链接 MSVC 导入库，运行时动态加载
照 `engine/render/vulkan/vk_loader.cpp` 的先例：`LoadLibrary("d3d12.dll")` + `GetProcAddress("D3D12CreateDevice")`，`dxgi.dll` 取 `CreateDXGIFactory2`。收益：链接期零依赖、绕开 MinGW 链 MSVC COFF `.lib` 的坑（项目已踩过 vulkan-1.lib）、`GryceRenderer.dll` 不因缺 SDK 而加载失败。

代价：需要自己声明用到的 COM 接口方法（从 SDK 头读签名，自己写调用），因此**仍需要 SDK 头作为编译期依赖**（见 2.3）。

### 2.3 Windows SDK 头：构建期可选探测，不入仓
SDK 头有微软许可，不适合 vendor 进 MIT 仓库。采用与 Vulkan 相同的模式：

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
