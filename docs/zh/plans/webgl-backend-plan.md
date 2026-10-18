# WebGL 后端接入计划

> 状态：**已完成**（2026-10-17 全量执行完毕，M0–M5 全部落地；M5 的 Firefox/Edge 交叉验证因本机未安装这两款浏览器未做）
> 制定时间：2026-10-17
> **实施记录与全部实测数据见 [`../webgl-backend.md`](../webgl-backend.md)。**
> 目标：新增第四个渲染后端 `--api webgl`，通过 Emscripten 把引擎编译成 WebAssembly，在浏览器里跑出与桌面 OpenGL 后端一致的画面。
> 估算：**新增约 1,200–2,100 行**（shim + ES300 源码转换 + CMake 分支 + 少量 shader 变体 + 调试探针；明细见 3.7），单人 **10–16 个工作日**（碎片时间 2–3 周）。
> 对比基准：现有 GL 后端共 **4,393 行**（`engine/render/opengl/`，实测 `wc -l`）被**整体复用、零重写**；D3D12 当年是新增 ~8,000 行（`d3d12-backend-plan.md`）。

---

## 0. 范围

**做**：Emscripten 构建链（CMake `EMSCRIPTEN` 分支、DLL→静态、资源 preload）；`GLBackend` 的 WebGL2 适配（DSA→bind-based shim、GLEW→GLES3 头）；GLSL 330 core → GLSL ES 300 的运行时源码转换；`RenderAPI::WebGL` 全链路接线（C API 枚举、工厂、smoke、shader 解析）；能力降级矩阵（compute/indirect/MSAA 等 WebGL2 没有的能力走既有 CPU 回退路径）。

**不做**（明确排除）：WebGPU/Dawn（用户明确要 WebGL；如未来要做，GLES shim 的经验大部分可复用）；WebGL 1.0（缺 instancing/depth texture/UBO，等于重写管线）；pthreads / OffscreenCanvas 多线程（v1 用引擎已有的 `sync_mode` 内联执行，避免 SharedArrayBuffer 的 COOP/COEP 部署门槛）；移动端 GPU 调优；把 web 冒烟测试放进 CTest（与 D3D12 同一约定：无 GPU/无头不可靠，只人工验证 + CI 验证"能编译"）。

---

## 1. 已核实的前置条件

以下都在本仓库代码里实测确认过（不是推测）：

