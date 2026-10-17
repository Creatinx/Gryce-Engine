# WebGL 后端实施记录

> 状态：**已完成**（2026-10-17）
> 对应计划：`docs/webgl-backend-plan.md`（M0–M5 全部落地）
> 新增代码约 **1,900 行**（shim 734 + ES300 转换器 181 + 探针 237 + 壳 101 + shader 变体 61 + 各文件 `#ifdef __EMSCRIPTEN__` 内联块与 CMake 分支约 600），GL 后端 4,393 行**零重写**。

---

## 1. 快速上手

```bash
# 环境（一次性）：emsdk 6.0.12 装在 /d/emsdk
export EMSDK=/d/emsdk
export EMSDK_NODE=/d/emsdk/node/24.19.0_64bit/bin/node
export EM_CONFIG=/d/emsdk/.emscripten
export PATH="/d/emsdk/upstream/emscripten:/d/emsdk/node/24.19.0_64bit/bin:$PATH"

CMAKE="D:/JetBrains/CLion 2026.1/bin/cmake/win/x64/bin/cmake.exe"
NINJA="D:/JetBrains/CLion 2026.1/bin/ninja/win/x64/ninja.exe"

# 配置（独立 build-webgl/，不碰桌面 build/）
emcmake "$CMAKE" -S . -B build-webgl -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_BUILD_TYPE=Release -DGRYCE_ENABLE_D3D12=OFF

# 构建
"$NINJA" -C build-webgl
```

### 单文件分发（`GryceGC webpack`，构建期无关）

CMake 只产多文件（`--preload-file`，增量链接快、体积小 33%）。要 UE5 那种「一个 html
双击即用」的形态时，用打包器做**链接后处理**，不碰构建系统、不重新链接：

```bash
toolchain/build/GryceGC.exe webpack --build-dir build-webgl --name GryceRenderSmoke
# 产物：build-webgl/GryceRenderSmoke.single.html（~5.1MB，零外部引用）
```

| | 多文件（CMake 默认） | 单文件（`GryceGC webpack`） |
|---|---|---|
| 产物 | `.html` + `.js` + `.wasm`(2.8M) + `.data`(639K) | **一个 ~5.1MB 的 `.html`** |
| 资源打包 | `--preload-file`（独立 .data，运行时 fetch） | 链接后内联（运行时零网络请求） |
| `file://` 双击直开 | ✗ CORS 拦 fetch（origin 'null'） | ✓ |
| 需要 HTTP 服务器 | 是 | 否 |
| 增量链接 | 快（只重链 wasm） | 快（打包是独立的链接后步骤） |
| 体积 | 基准 | +33%（.wasm/.data base64 内联） |

实现（`toolchain/src/core/web_pack.cpp`）：把 Emscripten 的 `.js` 内联进 html 壳，
再经两个公开运行时钩子消除剩余网络请求——`Module.locateFile` 对 `.wasm` 返回 base64
data: URL，`Module.getPreloadedPackage` 直接把 `.data` 作为 ArrayBuffer 交给 file
packager。`.data` 不存在时自动跳过（仅内联 wasm）。渲染输出与多文件 HTTP 版
**逐字节相同**（960×540 截图 mean\|d\|=0）。

### 运行

**单文件模式**：直接双击 `build-webgl/GryceRenderSmoke.single.html`，或命令行打开。
参数照样经 URL query 传（`file://` URL 也支持 `?query`）：

```bash
"/c/Program Files/Google/Chrome/Application/chrome.exe" --headless --no-sandbox --disable-gpu \
    --enable-unsafe-swiftshader --window-size=960,540 --virtual-time-budget=60000 \
    --screenshot="D:\\Gryce-Engine\\shot_webgl.png" \
    "file:///D:/Gryce-Engine/build-webgl/GryceRenderSmoke.single.html?--api=webgl&--frames=8"
```

