# Gryce Engine

[![CI](https://github.com/Creatinx/Gryce-Engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Creatinx/Gryce-Engine/actions/workflows/ci.yml)

面向 Windows 的 C++26 游戏引擎，核心为带反射系统的 ECS 架构与四后端（OpenGL / Vulkan / D3D12 / WebGL）渲染器。已打通场景序列化、资源系统、2D/3D 物理、音频与动画，附带两个可直接运行的宿主：渲染演示台 `GryceRenderSmoke`，以及"项目只写 main.js"的运行时 `GryceApp`。

平台与工具链：Windows 为主目标。构建脚本不含任何机器专属绝对路径，依赖前缀由 `cmake/GryceDependencyPaths.cmake` 从环境变量与编译器路径自动推断。已验证可直接用于 MSYS2 UCRT64、CLion / JetBrains 自带 MinGW、vcpkg 及任意自定义前缀；缺依赖时 `FetchContent` 自动拉源码兜底，拉不到会自动换镜像。

English documentation: [docs/README.md](docs/README.md)（英文入口）。

---

## 文档导航

文档按模块组织，中文在 `docs/zh/`，英文在 `docs/en/`，可直达：

| 模块 | 中文 | English |
| --- | --- | --- |
| 引擎架构（DLL 分层 / ECS / 反射 / 资源） | [docs/zh/architecture.md](docs/zh/architecture.md) | [docs/en/architecture.md](docs/en/architecture.md) |
| 构建（依赖策略 / 选项 / 产物 / CI） | [docs/zh/build.md](docs/zh/build.md) | [docs/en/build.md](docs/en/build.md) |
| 渲染（四后端 / 管线 / 阴影 / 效果） | [docs/zh/renderer.md](docs/zh/renderer.md) | [docs/en/renderer.md](docs/en/renderer.md) |
| 着色器规范（变体 / 宏规则 / ESSL300） | [docs/zh/shader-conventions.md](docs/zh/shader-conventions.md) | [docs/en/shader-conventions.md](docs/en/shader-conventions.md) |
| 模型导入与材质识别 | [docs/zh/model-import.md](docs/zh/model-import.md) | [docs/en/model-import.md](docs/en/model-import.md) |
| 自动 LOD | [docs/zh/lod.md](docs/zh/lod.md) | [docs/en/lod.md](docs/en/lod.md) |
| 渲染性能 | [docs/zh/performance.md](docs/zh/performance.md) | [docs/en/performance.md](docs/en/performance.md) |
| WebGL 后端 | [docs/zh/webgl-backend.md](docs/zh/webgl-backend.md) | [docs/en/webgl-backend.md](docs/en/webgl-backend.md) |
| GryceGC 工具链 | [docs/zh/toolchain.md](docs/zh/toolchain.md) | [docs/en/toolchain.md](docs/en/toolchain.md) |
| 脚本系统（QuickJS 组件） | [docs/zh/scripting.md](docs/zh/scripting.md) | [docs/en/scripting.md](docs/en/scripting.md) |
| 应用入口（GryceApp + main.js） | [docs/zh/app-entry.md](docs/zh/app-entry.md) | [docs/en/app-entry.md](docs/en/app-entry.md) |
| 原生扩展（extension.cpp / ABI） | [docs/zh/extension.md](docs/zh/extension.md) | [docs/en/extension.md](docs/en/extension.md) |
| 测试与 CI | [docs/zh/testing.md](docs/zh/testing.md) | [docs/en/testing.md](docs/en/testing.md) |
| 文档总索引 | [docs/zh/README.md](docs/zh/README.md) | [docs/en/README.md](docs/en/README.md) |


---

## 快速开始

```bash
# 1. 准备依赖（任选其一，缺了也会自动下载兜底）
#    MSYS2 UCRT64：
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-gcc-libs \
                    mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
                    mingw-w64-ucrt-x86_64-glfw mingw-w64-ucrt-x86_64-glew \
                    mingw-w64-ucrt-x86_64-box2d mingw-w64-ucrt-x86_64-jolt-physics \
                    mingw-w64-ucrt-x86_64-assimp
#    CLion 自带 MinGW / vcpkg / 自定义前缀：无需额外操作，CMake 自动推断前缀

# 2. 配置 + 构建
cmake -S . -B build -G Ninja
cmake --build build --parallel

# 3. 跑测试（纯计算正确性核对，全部通过才算绿）
ctest --test-dir build --output-on-failure

# 4. 运行（交互模式：WASD 移动，鼠标转视角）
build/GryceRenderSmoke.exe --api opengl --play
```

---

## 特性

**架构**

- 反射驱动的 ECS：组件经反射系统注册，可被序列化、编辑器与脚本统一访问
- 场景以 JSON 文本格式（`.gesc`）存储，实体由 `name` / `uuid` / `transform` / `components` 组成
- 运行期热重载：场景、着色器、材质与网格支持重载，无需重启进程
- Play 模式：进入后驱动系统更新（物理、动画），退出后恢复编辑态快照
- 稳定的 C API 边界（`engine/api`），引擎以 DLL 形式对外提供能力

详见 [docs/zh/architecture.md](docs/zh/architecture.md)。

**渲染**

- 四后端：OpenGL / Vulkan / D3D12 / WebGL，共用上层管线与着色器源（Vulkan、D3D12 的函数与着色器链均运行时加载，不绑定 MSVC 导入库；WebGL 经 Emscripten 复用 GL 后端源码）
- 内置调试 UI：左上角 FPS / 帧时间 / 限速状态叠加（带帧时间曲线），以及 ImGui 调试窗口（F1 开关）——可热开关垂直同步、帧率上限（软件锁帧）、省电模式、GPU 计时，以及阴影 / GPU 剔除 / SSR / IBL 等渲染开关
- Clustered Forward 前向管线、HDR、色调映射、自动曝光
- PBR 材质与预设（如 `Chrome` 等金属 / 镜面预设）
- 阴影：Shadow Atlas、VSM / ESM、PCSS 软阴影、点光源阴影、接触阴影
- 屏幕空间效果：SSR（反射）、SSAO、SSIL
- 全局光照与环境：SDFGI、VoxelGI、Reflection Probe、IBL、体积雾
- 后处理：Bloom、TAA、运动模糊、Bokeh DOF、FSR2、次表面散射、水面
- 2D 管线：精灵、图块地图、视差背景、2D 粒子、2D 天空盒、文本
- 3D 扩展：3D 粒子、线渲染、拖尾、实例化网格、Billboard、3D 文本、LOD、体积光

详见 [docs/zh/renderer.md](docs/zh/renderer.md)；着色器硬约束见 [docs/zh/shader-conventions.md](docs/zh/shader-conventions.md)。

**子系统**

- 物理：Box2D（2D 刚体、风力区、浮力区）+ Jolt Physics（3D 刚体、碰撞体）
- 音频：Amplitude Audio SDK（3D 空间音频、总线、事件）
- 动画：骨骼、动画剪辑、姿态混合
- 资源：`res:/` 虚拟路径、Pak / Gpack 打包、AES-GCM 加密、异步加载
- 资产格式：OBJ（含多材质 MTL）、FBX / glTF / DAE / PLY / STL / 3DS / Blend（Assimp）、DDS、KTX、EXR、PNG/JPG 等
- 模型导入自动识别材质：内嵌 / 外链贴图自动解析（含大小写与缺失路径回退、内嵌贴图落盘、glTF ORM 打包贴图拆通道），按文件里的 PBR 因子或 spec-gloss 字段推导金属度 / 粗糙度，导出金属 / 非金属 / 半透明 / 自发光四种表面类型来驱动光照——见 [docs/zh/model-import.md](docs/zh/model-import.md)
- LOD：导入期生成整条 LOD 链，运行时按几何误差的屏幕像素数选级，巨模型支持空间分块 LOD——见 [docs/zh/lod.md](docs/zh/lod.md)
- 性能：`--bench` 分段 GPU 计时 + CPU 分相；导入期顶点焊接（去索引化网格 4x 压缩）、PCSS 退化参数自动退回 PCF、逐帧 uniform 告警去重——量法与数据见 [docs/zh/performance.md](docs/zh/performance.md)

---

## 目录结构

```
Gryce-Engine/
├── CMakeLists.txt                   根构建脚本：依赖发现、测试注册、DLL 同步
├── cmake/                           构建辅助（无硬编码路径）
│   ├── GryceDependencyPaths.cmake   依赖前缀自动推断 + 缺失排查提示
│   └── GryceFetchDeps.cmake         缺失依赖的 FetchContent 源码构建兜底
├── engine/                          引擎本体（命名空间 gryce_engine）
│   ├── api/                         C API 边界（core / entity / scene / component / render ...）
│   ├── math/                        数学库与相机
│   ├── scene/                       实体、场景、预制体、序列化、UUID
│   ├── ecs/                         World、ComponentStore 与各 System（systems/ 子目录）
│   ├── components/                  组件定义（2d / 3d / common）
│   ├── reflection/                  反射注册与内建类型反射
│   ├── render/                      渲染抽象层与四个后端
│   │   ├── opengl/                  OpenGL 后端（WebGL 经 gl_webgl_shim.h 复用）
│   │   ├── vulkan/                  Vulkan 后端（运行时加载 vulkan-1.dll）
│   │   ├── d3d12/                   D3D12 后端（运行时加载 d3d12.dll / dxgi.dll）
│   │   ├── renderer_rd/             管线实现：forward_clustered / shadow / effects / environment
│   │   ├── storage_rd/              GPU 资源存储
│   │   └── shaders/                 GLSL 着色器源
│   ├── assets/                      模型 / 图像加载、材质推导、LOD、顶点优化、异步加载
│   ├── resources/                   工程、资源路径、Pak / Gpack、加密
│   ├── animation/                   骨骼、剪辑、姿态
│   ├── script/                      QuickJS 运行时 / 绑定 / 脚本组件 / 入口脚本 / 扩展装载
│   ├── app/                         通用应用宿主与模板 main（GryceApp.exe）
│   ├── platform/                    窗口、输入、光标
│   └── utils/                       日志、帧率限制等
├── docs/                            项目文档（zh / en 双语文档与历史规划）
├── test/
│   ├── math_tests.cpp               数学与资源规则测试（CTest 门禁之一）
│   ├── renderer_smoke.cpp           演示宿主源码（GryceRenderSmoke.exe，不参与 CTest）
│   └── demo_project/                演示工程（project.data + 场景 / 模型 / 着色器 + main.js）
├── third_party/                     仓库内置第三方依赖（Amplitude、imgui、stb ...）
├── toolchain/
│   └── grycegc/                     GryceGC 命令行工具链（C++23，替代原 Python 脚本）
└── .github/workflows/ci.yml         CI：构建 + CTest（全部通过才算通过）
```

---

## 依赖

| 依赖 | 来源 | 说明 |
| --- | --- | --- |
| GCC / gcc-libs | MSYS2 UCRT64 / CLion MinGW | 需支持 C++26 |
| CMake ≥ 3.24、Ninja | MSYS2 UCRT64 / CLion MinGW | 构建系统 |
| GLFW | 系统包或 FetchContent | 窗口与输入 |
| GLEW | 系统包或 FetchContent | OpenGL 扩展加载 |
| Box2D | 系统包或 FetchContent | 2D 物理，导入目标 `box2d::box2d` |
| Jolt Physics | 系统包或 FetchContent | 3D 物理，导入目标 `Jolt::Jolt` |
| Assimp | 可选，系统包或 FetchContent | 模型导入（FBX / glTF / DAE / PLY / STL 等；OBJ 走内置加载器），缺失时非 OBJ 格式不可用 |
| Vulkan SDK | 可选 | 缺失时自动关闭 Vulkan 后端（CTest 不依赖它） |
| Amplitude Audio SDK | 仓库内置 `third_party/amplitude` | 预编译静态库，随仓库分发 |
| stb / tinyexr / nlohmann_json / imgui / quickjs | 仓库内置 `third_party` | 图像、EXR、JSON、编辑器 UI、脚本 |

`GRYCE_DEP_FETCH` 控制缺失依赖的处理：`AUTO`（默认，缺哪项拉哪项）/ `ON`（全部源码构建）/ `OFF`（只用系统包，完全离线）。前缀推断、镜像切换与本地源码兜底的完整说明见 [docs/zh/build.md](docs/zh/build.md)。

---

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
```

常用选项：

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `GRYCE_ENABLE_IMGUI` | `ON` | ImGui 编辑器叠加层 |
| `BUILD_TESTING` | `ON` | CTest 用例 |
| `GRYCE_BUILD_TOOLCHAIN` | `ON` | GryceGC 命令行工具链 |
| `GRYCE_ENABLE_D3D12` | `ON` | D3D12 后端（无可用 `d3d12.h` 时静默跳过） |
| `GRYCE_ENABLE_WEBGL` | `OFF` | WebGL（Emscripten）构建，检测到工具链时自动启用 |
| `GRYCE_ENABLE_LTO` | `OFF` | 非 Debug 构建的进程间优化 |
| `GRYCE_ENABLE_X86_V3` | `ON` | x86-64-v3 目标 ISA（AVX2 / FMA / BMI / F16C / LZCNT / POPCNT） |

构建产物：`GryceCore.dll`（抽象层 + 场景 / ECS / 组件 / 反射 / 资源 + 物理音频）、`GryceRenderer.dll`（渲染管线与后端）、`GrycePlatform.dll`（窗口输入）、`GryceRenderSmoke.exe`（渲染演示台）、`GryceApp.exe`（main.js 运行时）、`GryceMathTests.exe` 与 `GryceGC.exe`。构建后 `GryceSyncRuntimeDlls` 会把引擎 DLL、第三方运行库与 MinGW 运行时复制到 exe 同目录，脱离 MSYS2 终端也能运行。

Web 构建（Emscripten）走独立 `build-webgl/`，产物可经 `GryceGC webpack` 打成单文件 HTML，详见 [docs/zh/webgl-backend.md](docs/zh/webgl-backend.md)。

---

## 运行演示

`GryceRenderSmoke` 默认加载 `test/demo_project` 工程：

```bash
build/GryceRenderSmoke.exe --api opengl
```

常用参数：

| 参数 | 说明 |
| --- | --- |
| `--api opengl\|vulkan\|dx12\|webgl` | 选择渲染后端 |
| `--project <dir>` / `--scene <res:/...>` | 指定工程根与入口场景 |
| `--frames N` | 驱动 N 帧后退出（交互模式下表示第 N 帧截图） |
| `--shot <png>` / `--resolution WxH` | 截图输出路径与视口尺寸 |
| `--play` | 交互模式：鼠标锁定转视角，WASD 移动，空格 / 左 Ctrl 升降，Shift 加速，Esc 退出 |
| `--simulate` | 进入 Play 模式驱动系统更新（物理 / 动画），用于脚本化演示与截图 |
| `--cam-pos x y z` / `--cam-look x y z` | 覆盖相机位姿（三个独立数值） |
| `--ssr` / `--no-ssr`、`--ssao` / `--no-ssao`、`--ssil` / `--no-ssil`、`--pcss` / `--no-pcss`、`--env` / `--no-env` | 逐项开关屏幕空间、阴影与环境效果 |
| `--msaa N` / `--no-msaa` | MSAA 档位 |
| `--no-gpu-cull` / `--no-occlusion` | 关闭 GPU 剔除 / 遮挡剔除（走 CPU 回退） |
| `--vsync` / `--no-vsync`、`--fps N` / `--no-fps`、`--fps-limit N`、`--power-saver` | 垂直同步、帧率叠加与限速、省电模式 |
| `--debug-view N` | 调试视图：0 合成、1 albedo、2 normal、3 roughness、4 metallic、5 阴影、6 直接光、7 环境光、8 级联 |
| `--bench N` | 分段 GPU 计时 + CPU 分相 |
| `--list` | 打印场景实体、相机、光源与网格统计 |
| `--help` | 查看完整参数列表 |

---

## 运行脚本入口（GryceApp）

`GryceApp` 是"项目只写 JavaScript"的运行时：宿主固定，窗口、场景、相机、退出都由 `main.js` 调 `engine.*` 决定，项目目录不再需要自己的 `main.cpp`。

```bash
build/GryceApp.exe --project test/demo_project --api opengl
```

演示工程的 `test/demo_project/main.js` 会建窗口、载入主场景、接管相机（鼠标转视角、WASD 移动、Esc 退出）。需要引擎没有的能力时，编一个 `extension.dll` 放到项目根即可给 `engine.*` 增补命名空间（样例 `test/demo_project/extension.cpp` → `engine.game.*`）。

无窗口验证与截图：

```bash
build/GryceApp.exe --project test/demo_project --frames 60 --shot shot.png --api vulkan
```

完整参数与 API 面见 [docs/zh/app-entry.md](docs/zh/app-entry.md)，扩展 ABI 与用法见 [docs/zh/extension.md](docs/zh/extension.md)。

示例：以 Vulkan 后端进入 Play 模式并在第 120 帧截图。

```bash
build/GryceRenderSmoke.exe --api vulkan --simulate --frames 120 --shot build/frame.png
```

Web 端参数经 URL query 传入（`?--api=webgl&--frames=8`），画布尺寸由页面布局决定，`--w/--h` 不再决定视口，详见 [docs/zh/webgl-backend.md](docs/zh/webgl-backend.md)。

---

## 测试

CI 的通过条件只有一条：`ctest` 里的用例全部通过。当前 17 条用例：`GryceMathTests` 承载 7 条数学与资源规则数值核对（vector / matrix / quaternion / camera / ray / material / lod），`GryceGCTests` 承载 10 条工具链用例。`grycegc_*` 用例本地并行会互相干扰，全量验证请 `ctest --test-dir build -j1`。

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

用例清单、设计约束与 CI 配置见 [docs/zh/testing.md](docs/zh/testing.md)。

---

## GryceGC 工具链

`toolchain/` 是引擎的命令行工具链（C++23，自包含零引擎 DLL 依赖），随主工程一起构建，也可独立配置。常用命令（`GryceGC help <command>` 查看完整参数）：

| 命令 | 作用 |
| --- | --- |
| `package` / `verify` | 打包发布目录 / 校验发布包自洽性 |
| `run` | 跨平台启动器（注入 `dll/` 到 PATH 后启动宿主） |
| `copyright` / `meta` | 版权印章 / 元数据读写 |
| `encrypt` / `decrypt` / `keygen` | AES-256-GCM / ChaCha20 加解密 |
| `pak` | 资源包 create / list / extract / verify |
| `bench` / `debug` / `build` / `font` | 基准 / 调试启动 / 构建封装 / 字体重新生成 |
| `webpack` | 把 Emscripten 多文件产物打成单文件 HTML |

```bash
# 典型发布流程
GryceGC package --build-dir build --verify
GryceGC copyright --package dist/GryceEngine --holder "Your Name" --license AGPL-3.0-or-later
GryceGC run --package dist/GryceEngine -- --frames 120 --shot out.png
```

完整命令面见 [docs/zh/toolchain.md](docs/zh/toolchain.md)。

---

## 演示工程

`test/demo_project` 是最小可运行工程：

- `project.data`：工程配置，含入口场景、窗口尺寸、HDR / 色调映射 / 曝光、阴影与环境、SSR / SSAO / SSIL / PCSS 等开关
- `scenes/main.gesc`：主场景，3d66 摄影测量模型（FBX，约 92MB，导入焊接后 1.46M 三角形），含地面、PBR 材质立方体、火焰粒子、相机与光源，以及一个由 Jolt 刚体驱动的自由下坠立方体
- `models/`：Wavefront OBJ 基础网格——`cube.obj`（1x1x1 原点居中）、`quad.obj`（y=0 平面 2x2 地面，场景按 6x 缩放为 12x12，与 `GroundCollider` 半长 6 对齐）、`sphere.obj`（r=0.5 UV 球，备用）。三者均为右手系、逆时针外翻，与 `glFrontFace(GL_CCW)` / `VK_FRONT_FACE_COUNTER_CLOCKWISE` 一致
- `shaders/`：预编译 SPIR-V 兜底产物。GLSL 源统一在 `engine/render/shaders/`（见 `engine/render/core_shaders.cpp` 的运行时查找逻辑）；`*.spv.hash` 仅为本地缓存指纹，不入库

注意：根 `.gitignore` 忽略 MSVC 中间产物的 `*.obj` 规则会连带吞掉模型文件，靠文件末尾的 `!test/demo_project/models/*.obj` 例外放行。新增模型目录时记得同步例外规则。

---

## 许可

项目主体采用 GNU Affero General Public License v3.0（SPDX: `AGPL-3.0-or-later`），全文见 [LICENSE](LICENSE)。`third_party/` 下的第三方库遵循各自原有许可证（如 `third_party/imgui/LICENSE.txt`、`third_party/quickjs/LICENSE` 等）。

注意 AGPL 的网络交互条款（第 13 条）：提供修改后的版本供他人通过网络交互（如在线渲染服务）时，必须同时向使用者提供对应源码；仅本地使用或私有部署不触发该条。