| 前提 | 状态 | 证据 |
|---|---|---|
| `IRenderBackend` 抽象 + `RenderAPI` 枚举 + 工厂，新增后端有明确挂点 | 是 | `engine/render/render.h:278-286`、`engine/render/render.cpp:14-39`（DX11/DX12 当初就是照此接入） |
| `RenderBackendCapabilities` 支持声明式能力降级 | 是 | `engine/render/render.h:26-52`（compute/indirect/MSAA/rgba16f 全是可选字段） |
| compute 不可用时**已有 CPU 回退路径** | 是 | `render.h:120-137` 注释明确"后端未实现时 `supports_compute_shaders()` 返回 false，调用方必须据此回退到 CPU 路径"；`--no-gpu-cull` 冒烟参数即回退入口 |
| `sync_mode`（渲染线程内联执行）已存在 | 是 | `engine/api/render_api.cpp:70,780,884`：`sync_mode=true` 时**不启动** RenderThread，命令在主线程内联执行 —— WebGL v1 直接复用，浏览器里 WebGL context 不跨线程 |
| GL 着色器全部是现代 core profile 写法 | 是 | `grep -rh "^#version" shaders/`：GL 侧 101 个 `#version 330 core` + 6 个 `#version 430 core`；**`gl_FragColor`/`texture2D`/`varying`/`attribute` 实测 0 处**（唯一误命中是 `hdr_copy.vert:5` 注释里的"varying"字样） |
| GL 侧 SSBO 使用者极少 | 是 | `grep -rl std430`（去掉 vulkan_ 变体）只有 3 个：`cull_instances.comp`、`forward_clustered/scene_pbr_ssbo.frag`、`instanced.vert` |
| shader 源码解析已按 api 分支（有 vulkan_ 前缀先例） | 是 | `engine/render/shader_source_resolver.cpp:93-132`：Vulkan 优先 `vulkan_{name}`，缺失回退基名 —— WebGL 照抄一套 `webgl_{name}` 即可 |
| GLFW 有 Emscripten 官方端口 | 是 | Emscripten `-s USE_GLFW=3`，平台层（`engine/platform/window.cpp`）预期零改动；窗口上下文走 `WindowContextType::OpenGL` 分支（WebGL context 由 GLFW 端口创建） |
| C API 枚举 / 后端切换白名单需要同步加位 | 是（待改） | `engine/api/types.h:24-29`（`GRYCE_RENDER_API_*`）、`engine/api/render_api.cpp:161-170`（`to_internal_api`）、`:1848`（`GRender_RequestBackend` 白名单只放行 GL/Vulkan）、`engine/api/platform_api.cpp:198-218`（`gapi_name` / `create_embedded_window`） |
| 冒烟程序 `--api` 解析 | 是（待改） | `test/renderer_smoke.cpp:849-868`（解析 + 截图命名）、`:879-881`（窗口 ctx 类型：GL 分支 / NoApi 分支） |
| **emsdk 未安装** | 否 | 本机无 `emcc`/`emcmake`（M0 第一件事；注意网络约束，见风险登记） |
| **Amplitude 音频是预编译 MinGW `.a`** | 否 | 根 `CMakeLists.txt:216-233` 明确"随仓库分发"，Emscripten（clang/wasm）无法链接 → Web 上音频需 stub |
| **Box2D / Jolt 在 Windows 是 DLL 导入目标** | 需处理 | `engine/CMakeLists.txt:132-133` 链的是系统共享库；Web 上要么 FetchContent 源码构建（两者均可移植），要么首版 stub |
| **Assimp 同理** | 需处理 | 首版最稳路径：只用内置 ObjLoader（零依赖）；assimp 源码构建放 M4 可选 |

---

## 2. 关键设计决策

### 2.1 为什么是 WebGL2（GLES 3.0）而不是 WebGL 1.0 / WebGPU
- WebGL 1.0 ≈ GLES 2.0：没有 instancing、没有 depth texture、没有 UBO —— 引擎的实例化、阴影图、材质 uniform 全部要重做，等于新写一个后端。
- WebGL2 ≈ GLES 3.0：instancing / `texelFetch` / depth texture / shadow sampler / UBO / sRGB / half-float 全都在，与桌面 GL 的差距是**有限且可枚举的**（见第 5 节）。
- WebGPU 是更现代的答案，但用户明确要 WebGL；且 Dawn/wgpu 接入成本（新 RHI 语义、WGSL 全量着色器迁移）远高于 GLES shim。记为未来备选，不进本期。

### 2.2 复用 GLBackend + Emscripten shim，不新建 `webgl/` 后端目录
这是与 D3D12 计划的**根本区别**：D3D12 与 GL 无任何源码共享，必须新目录 7,900 行；而 WebGL2 与桌面 GL **GLSL 同源、矩阵约定同源（裁剪空间都是 [-1,1]、深度方向一致、无 Y 翻转）**。因此：

- 不新建后端类，`RenderAPI::WebGL` 在工厂里返回**同一个 `GLBackend`**（`gl_backend.cpp` 内已有 `#ifdef` 先例可循）；
- 差异全部收敛到三个注入点：**GL 头/GLEW 替换**、**8 个 DSA 入口的 bind-based shim**、**着色器 ES300 转换**；
- 收益：桌面 GL 后端的 bug 修复自动惠及 Web；Web 侧只为"WebGL2 没有的能力"付费。

### 2.3 着色器链：运行时源码转换（330 core → 300 es），不复制文件
GL 侧 107 个 shader 里 101 个是 330 core。ES 300 的差异只有三项，全部可在加载时机械处理：

1. `#version 330 core` → `#version 300 es`（字符串替换首行）；
2. 注入精度限定符（fragment shader 的 `precision highp float;` / `precision highp int;` + sampler 精度；ES 300 强制要求）；
3. 无第三项 —— 实测 GL 侧 shader 没有 `gl_FragColor`/`texture2D`/`varying`/`attribute`，`out` 变量、`texture()`、`texelFetch`、`layout(location=)` 在 ES 300 全部合法。