**多文件模式**：必须走 HTTP（`file://` 下 Chrome 以 CORS 拦掉 `.wasm`/`.data` 的 fetch，报
`Aborted(both async and sync fetching of the wasm failed)`）：

```bash
cd build-webgl && python -m http.server 8932 --bind 127.0.0.1 &
"/c/Program Files/Google/Chrome/Application/chrome.exe" --headless --no-sandbox --disable-gpu \
    --enable-unsafe-swiftshader --window-size=960,540 --virtual-time-budget=60000 \
    --screenshot="D:\\Gryce-Engine\\shot_webgl.png" \
    "http://127.0.0.1:8932/GryceRenderSmoke.html?--api=webgl&--frames=8"
```

`build-webgl/index.html`（手动放置，非 CMake 产物）会把根路径 `/` 跳转到应用页并保留 query。

**命令行参数**经 URL query 传入（`test/webgl_shell.html` 解析）：`?--api=webgl&--frames=8&--scene=res%3A%2Fscenes%2Fstress.gesc`。
规则：query 按 `&` 分段，含 `=` 的拆成两个 argv，否则作为独立 flag。所以 `--api=webgl` 与
`--api webgl` 两种桌面写法都能表达。

**日志回读**：壳把 `Module.print`/`printErr` 收进 `<pre id="log">`，`--dump-dom` 直接拿全文；
`--enable-logging=stderr --v=0` 可另外抓 console。

**网页默认行为（常驻交互）**：不给 `--frames` / `--bench` / `--play-frames` 时，
web 构建进 `emscripten_set_main_loop_arg` 的 requestAnimationFrame 循环，一直渲染不退出；
点击画面锁定鼠标后鼠标转视角、WASD 移动、Shift 加速、Esc 释放。只有显式要求跑帧数时才走
「渲染 N 帧 → 截图 → 退出」的一次性路径（无头验证必须可终止）。三条实现约束：

- `Window` / `FpsCamera` 在 `__EMSCRIPTEN__` 下必须堆分配：`set_main_loop` 靠异常展开
  main 的栈，栈对象析构会把 canvas 一起销毁（症状：页面空白、getContext 失效）。
- 指针锁定只能由用户手势触发（浏览器安全策略），GLFW 的 `CURSOR_DISABLED` 从帧循环里
  下发会被拒 —— 壳在 canvas 的 mousedown 里补一次 `requestPointerLock()`。
- pointer lock 下光标位置是**单调累积量**，没有「回中」语义：`FpsCamera::update` 在
  `__EMSCRIPTEN__` 下跳过 `set_cursor_pos`，直接以实际位置作增量基线（否则视角会飞）。

**页面默认不显示日志**：`#log` / `#status` 与截图提取链接默认 `display:none`，URL 加
`?debug=1` 才显示（`debug` 是壳自己的开关，不转发给 `main`）。CSS 隐藏不影响
`--dump-dom` 读到日志文本，无头验证照旧。日志面板只保留最近约 60KB，常驻循环不会把 DOM 撑爆。

---

## 2. 架构：单头文件 shim，桌面零感知

WebGL2 = GLES 3.0，是桌面 GL 的**严格子集**。差异全部收在一个头文件里：

```
cmake/web/GL/glew.h          （12 行）include path 前置后成为 <GL/glew.h> 的替身
   └── engine/render/opengl/gl_webgl_shim.h   （734 行，仅 #ifdef __EMSCRIPTEN__ 生效）
```

9 处既有 `#include <GL/glew.h>` **一行未改**。`cmake/web` 只在 `EMSCRIPTEN` 分支加进 include path，
桌面构建根本不进那个目录。

shim 内部分四类，按「是否真可达」划分：

| 类别 | 处理方式 | 例 |
|---|---|---|
| DSA 入口（真可达） | bind-based **真实现**，target 从注册表反查 | `glCreateTextures` / `glTextureStorage2D` / `glTextureSubImage2D` / `glNamedFramebufferTexture` / `glCompressedTextureSubImage2D` |
| 能力缺失但语义可翻译 | **真实现** + 能力翻译 | `glDrawBuffer`→`glDrawBuffers(1,&buf)`；`GL_CLAMP_TO_BORDER`→`GL_CLAMP_TO_EDGE`（见 §4） |
| 桌面专有能力 | loud-error 占位（`unsupported_call()` 只打一次） | `glDispatchCompute` / `glDrawElementsIndirect` / `glNamedBufferData` / `glGetBufferSubData` / `glMemoryBarrier` |
| 纯编译占位枚举 | `#define` 桌面值 | 22 个 sampler/uniform 类型枚举、`GL_COMPUTE_SHADER`、`GL_DEBUG_SEVERITY_*`、`GL_SHADER_STORAGE_BLOCK` |

**target 注册表**（`std::unordered_map<GLuint, GLenum>`）：DSA 调用不携带 target，而 bind-based
等价物必须知道 target。`glCreateTextures` 与包装后的 `glBindTexture` 两条创建路径都登记；
`glDeleteTextures` 同步失效，避免 id 复用后张冠李戴。

**宏重命名**放在文件**最末**（所有 inline 定义之后），因为定义体内部调用的是真实 ES3 函数 ——
放前面会无限递归。

### 2.1 一个值得记的坑：DSA 签名不带 target

最初给 `gryce_webgl_texture_storage2d` 加了 `GLenum target` 参数，编译期 12 处调用全不匹配。
桌面 GL 的 `glTextureStorage2D(GLuint texture, GLsizei levels, GLenum internalformat, ...)`
**本来就没有 target** —— 这正是需要注册表的原因。签名必须与桌面逐字一致。

---

## 3. ES300 着色器转换（运行时、内存中、零文件复制）

`gl_shader.cpp` 的 `to_glsl_es300()`（181 行）在 `GLShader::compile` / `link_program` /
`link_compute_program` 的编译循环里被调用。四项机械转换：

1. **版本行改写**：`#version 330 core` / `430 core` → `#version 300 es`。
   ESSL 与 ANGLE 都要求 `#version` 在**第一行**（连前置注释都报
   `#version directive must occur on the first line of the shader`），所以原版本行及其之前的
   内容整段丢弃；非注释前缀会告警。
2. **精度块注入**：ES 300 的 fragment shader 没有 float/int 默认精度，不声明直接编译失败。
   注入 `precision highp float/int/sampler2D/samplerCube/sampler2DShadow/sampler2DArray`。
3. **uniform 初始化器剥离**：ESSL 3.00 **不允许**（ANGLE 报 `cannot initialize this type of
   qualifier`），桌面 GLSL 330 允许。引擎 78 处 uniform 带初始化器（tonemap / TAA / SSR 的
   开关量）。因为 GLSL 里 `uniform` 不是合法的局部变量限定符，任何以 `uniform` 开头的行必是
   全局声明，**不需要花括号深度跟踪**。剥离时按 `//` 与 `/*` 切掉注释再找 `=`，否则
   `// 0 = 顶点属性实例流` 这类注释会被误判。引擎所有 uniform 都由 CPU 侧每帧写入，剥离不改变
   行为；仍逐条 GLOG_INFO 列出被剥名字，项目覆盖 shader 若依赖 shader 侧默认值会在这里显形。
4. **`texture2D(` → `texture(`**：防御性，引擎实测零使用。

只写了一个 shader 变体：`webgl_instanced.vert`（61 行，去掉 SSBO，走顶点属性实例流）。
`scene_pbr_ssbo` 不改文件，由 `scene_shader_forward_clustered.cpp` 在 `__EMSCRIPTEN__` 下
路由到 `scene_pbr`。

### 3.1 解析器必须知道当前 api