转换器约 150–250 行，挂在 `gl_shader.cpp` 的编译入口前，`RenderAPI::WebGL` 时启用。**不复制** 101 个文件（复制的维护代价在 D3D12 计划里已被 `vulkan_*` 变体验证过：204 个变体文件）。只为真正语义不同的 2 个文件写 `webgl_` 前缀变体（见第 4 节）。

### 2.4 线程模型：sync_mode 内联，不上 pthreads
浏览器里 WebGL context 与创建它的线程绑定。两条路：

- **pthreads + `PROXY_TO_PTHREAD`**：需要 COOP/COEP 响应头（SharedArrayBuffer），`file://` 直接打开不可用，本地调试和部署都变复杂；
- **sync_mode**（v1 选择）：引擎已有 `desc.sync_mode`，`GRender_Init` 时不起 RenderThread，命令缓冲在主线程内联消费。Emscripten 主循环（`emscripten_set_main_loop`）每帧驱动，天然匹配 rAF 节奏。代价：失去"主线程录制 / 渲染线程执行"的并行，但 Web 首帧正确性优先，且 sync 路径本来就被冒烟测试长期覆盖。

OffscreenCanvas + Worker 作为 v2 演进方向记录，不进本期。

### 2.5 构建：EMSCRIPTEN 条件分支，DLL → 静态单 wasm
- 根 `CMakeLists.txt` + `engine/CMakeLists.txt` 加 `if(EMSCRIPTEN)` 分支：跳过 GLEW/`opengl32`/`gdi32`/`winmm`/MinGW 运行时拷贝；GLFW 用 `-s USE_GLFW=3`（Emscripten 自带端口，**不**走 find_package）；
- 三个 `SHARED` DLL 在 web 目标下编成 `STATIC`，与 smoke main 链成单一 `.js` + `.wasm`（side-module/DLL 在 Emscripten 下是纯自虐）；
- 资源进 MEMFS：`--preload-file <project>`（场景 JSON / OBJ / 贴图 / shader 覆盖）；引擎自己的 gpack/pak bundle 也走 `fopen`，同一机制覆盖；
- 新增 `option(GRYCE_ENABLE_WEBGL)` + 一个 `cmake/web/` 工具链示例或构建脚本；**无 emsdk 的机器现有构建零影响**（所有 web 代码都在 `#ifdef __EMSCRIPTEN__` / CMake `EMSCRIPTEN` 分支内）。

### 2.6 能力降级走 `RenderBackendCapabilities` 声明，不散落 ifdef
`get_capabilities()` 在 WebGL 构建下返回：`supports_compute=false`、`supports_geometry_shader=false`、`max_msaa_color_samples=1`（首版）、`rgba16f` 按 `EXT_color_buffer_float` 探测结果填。管线层（`render_pipeline.cpp` / compositor / GPU 剔除）已经按这些字段决策，**预期零改动**；smoke 的 `--no-gpu-cull` A/B 即验证回退等价性。

### 2.7 截帧 / 回读
`capture_frame_rgba` 用 `glReadPixels`：WebGL 的 drawing buffer 在 composit 后内容即失效，必须在**同一帧任务内**读取（引擎 `--shot` 流程本就是帧末同步回读，预期可行；若遇 `preserveDrawingBuffer` 需求，在 Emscripten 的 GL 上下文属性里开，代价是每帧多一次拷贝，仅调试用）。

---

## 3. 代码改动清单

### 3.1 API / 接线层（小改，注意 D3D12 计划里同款"静默 fall-through"陷阱）

| 文件 | 改动 | 陷阱 |
|---|---|---|
| `engine/api/types.h:24-29` | `GRYCE_RENDER_API_WEBGL = 4`（**追加**，不打乱既有枚举值） | 放在 DX12 之后，保持 ABI 兼容 |
| `engine/render/render.h:278` | `RenderAPI` 加 `WebGL` 位 | 同步更新分层注释 |
| `engine/render/render.cpp:14` | `case RenderAPI::WebGL: return std::make_unique<GLBackend>()` | 与 OpenGL 同一实现，靠 `__EMSCRIPTEN__` 区分行为 |
| `engine/api/render_api.cpp:161-170` | `to_internal_api()` 加映射 | **必须显式 case**：DX12 计划记载过"DX11/DX12 都 fall-through 到 OpenGL，传 DX12 静默跑 GL"——WEBGL 不能重蹈 |
| `engine/api/render_api.cpp:1848` | `GRender_RequestBackend` 白名单放行 WebGL | 否则运行期切后端请求被静默忽略 |
| `engine/api/render_api.cpp:768,1853` | 后端名日志补 "webgl" | 排查时能认出后端 |
| `engine/api/platform_api.cpp:198-205` | `gapi_name()` 加 case | |
| `engine/api/platform_api.cpp:212-218` | `create_embedded_window()`：WebGL 走 `GLFW_OPENGL_API` 分支（与 OpenGL 同，Emscripten GLFW 建的就是 WebGL context） | 现逻辑"非 GL 即 NoApi" |
| `test/renderer_smoke.cpp:849-868` | `--api webgl` 解析 + `shot_webgl.png` 默认名 | |
| `test/renderer_smoke.cpp:879-881` | ctx 类型：WebGL 归入 `OpenGL` 分支 | NoApi 分支会建不出 GL context |

### 3.2 新文件：`engine/render/opengl/gl_webgl_shim.h`（约 250 行）

把 GL 后端用到的 **8 个 DSA 入口**（实测清点，见下）在 `__EMSCRIPTEN__` 下宏替换为 bind-based 实现：

| DSA 入口 | 清点结果 | WebGL2 等价 |
|---|---|---|
| `glCreateTextures` | gl_texture.cpp ×8 处 | `glGenTextures` + `glBindTexture` |
| `glTextureStorage2D` | gl_texture.cpp ×10 处 | bind + `glTexStorage2D`（GLES3 核心就有不可变存储） |
| `glTextureSubImage2D` | gl_texture.cpp ×6 处 | bind + `glTexSubImage2D` |
| `glTextureParameteri` | gl_texture.cpp ×30+ 处 | bind + `glTexParameteri` |
| `glGenerateTextureMipmap` | gl_texture.cpp ×4 处 | bind + `glGenerateMipmap` |
| `glCreateFramebuffers` | gl_framebuffer.cpp:28 | `glGenFramebuffers` + `glBindFramebuffer` |
| `glNamedFramebufferTexture` | gl_framebuffer.cpp:63,77 | bind FBO + `glFramebufferTexture2D` |
| `glBindTextureUnit` | gl_texture.cpp 槽位缓存 | `glActiveTexture` + `glBindTexture` |

shim 用**同名内联函数 + 宏重命名**（或 `-include` 强制包含头），`gl_texture.cpp` / `gl_framebuffer.cpp` **零改动**。内部维护"当前绑定的 texture/FBO"小缓存，避免每条 DSA 调用都来回 bind。注意 `gl_apply_anisotropy()`（`gl_utils.h`）走 `GL_EXT_texture_filter_anisotropic` 扩展——Emscripten 下同样走扩展枚举，只是 WebGL 的扩展获取用 `getExtension`（GLFW/Emscripten 已封装成 `emscripten_webgl_get_api_extensions` 或直接 GL 调用，M0 验证）。

### 3.3 GLEW 摘除（`#ifdef __EMSCRIPTEN__`）

| 位置 | 改动 |
|---|---|
| `engine/render/opengl/gl_backend.cpp:51-55` | `glewInit()` 跳过；`#include <GLES3/gl3.h>`（Emscripten 头，直接映射 WebGL2）替换 GLEW 头 |
| `gl_backend.cpp:115` `gpu_profile_supported()` | `GLEW_VERSION_3_3` 判断 → Web 构建恒 false（`EXT_disjoint_timer_query_webgl2` 非标配，首版不做） |
| `gl_backend.cpp:65` `gl_register_debug_callback()` | WebGL2 有 KHR_debug，可保留；不可用时静默跳过 |
| `gl_frame_pacing.cpp` | NV_delay_before_swap / WGL 扩展在 Web 不存在 → init 直接返回（capabilities 已声明 false） |
| 根 / `engine/CMakeLists.txt` | Emscripten 分支不链接 `GLEW::GLEW`、`opengl32`、`gdi32` |