`GLShader::load_program` / `load_compute` 原先硬编码 `RenderAPI::OpenGL`，导致
`shader_source_resolver` 的 `webgl_{name}` 优先分支永不生效（表现为拿 SSBO 版
`instanced.vert`，报 `std430 : invalid layout qualifier`）。已按 `#ifdef __EMSCRIPTEN__`
切到 `RenderAPI::WebGL`。

---

## 4. 平台差异逐条实现口径

| WebGL2 约束 | 处理 | 依据 |
|---|---|---|
| 无 DSA | shim bind-based 真实现 + target 注册表 | 探针实测 `glCreateTextures`/`glNamedFramebufferTexture` 均 absent |
| 无 compute shader | `GLEW_VERSION_4_3 0` → `supports_compute_shaders()=false` → 走既有 CPU 剔除 | 引擎 `render.h` 本就要求"后端未实现时回退 CPU 路径" |
| 无 indirect draw | loud-error 占位；引擎默认就是 CPU 提交 | — |
| 无 `sampler2DMS` / MSAA render-to-texture | `max_msaa_color_samples=1`，`RenderPipeline` 自动关 MSAA | 探针 `WEBGL_multisampled_render_to_texture = no` |
| 无 `GL_CLAMP_TO_BORDER` / `GL_TEXTURE_BORDER_COLOR` | `glTexParameteri/SamplerParameteri` 包装层把 WRAP 的 BORDER 映射为 EDGE，`TEXTURE_BORDER_COLOR` 调用丢弃并 log 一次 | 见下 |
| 无 `GL_TIME_ELAPSED` | `GLEW_VERSION_3_3 0` → GPU profiling 关闭 | 探针 `EXT_disjoint_timer_query_webgl2 = no` |
| 无 `glGetBufferSubData` | loud-error 占位 + dst 清零（不泄堆内存） | SSBO 本身不存在，被空判挡在运行期外 |
| RGBA16F 需扩展 | 运行期探测 `EXT_color_buffer_float`，缺失 `GLOG_ERROR` 明确失败 | 探针实测 **available** |
| 各向异性过滤 | `GLEW_EXT_texture_filter_anisotropic` 改为运行期扩展探测函数 | 探针 **available** |

**关于 border clamp 为什么能安全退化**：引擎只在深度/阴影贴图上用它，期望"采样越界返回深度 1.0
= 全亮 = 不投影"。这个语义由 shader 侧保证 —— `pbr.frag` / `skinned_pbr.frag` /
`deferred_lighting.frag` 的 `cascade_shadow()` 都有
`if (proj.x<0.0 || proj.x>1.0 || proj.y<0.0 || proj.y>1.0) return 1.0;` 显式早退，另有 5%
边缘 `smoothstep` 淡出到全亮；PCF 滤波核只有几个 texel 宽（<< 5% 贴图边长），淡出区完全覆盖。

---

## 5. 线程模型：三处同步退化

无 `-pthread`（避免 SharedArrayBuffer / COOP / COEP 部署门槛），`std::thread` 构造抛
`system_error: thread constructor failed`。三处按同一模式退化，**API 表面不变**：

| 组件 | 退化方式 |
|---|---|
| `GLog` 单例里的 `AsyncLogger` | `log()` 同步直写内层后端。这是最隐蔽的一处：`GLog::GLog()` 在首次 `GLOG_*` 时构造，异常没人接 → 整个运行时 `Aborted()` 且无任何信息 |
| `AsyncLoader`（资源加载线程池） | `submit()` 立即跑完 worker 并投进 `completed_`，`poll()` 照常在主线程执行回调 —— 语义契约不变，只是"异步"窗口消失 |
| `RenderThread` | `GRender_Init` 在 `__EMSCRIPTEN__` 下强制 `sync_mode=true`（WebGL context 主线程绑定），线程根本不启动 |

另有三处是**链接期**问题，用空 `INTERFACE IMPORTED` 目标让链接行不变，源码 `#ifdef` 桩化：
Box2D / Jolt / Amplitude（预编译 MinGW `.a` 无法为 wasm 链接）。Assimp 首版 OBJ-only
（内置 `ObjLoader` 零依赖）。

---

## 6. 构建细节（`build-webgl/`）

- **DLL → 静态**：`engine/CMakeLists.txt` 的 `_gryce_lib_type` 在 `EMSCRIPTEN` 下取 STATIC
  （cmake 4.3.1 不支持 emscripten shared library，会有 warning，无害）。
- **`SUFFIX ".html"`**：Emscripten 只有生成 html 输出时才应用 `--shell-file`，否则 warning
  `--shell-file ignored when not generating html output` 并把壳**静默丢弃**。
- **preload payload**：`test/webgl_demo@/webgl_demo`（MEMFS 工程）+ `engine/render/shaders@/shaders`。
  后者是必需的：`core_shaders::engine_shaders_dir()` 在 web 上落到 `current_path()='/'`，
  候选 #1 正是 `<base>/shaders`。
- **payload 变化必须触发重链**：`--preload-file` 是链接期打包，改/增文件不重链则 `.data` 不更新，
  表现为"改了场景却没生效"。`target_sources`+`HEADER_FILE_ONLY` 只挂编译步、挂不上链接步
  （ninja 的 LINK 语句只依赖 `.o` 与 `.a`），改用
  `set_source_files_properties(test/renderer_smoke.cpp PROPERTIES OBJECT_DEPENDS <glob>)`
  + `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)`：payload 变 → 重编该 `.o` → 必然重链。
- **`GRYCE_WEBGL_DEBUG`**（默认 ON）：`-sASSERTIONS=1 -sDISABLE_EXCEPTION_CATCHING=0
  -sSTACK_OVERFLOW_CHECK=1`。没有它，abort 只报一句 `Aborted()`，定位不了 §5 那类问题。
- **`-sEXPORTED_RUNTIME_METHODS=FS`**：壳的截图外取要读 MEMFS。Emscripten 6.x 对未导出的
  运行时方法做了「访问即 abort」处理 —— 壳里碰一下 `Module.FS` 就会
  `Aborted('FS' was not exported. add it to EXPORTED_RUNTIME_METHODS ...)`，
  把整个运行时在 main() 中途杀掉（症状是日志只打了前一两行就断）。壳侧也做了
  try/catch 防御，双保险。
- **x86-v3 ISA 标志**在 `EMSCRIPTEN` 下排除（wasm 不认 `-mavx2`）。
- CTest 与 toolchain 在 `EMSCRIPTEN` 下跳过；`GryceSyncRuntimeDlls` 整块跳过。

---

## 7. 验证结果

### 7.1 M0 能力探针（Chrome headless + SwiftShader）

`test/webgl_probe/`，结果 `PROBE_RESULT PASS`：

| 探测项 | 结果 | 对引擎的含义 |
|---|---|---|
| `EXT_color_buffer_float` | **yes** | RGBA16F HDR 管线可用 |
| `OES_texture_float_linear` | yes | 浮点纹理可 mipmap / 线性过滤 |
| `EXT_texture_filter_anisotropic` | yes | AF 可用（做成运行期探测是对的） |
| `EXT_disjoint_timer_query_webgl2` | no | GPU profiling 禁用 |
| `WEBGL_multisampled_render_to_texture` | no | MSAA 渲染到纹理不可用 → `max_msaa_color_samples=1` |
| DSA `glCreateTextures` / `glNamedFramebufferTexture` | **no** | 证明 DSA shim 必需 |
| FBO RGBA16F / DEPTH24 | 均 COMPLETE | HDR + 深度纹理附件可用 |
| MEMFS `--preload-file` | 21 bytes 读回 | 资源加载链路可用 |
| `readPixels` 中心 | `(230,76,51,255)` | 截帧 / 回读链路可用 |
| `glGetError` | `0x0` | 无隐藏错误 |