### 3.4 着色器接入

| 文件 | 改动 |
|---|---|
| `engine/render/opengl/gl_shader.cpp` | 编译入口前插 ES300 源码转换（`RenderAPI::WebGL` 时启用；版本行改写 + precision 注入） |
| `engine/render/shader_source_resolver.cpp:97` | WebGL 分支优先 `webgl_{name}` 前缀（照 vulkan_ 先例），缺失回退基名 |
| `engine/render/shaders/webgl_instanced.vert`（新） | `instanced.vert` 的 SSBO 版在 WebGL2 不可用 → 改为实例顶点缓冲（`vertexAttribDivisor`，Vulkan `bind_instance_stream` 同款思路）；`instanced.frag` 只需版本/精度机械转换 |
| `engine/render/renderer_rd/forward_clustered/scene_shader_forward_clustered.cpp:41` | `SHADER_GROUP_BASE` 在 WebGL 下路由到非 SSBO 路径（`scene_pbr`），避免 `scene_pbr_ssbo.frag` 的 LightBuffer SSBO；该选择目前是硬编码字符串，按 api 条件化 |
| `engine/render/shaders/depth_resolve.frag` | MSAA 首版关闭（`sampler2DMS`/`gl_FragDepth` 在 ES300 不存在）→ 该 shader 在 Web 不加载；MSAA 后续单独验证 blit resolve |

### 3.5 第三方 / 依赖（Web 分支）

| 依赖 | Web 策略 |
|---|---|
| GLFW | `-s USE_GLFW=3` 端口，平台层零改 |
| ImGui | `imgui_impl_glfw` + `imgui_impl_opengl3` 以 `IMGUI_IMPL_OPENGL_ES3` 编译（vendored 源自带 GLES3 路径）；默认 `--no-debug-ui`，M5 再验证 |
| Box2D / Jolt | 允许 FetchContent 源码构建（均可移植）；**首版可 stub**（`ecs/systems/physics_system_*.cpp` 空转，静态场景渲染不受影响） |
| Assimp | 首版 **OBJ-only demo**（内置 ObjLoader 零依赖，最稳）；assimp 源码构建为 M4 可选项（编译时间长，wasm 体积涨） |
| Amplitude | 预编译 `.a` 无法用于 wasm → `ecs/systems/audio_system.cpp` 在 `__EMSCRIPTEN__` 下编译成 no-op stub，API 表面不变 |

### 3.6 CMake（根 + `engine/`）

```
if(EMSCRIPTEN)
    # 静态库（无 DLL）、跳过 GLEW/opengl32/gdi32/winmm/MinGW 运行时拷贝
    # GLFW: -s USE_GLFW=3；ImGui: -DIMGUI_IMPL_OPENGL_ES3
    # 链接选项: -sALLOW_MEMORY_GROWTH -sUSE_GLFW=3 -sFULL_ES3
    # 资源: --preload-file <demo_project>
    # 音频 stub；物理 stub（或 FetchContent）
endif()
```
新增 `option(GRYCE_ENABLE_WEBGL)`（默认 OFF；检测到 `EMSCRIPTEN` 工具链时自动 ON）。

### 3.7 代码量估算（分行数，只算新增/改动）