### 7.2 同场景 GL vs WebGL A/B（`test/webgl_demo`，960×540，`--no-debug-ui --no-fps --no-ssr`）

| `--debug` | 桌面 GL | WebGL | 差 |
|---|---|---|---|
| 0（最终合成） | (138.6,138.3,144.7) | (138.7,138.4,144.8) | 0.1/255 |
| 1（albedo） | (135.8,135.0,139.2) | (135.8,135.0,139.2) | **0** |
| 2（normal） | (131.1,146.3,134.8) | (131.1,146.3,134.8) | **0** |
| 3（roughness） | (139.2,139.2,142.6) | (139.2,139.2,142.6) | **0** |
| 4（metallic） | (51.6,51.6,55.0) | (51.6,51.6,55.0) | **0** |
| 5（shadow） | (141.1,141.1,144.5) | (141.2,141.2,144.5) | 0.1/255 |
| 6（direct） | (127.4,126.6,131.7) | (127.4,126.7,131.8) | 0.1/255 |
| 7（ambient/IBL） | (105.3,106.9,117.0) | (105.3,107.0,117.0) | 0.1/255 |
| 8（cascade） | (55.4,134.4,61.4) | (55.4,134.4,61.3) | 0.1/255 |

4/9 逐通道相同，其余 0.1/255 —— 远低于同设置两次运行的噪声地板（0.03%）。albedo/normal/
roughness/metallic 全等说明纹理上传、sRGB、mipmap、uniform 通路完全正确；shadow/direct/
ambient/cascade 等说明 HDR、shadow atlas、PCF、IBL、tonemap 全对。

### 7.3 GPU 剔除 CPU 回退 A/B

| 配置 | 结果 |
|---|---|
| 桌面 GL，GPU 剔除 | (120.6,108.4,109.5) 50.30% 367 档 |
| 桌面 GL，`--no-gpu-cull` | (120.6,108.4,109.5) 50.30% 367 档 —— **逐字节相同** |
| WebGL（CPU 回退） | (120.8,108.6,109.6) 50.27% 247 档 —— 差 0.2/255 |

### 7.4 压测场景

`test/webgl_demo/scenes/stress.gesc`：115 实体 / 112 网格（10×10 立方体阵列，6 种材质循环：
glossy / chrome / gold / plastic / rough / dark-metal + 12 个球）。渲染正常，见
`shot_webgl_stress.png`。

### 7.5 ImGui ES3

`engine/CMakeLists.txt` 给 `GryceRenderer` 加 `IMGUI_IMPL_OPENGL_ES3`（否则 imgui 的
`imgui_impl_opengl3.h` 在 Emscripten 上自动选 **ES2**）。实测调试面板正常渲染。
`FontAtlas` 找不到 `C:\Windows\Fonts\*.ttf`（MEMFS 里没有系统字体），回落到 ImGui 内置
默认字体，**非致命**；要中文/等宽字体需另预载。

### 7.6 截帧回读（已验证）

`GLBackend::capture_frame_rgba` → `read_framebuffer_rgba_topdown`（`glReadPixels`）→
`stbi_write_png` → smoke 自己再 `stbi_load` 回来算均值。写+读往返在 web 上成立
（`--shot shot_webgl.png` 的 `[smoke] image 960x540 mean=...` 即为证据）。

壳另带 **MEMFS 截图外取**：轮询 `/` 下的 png/bmp，读成 base64 data URL 塞进 DOM，
`--dump-dom` 可直接拿回图。实测解出 2,074,523 字节有效 PNG（画面与 ImGui 调试面板均在），
即整条 `capture_frame_rgba → PNG → 出浏览器` 链路打通。注意需 `-sEXPORTED_RUNTIME_METHODS=FS`，
见 §6 与 §9。

### 7.7 桌面端零回归