| 组件 | 行数 | 说明 |
|---|---|---|
| `engine/render/opengl/gl_webgl_shim.h`（新） | 250–350 | 8 个 DSA 入口的 bind-based 实现 + 绑定状态缓存 + 扩展查询助手 |
| ES300 源码转换器（新，`gl_shader.cpp` 编译入口挂载） | 200–300 | 版本行改写 + precision 注入 + 失败诊断 |
| CMake `EMSCRIPTEN` 分支（根 + `engine/` + 构建示例脚本） | 150–250 | 静态链接、跳过 GLEW/opengl32/gdi32/winmm、`-s USE_GLFW=3`、`--preload-file` |
| API 接线 6 处（`types.h` / `render.cpp` / `render_api.cpp` / `platform_api.cpp` / `renderer_smoke.cpp` / `shader_source_resolver.cpp`） | 80–150 | 枚举位、工厂 case、白名单、日志名、`--api` 解析 |
| GLEW 摘除与 `#ifdef` 适配（`gl_backend.cpp` / `gl_frame_pacing.cpp` / `gl_utils.h`） | 80–120 | `glewInit`、`GLEW_VERSION_3_3`、NV/WGL 扩展分支 |
| `webgl_instanced.vert` 变体 + scene_pbr 非 SSBO 路由 | 40–80 | 唯一两个语义不同的 shader 处理 |
| 依赖 stub（`audio_system` / `physics_system_*`，走 stub 路线时） | 100–200 | 保持 API 表面、编译期空转 |
| ImGui ES3 适配（`IMGUI_IMPL_OPENGL_ES3` 定义 + 小包装） | 50–150 | 默认 `--no-debug-ui`，M5 验证 |
| M2–M4 调试探针 / 日志（env 门控） | 200–500 | 沿用 D3D12 的探针传统（`GRYCE_DX12_PROBE_*` 同款）；试错成本主要落在这里 |
| **合计** | **1,150–2,100** | 不含 107 个 shader 的复制（转换在内存中做）；`gl_texture.cpp` / `gl_framebuffer.cpp` 零改动 |

**不写的东西**（这是估算远小于 D3D12 的原因）：GL 后端 4,393 行整体复用；107 个 GLSL 不复制文件；管线层（`render_pipeline.cpp` / compositor / GPU 剔除）零改动，能力降级走 `RenderBackendCapabilities` 声明。

---

## 4. 着色器迁移（实测口径）

| 类别 | 数量 | 处理 |
|---|---|---|
| `#version 330 core`（GL 侧） | 101 | 机械转换：版本行 + precision 注入。实测无 `gl_FragColor`/`texture2D`/`varying`/`attribute` |
| `#version 430 core`（GL 侧） | 6 | 见下 |
| `vulkan_*` 变体 | 106 | 不转（Web 不用 SPIR-V） |

430 core 的 6 个逐个人理：

1. `cull_instances.comp` —— compute shader，WebGL2 没有 compute → 不加载（GPU 剔除走 CPU 回退）；
2. `instanced.vert` —— 读 `DstModels` SSBO → `webgl_instanced.vert` 变体（实例属性流）；
3. `instanced.frag` —— 无 SSBO，版本随 vert 写成 430 → 机械转换；
4. `deferred_lighting.frag` / `pbr.frag` —— 430 但**无 std430 block**（实测 `grep std430` 未命中这两个）→ 机械转换；
5. `scene_pbr_ssbo.frag` —— LightBuffer SSBO → WebGL 路由到 `scene_pbr` 非 SSBO 路径（见 3.4）。

**推进方式**：转换器先跑通 demo_project 最小集出第一帧，再全量 107 个。每个 shader 编译失败都有明确日志（Emscripten 的 WebGL 编译错误信息很详细），不存在 D3D12 那种"整条管线静默退化成只清屏"的风险面——但也要防"单个 pass 失败被静默跳过"，M4 用 `--debug 1..8` 逐档核对（与桌面同一套手段）。

---

## 5. 平台差异清单（WebGL2 约束，逐条实现时验证）

| 项 | 桌面 GL | WebGL2 | 处理 |
|---|---|---|---|
| DSA（直接状态访问） | 4.5 核心 | **无** | 8 入口 shim（2.2 / 3.2） |
| Compute shader / SSBO | 有 | **无** | `supports_compute=false` → GPU 剔除/实例流回退 CPU（既有路径） |
| 间接绘制（`drawElementsIndirect`） | 有 | **无** | CPU 提交（引擎默认路径） |
| MSAA | FBO 多样本 + resolve | renderbuffer 多样本可建；`sampler2DMS` + 着色器内深度 resolve **无** | 首版 `max_msaa_color_samples=1`；后续验证 blit resolve（`WEBGL_multisampled_render_to_texture` 或颜色 blit） |
| RGBA16F 渲染目标 | 核心 | 需 `EXT_color_buffer_float` | 探测；缺失则明确报错（HDR 管线不可用，不做假降级） |
| RGBA32F 线性过滤 | 核心 | 需 `OES_texture_float_linear` | 引擎几乎不用 32F 线性（IBL LUT 是 16F），探测 + 告警 |
| 深度纹理 / shadow sampler | 核心 | **有**（DEPTH_COMPONENT24 + COMPARE 模式） | 阴影 atlas 直接可用 |
| sRGB 纹理 | 核心（sized format） | **有**（SRGB8_ALPHA8） | 与 GL 后端现有 `set_srgb` 路径一致 |
| 压缩纹理（BC/KTX） | 有 | 需 `WEBGL_compressed_texture_s3tc`（桌面浏览器有、移动没有） | 探测；缺失走解压上传（引擎有解压路径） |
| 各向异性过滤 | 扩展 | `EXT_texture_filter_anisotropic` | 已有 `gl_apply_anisotropy`，探测后降级 |
| GPU 分段计时 | `GL_TIME_ELAPSED` | 需 `EXT_disjoint_timer_query_webgl2` | 首版 false |
| 裁剪空间 Z / 深度方向 / Y 翻转 | [-1,1]，near→0 | **与 GL 完全一致** | 零处理（这是相对 D3D12 少掉的一整类问题：无 FLIP_VERTEX_Y、无 Z remap） |
| 纹理槽位 | 受单元数限制（gl_slot_map 压缩） | WebGL2 最少 16 个 fragment 单元（规范下限 32？实现相关） | capabilities 据实上报，`gl_slot_map` 既有压缩逻辑直接复用 |
| 文件 IO | `fopen` | MEMFS（`--preload-file`） | 资源路径不变 |
| 线程 | RenderThread | sync_mode 内联 | 见 2.4 |
| 帧率控制 / vsync | WGL/NV 扩展 | 浏览器 rAF 管，无 swap interval | `set_swap_interval` 空转；Emscripten 主循环自动跟 rAF |

---

## 6. 里程碑（每阶段可验证产物）

| 里程碑 | 内容 | 工作日 | 验证方式 | 状态 |
|---|---|---|---|---|
| **M0 环境探针** | 装 emsdk（注意网络约束）；独立最小程序：GLFW + WebGL2 清屏 + 画一个三角形；验证 `EXT_color_buffer_float`、DSA 行为、`--preload-file` MEMFS 读取 | 1–2 | 浏览器里看到三角形，console 无错 | 已完成 |
| **M1 构建链 + 后端骨架** | CMake `EMSCRIPTEN` 分支（DLL→静态、跳过 GLEW/opengl32）、GL 头替换、8 入口 DSA shim、GLEW 摘除、`--api webgl` 接线；`sync_mode` 强制开启 | 2–3 | `--api webgl` 出纯色窗口（截图人工确认） | 已完成 |
| **M2 着色器通路** | ES300 转换器 + shader 解析 api 分支；最小场景（一个 cube，pbr 最小集） | 2–3 | 浏览器看到有光照的立方体 | 已完成 |
| **M3 帧缓冲 / 纹理** | HDR RGBA16F（`EXT_color_buffer_float`）、shadow depth texture、skybox cube map、mipmap | 2 | 截图与 `--api opengl` 并排看趋势一致 | 已完成 |
| **M4 场景全链路** | demo_project 或 OBJ 小场景跑通 PBR + 阴影 atlas + IBL + 后处理链；GPU 剔除 CPU 回退 A/B；assimp 源码构建（可选） | 3–5 | `--no-gpu-cull` A/B 差异在噪声地板；`--debug 1..8` 逐档核对 | 已完成 |
| **M5 UI / 截图 / 降级清单** | ImGui ES3 后端（或默认关）、`capture_frame_rgba`、能力降级矩阵全量验证、Firefox 交叉验证 | 1–2 | UI 显示正常；截图文件正确；三浏览器均可加载 | 部分完成 |
| **合计** | | **10–16** | | |