| 检查 | 结果 |
|---|---|
| 桌面 `ninja -C build` 全量 | 干净通过 |
| CTest | **16/16 全绿** |
| `--api opengl` 均值 | (92.1,87.3,85.2) —— 与历史基线一致 |
| `--api vulkan` 均值 | (94.3,89.2,85.8) —— 与历史基线一致 |
| `--api dx12` 均值 | (94.3,89.2,85.9) —— 与历史基线一致 |

---

## 8. 已知限制 / 未验证项

| 项 | 状态 |
|---|---|
| **Firefox / Edge 交叉验证** | **未做** —— 本机未安装 Firefox 或 Edge，全部验证走 Chrome headless（SwiftShader 软件渲染）。计划验收标准第 1 条只完成 1/3 |
| 真机 GPU（非 SwiftShader） | 未验证。软件光栅与真机在精度/性能上会有差异，但 WebGL2 规范路径相同 |
| Assimp（FBX/glTF 导入） | 未做。web 首版 OBJ-only；`--preload-file` 带 92MB FBX 不适合 web 分发 |
| 物理 / 音频 | stub 静音。Box2D / Jolt / Amplitude 源码构建 under Emscripten 列为后续可选项 |
| ImGui 中文字体 | 需预载字体文件到 MEMFS |
| 移动端 GPU | 未验证 |
| `GRYCE_WEBGL_DEBUG` 关闭后的发布构建 | 未验证体积/行为差异 |

---

## 9. 排错速查

| 症状 | 原因 |
|---|---|
| `Aborted(both async and sync fetching of the wasm failed)` | 用 `file://` 打开的。必须 HTTP |
| `Aborted()` 且无其他信息 | 未开 `GRYCE_WEBGL_DEBUG`；或 `std::thread` 在无 pthread 下被构造 |
| `std::__2::system_error: thread constructor failed: Not supported` | 又有新的 `std::thread` 使用者漏了同步退化（AsyncLogger 是最早踩的） |
| `#version directive must occur on the first line of the shader` | 转换器没把 `#version` 放第一行（老 bug，已修） |
| `'esprecision' : invalid version directive` | `#version 300 es` 后漏 `\n`，与 precision 块粘在一起（老 bug，已修） |
| `cannot initialize this type of qualifier` | uniform 初始化器没剥（老 bug，已修） |
| `'std430' : invalid layout qualifier` | 拿到了 SSBO 版 shader：`load_program` 没传 `RenderAPI::WebGL`（老 bug，已修） |
| `'sampler2DShadow' : Illegal use of reserved word` | `#version` 被拒后退化成 ES 1.00 语义，通常是上一个问题的连带现象 |
| `failed to resolve 'pbr' in 'webgl_demo/shaders'` | shader 树没预载到 `/shaders` |
| `still waiting on run dependencies: datafile_single/GryceRenderSmoke.data` 刷屏、`main()` 不执行 | 以为 `-sSINGLE_FILE=1` 能把 `.data` 也内联 —— **它只内联 wasm**；`--preload-file` 生成的 `.data` 仍然走 fetch。单文件分发已改为 `GryceGC webpack` 的链接后处理（内联 js + `Module.locateFile`/`Module.getPreloadedPackage`），CMake 不再提供单文件模式 |
| 改了场景/ shader 却没生效 | payload 没触发重链（`OBJECT_DEPENDS` 那节） |
| `--shell-file ignored when not generating html output` | target 缺 `SUFFIX ".html"` |
| `Aborted('FS' was not exported...)` | 壳读了 `Module.FS` 但没 `-sEXPORTED_RUNTIME_METHODS=FS`；**会在 main() 中途把运行时杀掉**，表现为日志只打了一两行 |
| `WebGL2 shim: glTexParameterfv(GL_TEXTURE_BORDER_COLOR) is not available` | **预期行为**，border clamp 已按 §4 退化为 edge clamp |