**统一验证方式**：`GryceRenderSmoke.exe --api webgl --shot shot_webgl.png`（Emscripten 产物为 `GryceRenderSmoke.html`，用本地 HTTP 服务器打开，参数经 URL query 传入 `Module.arguments`）。**不进 CTest**（与 D3D12 同一约定），CI 只验证"Emscripten 能编译 + 链接"。

> **实施记录与全部实测数据见 `../webgl-backend.md`。**
>
> M5 未完成项：**Firefox / Edge 交叉验证** —— 本机两款浏览器均未安装，全部验证走 Chrome
> headless（SwiftShader 软件渲染）。真机 GPU、Assimp、物理/音频源码构建、ImGui 中文字体
> 同样未验证，明细见该文档 §8「已知限制」。

---

## 7. 验收标准

1. Chrome / Edge / Firefox 最新版：`--api webgl` 加载场景跑通，无崩溃、无 console error
2. 画面与 `--api opengl` 同场景**人工并排比对**一致（几何、材质、阴影；允许浮点精度级差异）
3. GPU 剔除自动回退 CPU 路径，`--no-gpu-cull` A/B 差异在噪声地板内
4. **CTest 仍 15/15 全绿**（WebGL 不进门禁）
5. Windows 桌面构建**零回归**：所有 WebGL 代码都在 `#ifdef __EMSCRIPTEN__` / CMake `EMSCRIPTEN` 分支内，GL/Vulkan/D3D12 行为不变
6. 无 emsdk 的机器：CMake 配置、构建、运行与现在完全一致

---

## 8. 风险登记

| 风险 | 影响 | 缓解 |
|---|---|---|
| emsdk 安装受网络约束（GitHub HTTPS 不通） | 中，卡 M0 | 走项目既有办法：gh-proxy.com 镜像或 SSH；emsdk 只是 git clone + python，不涉及大文件 |
| `EXT_color_buffer_float` 缺失（老设备 / 移动） | 高，HDR 管线不可用 | M0 探针先在三浏览器验证；运行时探测，缺失给出明确错误而非花屏 |
| Box2D/Jolt/Assimp 源码构建 under Emscripten 的配置摩擦 | 中 | 首版 OBJ-only + 物理 stub，把不确定性推到 M4 之后 |
| DSA shim 的 bind 状态缓存写错 → 纹理串图 | 中 | shim 集中在单头文件；M3 用 `--debug 1`（albedo 档）逐场景核对，串图会立刻可见 |
| wasm 体积 / 首帧加载时间（若带 assimp + FBX） | 中 | 首版 OBJ 小场景；FBX 92MB 本来就不适合 web 分发，需要时离线预转换 |
| ImGui ES3 后端细节（GLSL 版本 define、GLFW 输入事件差异） | 低中 | 默认 `--no-debug-ui`；M5 单独验证，失败也不阻塞主链路 |
| sync_mode 下主线程内联执行的帧 pacing 与桌面不同（rAF 节奏） | 低 | 浏览器场景本就以 rAF 为准；`--frames` + `--shot` 流程不受影响 |
| WebGL 是桌面 GL 的**严格子集**，漏网的 desktop-only 调用只在运行时暴露 | 中 | M0/M1 尽早真机跑；`GL_CHECK_ERROR` 在 Web 下同样有效（Emscripten 会打印） |

---

## 9. 一句话结论

与 D3D12 的根本区别：WebGL2 与桌面 GL **同源**（GLSL、[-1,1] 裁剪空间、GL 深度方向、无 Y 翻转、无 Z remap），4,393 行的 GL 后端整体复用，真正的增量只是「**8 个 DSA 入口的 shim + 107 个 shader 的机械 ES300 转换（其中仅 2 个需要写变体）+ Emscripten 构建分支 + 能力降级矩阵**」——预估 **1,150–2,100 行**、10–16 天，且桌面四后端（GL/Vulkan/D3D12）零回归风险被 `#ifdef __EMSCRIPTEN__` 硬隔离。建议先做 M0 探针（1–2 天：浏览器里跑通三角形 + 验证 `EXT_color_buffer_float`）把最大不确定性消掉，再决定全量投入。
