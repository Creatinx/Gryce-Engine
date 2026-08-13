# Gryce Engine

[![Version](https://img.shields.io/badge/version-0.1.0-blue.svg)](./CMakeLists.txt)
[![Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](./CMakeLists.txt)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](./LICENSE)

> 一个处于原型阶段的 C++23 游戏引擎：Vulkan（默认）/ OpenGL（兼容）双渲染后端、ECS 架构、JSON 场景序列化。核心按模块拆分为多个 DLL，只通过**纯 C API**（`extern "C"`）对外服务；编辑器为 WPF（C#）实现，与核心完全解耦。

---

## 文档

| 文档 | 内容 |
|---|---|
| [C API 调用文档](./docs/C-API调用文档.md) | C API 完整调用文档：模块划分、生命周期、命令队列、逐函数参考、C / C# 示例 |
| [已实现功能](./docs/已实现功能.md) | 已实现功能清单（模块、组件、系统、示例、测试、工具）与未实现 / TODO |
| [架构说明](./docs/架构说明.md) | 模块架构、线程模型、数据流 |

---

## 特性

- **双后端渲染（RHI）**
  - Vulkan 1.2：**默认后端**，2D 批处理 + 3D PBR + Shadow + HDR + Bloom，支持验证层、VMA、多视口与扩展动态状态。
  - OpenGL 4.6：兼容后端（旧硬件 / 调试），功能与 Vulkan 后端同步维护。
  - DirectX 11 / 12：枚举值已预留（`GRYCE_RENDER_API_DX11 / DX12`），尚未实现。
  - PBR 材质工作流：albedo / normal / roughness / metallic / ao / emissive 六张贴图槽 + 颜色参数。
  - IBL 环境光照、天空盒、HDR/EXR 环境贴图、tonemapping（Reinhard / ACES）。
  - 阴影：光空间正交盒贴合相机视锥（纹素对齐、深度延伸覆盖屏外投射体）、着色器边缘淡出、自适应 bias + 硬件 slope-scaled depth bias。
  - 渲染质量可配置（阴影、环境光、HDR、tonemap、exposure、IBL 强度），持久化到 `project_settings.json`。
- **ECS + 场景系统**
  - Entity-Component-System 架构，类 Godot/Unity 的节点层级；每个场景有且仅有一个合成根节点。
  - `.gesc` JSON 场景格式（版本 2，兼容 v1），支持 `res:/` 虚拟路径、场景热重载与差异保存。
  - Prefab / Prefab Variant：嵌套、覆盖参数、还原模板、场景紧凑引用。
  - 2D 父链变换、`top_level` 脱离父链、`z_index` 参与绘制排序。
- **资源管线**
- `AssetManager` 缓存 mesh / texture / material，引用计数 + LRU 卸载；`AsyncLoader` 异步加载；`.gpack/.gpkg` 资源包挂载（GryceGC 打包产物，`GCore_Init` 自动挂载，真实文件优先、包内提取兜底）。
  - 模型：OBJ 内置加载器 + Assimp（FBX / glTF / DAE / PLY / STL）。
  - 纹理：PNG / JPG / BMP / DDS / KTX（BC1~BC7 / ASTC / ETC2）、立方体贴图、HDR/EXR；资源路径统一 UTF-8 处理，支持中文文件名。
  - 字体：TTF 动态图集（stb_truetype）；材质资源 `.gmat`、导入设置 `.gimport`。
- **骨骼动画**
  - Skeleton / AnimationClip / Pose，CPU 插值 + GPU Skinning；128 骨上限，GL/VK 双后端蒙皮 PBR。
  - `SkinnedMeshRenderer` + `AnimatorSystem`，编辑器 Animation 面板可查看片段与时长。
- **物理**
  - 3D：Jolt Physics v5.2.0 — 刚体、静态体、角色控制器、Hinge/Fixed/Spring/Distance 关节、碎裂。
  - 2D：Box2D v3.0.0 — 刚体/静态体、圆形/多边形碰撞体、Distance/Spring 关节、角色控制器。
  - 统一 `IPhysicsWorld2D/3D` 抽象 + Raycast；`PhysicsSystem3D/2D` 在 Play Mode 中真实模拟。
- **音频**：miniaudio 引擎，`AudioSource` / `AudioListener`（3D 空间音效）、变速器 `TimeStretcher`。
- **运行时 UI（2D）**
  - ColorRect、Label、Sprite2D、Circle、Polygon、TileMap、ParticleEmitter2D、ParallaxBackground、Skybox2D、Camera2D。
  - 2D 光照：环境光、方向光、点光源、聚光灯、法线贴图、阴影/遮挡。
- **输入**：键盘、鼠标、自定义光标、鼠标锁定（FPS 模式）。
- **WPF 编辑器（MVP）**
  - `editor/GryceEngine.Editor.csproj`（.NET Framework 4.8，iNKORE Fluent 主题），仅通过模块 DLL 的 **C API** 与 Core 通信。
  - Hierarchy / Inspector（反射字段编辑）/ Viewport / Project / Console / Animation / Toolbar 面板。
  - Play Mode：真实驱动物理与骨骼动画，停止时从快照恢复场景（类 Unity 行为）。
  - 材质编辑器：PBR 参数 + 贴图槽，改动即时生效；Godot 风格 Create Entity 对话框。
  - 深色/浅色主题、中/英本地化运行时切换并持久化；快捷键体系（Ctrl+S/Z/Y/N、Delete、F2、F、W/E/R、Ctrl+P、Ctrl+X/C/V/D）。
- **日志与性能**
  - 异步日志 `AsyncLogger`（内存 Sink 转发到编辑器 Console），帧率限制、VSync、NVIDIA `WGL_NV_delay_before_swap`、GPU Busy Spin、截图。
  - 热路径优化：每帧日志降级、Release 剔除 GL 错误检查、DrawItem 跨帧复用、重复材质绑定跳过、同步渲染模式下每帧网格上传预算（30/帧）。
- **脚本系统（规划）**：计划引入 Lua 脚本层（玩法逻辑、组件行为、热重载）；引擎核心继续以 C++ 实现并通过绑定层暴露 API。

---

## 快速开始

### 环境要求

| 项 | 说明 |
|---|---|
| 平台 | Windows 10/11（主要支持） |
| 编译器 | **MinGW-w64 GCC**（推荐 MSYS2 UCRT64）或 **MSVC**（VS 2022+） |
| 构建工具 | CMake ≥ 3.28，Ninja（推荐） |
| 显卡 | Vulkan 1.2（默认后端）/ OpenGL 4.6（兼容后端）兼容 |
| Vulkan SDK | 构建 Vulkan 后端（默认）所需；无 SDK 时仅 OpenGL 可用 |
| Python | 使用 `build.py` 时需要（依赖下载脚本） |

> 本项目主要使用 **MSYS2 UCRT64 MinGW-w64** 工具链开发与测试。CMake 会自动优先选择 MinGW；未找到时 fallback 到 MSVC（需在 VS x64 Native Tools Prompt 中运行）。

### 安装依赖（MSYS2 UCRT64，推荐）

打开 **MSYS2 UCRT64** 终端（开始菜单搜索 "MSYS2 UCRT64"）：

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-glew \
          mingw-w64-ucrt-x86_64-glfw
```

首次构建时，`build.py` / `tools/deps_manager.py` 会自动下载并解压以下源码到 `build/deps/`（原始压缩包缓存到 `deps_cache/`，均不入 Git）：

| 依赖 | 版本 | 用途 |
|---|---|---|
| GLFW | 3.4 | 窗口/上下文 |
| GLEW | 2.2.0 | OpenGL 扩展加载 |
| Assimp | 5.4.3 | FBX/glTF/DAE/PLY/STL 模型导入 |
| Box2D | 3.0.0 | 2D 物理 |
| Jolt Physics | 5.2.0 | 3D 物理 |
| GoogleTest | 1.15.2 | 单元测试 |

仓库内已自带：imgui、imguizmo、nlohmann/json、stb、miniaudio、tinyexr。

### 构建

#### 方式 A：MSYS2 UCRT64 终端（推荐）

```bash
# Debug（默认 Vulkan 后端；demo 用 --opengl 切换兼容后端）
cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Debug

# Release
cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/Release
```

#### 方式 B：普通 PowerShell / CMD（显式指定 MinGW）

```powershell
cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build/Debug
```

> 若 `gcc` 不在 PATH：`-DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe`

#### 方式 C：build.py（推荐）

```powershell
python build.py                    # 默认 Debug，自动下载缺失依赖
python build.py Release            # Release / RelWithDebInfo / MinSizeRel
python build.py --setup-deps       # 仅下载并解压依赖
python build.py --clean            # 清理构建产物（保留 deps/）
python build.py --clean-all        # 完全清理（含 deps/，下次重新下载）
python build.py --jobs 8           # 并行任务数
python build.py --build-dir build-mingw
python build.py --no-lock          # 使用 CMake 默认编译器检测
python build.py --msvc             # 强制 MSVC + Visual Studio 2026 generator
python build.py --offline          # 离线模式（仅用本地缓存依赖）
python build.py --verbose          # 输出 ninja 详细日志
```

`build.py` 构建结束后会把生成的 `GryceEngine.slnx` 同步到仓库根目录，供 VS2026 直接打开。

#### 方式 D：MSVC（Visual Studio 2022+ / 2026）

```powershell
# 在 x64 Native Tools Command Prompt 中
cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Debug

# 或使用 build.py（自动检测 cl.exe）
python build.py
```

#### 方式 E：Visual Studio 2026

仓库根目录的 `CMakeSettings.json` 已内置 `x64-Debug` / `x64-Release`（Ninja）配置，可直接"打开文件夹"；也可命令行生成解决方案：

```powershell
cmake -S . -B out/vs -G "Visual Studio 18 2026" -A x64
```

### 构建产物

（以 `build.py` 默认目录为例）

```text
build/Debug/bin/Debug/
├── GryceCore.dll / GrycePlatform.dll / GryceRenderer.dll / GrycePhysics.dll
├── 3dtest.exe          # 3D 综合演示
├── 2dDemo.exe          # 2D 平台跳跃演示（场景驱动，编辑器可编辑关卡）
└── gryce_tests.exe     # 单元测试

editor/bin/x64/<Config>/net48/
└── GryceEngine.Editor.exe   # WPF 编辑器（CMake 构建时自动复制 4 个原生 DLL + glfw 到该目录）
```

### 发布（GryceGC）

```powershell
# 构建打包工具（随主构建一起生成 build/bin/<Config>/grycegc.exe）
cmake --build build --target GryceGC --config Release

# 打包（Debug/Release 均可）：不复制 res/，而是将资源按类别打包为多个 .gpkg
build/bin/Release/grycegc.exe --project examples/3dtest --name MyGame --build-dir build --config Release --out build/game --author "Your Name"

# 2D 演示（2dDemo）同样是一个 GryceGC-A 项目，可直接打包运行
build/bin/Release/grycegc.exe --project examples/2dDemo --name 2dDemo --build-dir build --config Release --out build/game --author "Your Name"

# 运行产物（无 res/ 目录）：
#   <out>/<name>/MyGame.exe        游戏入口
#   <out>/<name>/runtime/          核心运行时 DLL + MSVC/MinGW/GCC 运行时（兜底）
#   <out>/<name>/assets/*.gpkg     资源包（GPAK）
#   <out>/<name>/gdata             包元数据（源文件记录 + 64 字节 SHA-512 密钥 + 作者）
build/game/MyGame/MyGame.exe                                          # 项目根默认取 exe 所在目录
build/game/MyGame/MyGame.exe --project build/game/MyGame --scene res:/scenes/main.gesc
```

GryceGC 是 C++ 工具（`tools/grycegc/`），通过 GryceCore 的 GPack C API（`GCore_PackCreate/AddFile/Write`）生成 GPAK 格式的 `.gpkg` 资源包，并生成 `gdata` 包元数据（每个源文件的 SHA-256、由源记录派生的 64 字节 SHA-512 密钥、作者/项目/时间等）。Core 启动时自动挂载 `assets/` 与项目根下的 `.gpkg/.gpack`；游戏入口对核心 DLL 延迟加载，从 `runtime/` 子目录解析。着色器、场景、脚本、网格、纹理等加载管线统一走 `AssetManager::resolve_for_reading`：真实文件优先，包内提取兜底；shader 每次加载都会重新编译（无预编译缓存依赖）。

运行时加载策略：游戏启动时优先使用**系统的 VC++ 运行时**（从 System32 显式预加载，引擎 DLL 会绑定到系统版本），只有当系统缺少该运行时，才回退使用 `runtime/` 里打包的 MSVC/MinGW/GCC 运行时。

### 运行

```powershell
# 3D 综合演示（默认 Vulkan 后端）
./build/Debug/bin/Debug/3dtest.exe

# 3D 综合演示（OpenGL 兼容后端）
./build/Debug/bin/Debug/3dtest.exe --opengl

# 2D 平台跳跃演示
./build/Debug/bin/Debug/2dDemo.exe

# WPF 编辑器（原生 DLL 已由 CMake 自动部署）
./editor/bin/x64/Debug/net48/GryceEngine.Editor.exe

# 单元测试
./build/Debug/bin/Debug/gryce_tests.exe
```

#### 演示程序命令行参数

| 参数 | 说明 |
|---|---|
| `--vulkan` | 使用 Vulkan 后端（默认） |
| `--opengl` | 使用 OpenGL 兼容后端 |
| `--vulkan-validation` | 启用 Vulkan 验证层 |
| `--screenshot` | 首帧后请求截图（写入引擎根目录 `screenshot_vulkan.bmp` / `screenshot_opengl.bmp`） |
| `--screenshot-delay <秒>` | 延迟指定秒数后截图 |
| `--auto-close <秒>` | 运行指定秒数后自动退出（CI 用） |

---

## 控制说明

### 3D 演示（3dtest）

| 按键 | 功能 |
|------|------|
| `W/A/S/D`、`方向键` | 移动 |
| `Space` / `Left Ctrl` | 上升 / 下降 |
| `Left Shift` | 冲刺 |
| `Right Shift` | 角色控制器跳跃（角色演示） |
| `鼠标移动` | 视角 |
| `Tab` | 锁定 / 释放鼠标 |
| `ESC` | 退出程序 |
| `R` | 重置 3D 场景 |
| `鼠标左键` | 拾取并拖拽物体（重力枪） |
| `F1` | 切换线框模式（仅 OpenGL 后端） |
| `F2` | 触发 Cube 碎裂演示 |
| `F3` | 保存当前场景到 `res:/scenes/main.gesc` |
| `F4` | 重建触发器演示 |

### 2D 演示（2dDemo）

场景驱动的 2D 平台跳跃游戏：3 个编辑器可加载关卡（`scenes/level_*.gesc` + `levels.json`），
玩法逻辑全部写在 GryceSRT Lua 脚本（`scripts/*.lua`）；包含关卡过关系统、昼夜循环
（白天/黄昏/夜晚/黎明）、2D 光照、Box2D 物理、巡逻敌人、可升级枪械（鼠标瞄准射击、
拾取升级三发）与终点过关条件。同样的脚本在编辑器 Play 模式、独立 exe 与打包产物中运行。

| 按键 | 功能 |
|------|------|
| `A/D`、`方向键` | 左右移动 |
| `Space` / `W` / `上方向键` | 跳跃 |
| `鼠标左键` / `Z` | 射击（鼠标瞄准） |
| `R` | 重新开始游戏 |
| `ESC` | 退出程序 |

---

## 项目结构

```text
Gryce-Engine/
├── cmake/                  # CMake 工具脚本（编译器选项、依赖解析、着色器编译）
├── core/                   # 引擎核心源码（4 个模块化 DLL）
│   ├── api/                # C API 实现（core/entity/component/scene/asset/material/animator/physics/render/platform）
│   ├── GryceCore/          # GryceCore.dll 公共 C API 头文件（types/core/entity/component/scene/asset/material/animator）
│   ├── GryceRenderer/      # GryceRenderer.dll 公共 C API 头文件（render/viewport）
│   ├── GrycePlatform/      # GrycePlatform.dll 公共 C API 头文件（window/input）
│   ├── GrycePhysics/       # GrycePhysics.dll 公共 C API 头文件（physics）
│   ├── animation/          # 骨骼动画（Skeleton/AnimationClip/Pose）
│   ├── assets/             # 资源加载器（OBJ/Assimp/纹理/压缩纹理/异步加载）
│   ├── audio/              # 音频（miniaudio）
│   ├── components/         # ECS 组件（3D + 2D + 物理 + 音频）
│   ├── ecs/                # ECS（World/System/ComponentStore + 内置系统）
│   ├── math/               # 数学库（Vector/Matrix/Quaternion/Camera/Ray）
│   ├── physics/            # 物理抽象与 Box2D / Jolt 后端
│   ├── platform/           # 窗口、输入、光标（GLFW）
│   ├── reflection/         # 组件反射（编辑器 Inspector 前置）
│   ├── render/             # 渲染核心 + OpenGL/Vulkan 后端（opengl/、vulkan/）
│   ├── resources/          # 资源路径（res:/）、项目根、gpack
│   ├── scene/              # Scene/Entity/Transform 层级/Prefab/序列化
│   └── utils/              # 日志（AsyncLogger）、帧率限制
├── docs/                   # 文档（C API / GryceSRT / GryceGC-A / 已实现功能 / 架构说明）
├── editor/                 # WPF 编辑器（C#，.NET Framework 4.8）
│   ├── src/Native/         # C API 的 P/Invoke 包装（与头文件一一对应）
│   ├── src/Services/       # EngineService（引擎生命周期、命令下发、自动保存）
│   ├── src/ViewModels/     # EditorViewModel 等（回调注册、面板刷新）
│   └── src/Views/          # 面板 XAML（Hierarchy/Inspector/Viewport/Project/Console/Animation/...）
├── examples/               # 示例游戏项目
│   ├── common/             # 示例公共框架（app_launcher、调试面板）
│   ├── 3dtest/             # 3D 综合演示（PBR/阴影/物理/关节/角色/碎裂/动画/音频/场景热重载）
│   └── 2dDemo/             # 2D 平台跳跃（关卡/昼夜/光照/物理/敌人/枪械/过关）
├── tests/                  # 单元测试（GTest）
├── third_party/            # 第三方库源码（imgui、imguizmo、json、stb、miniaudio、tinyexr）
├── tools/                  # 工具脚本（deps_manager.py、gen_skybox.py、gen_skinned_fixture.py）
├── deps_cache/             # 依赖源码本地缓存（gitignore）
├── CMakeLists.txt          # 根 CMake
├── CMakeSettings.json      # VS "打开文件夹" 配置
├── build.py                # 一键构建脚本
├── Directory.Build.props   # MSBuild 全局属性（编辑器 C# 工程）
├── GryceEngine.slnx        # VS2026 解决方案（build.py 自动同步）
└── GryceECLib_Integration_Plan.md  # 模块化 Core/Editor 分离的历史设计方案
```

---

## 架构概览

```text
┌────────────────────────────────────────────────────────────┐
│ Application（3dtest / 2dDemo）— C++，直接链接引擎内部 API  │
└───────────────────────────────┬────────────────────────────┘
                                │
┌───────────────────────────────▼────────────────────────────┐
│ WPF Editor（C#）: Views → ViewModels → EngineService        │
│                     → Native (P/Invoke)                     │
└───────────────────────────────┬────────────────────────────┘
                                │  C ABI（extern "C"，cdecl）
┌───────────────────────────────▼────────────────────────────┐
│ GryceCore.dll  GryceRenderer.dll  GrycePlatform.dll  GrycePhysics.dll │
│  Scene/Entity     OpenGL/Vulkan 后端   Window/Input/Cursor  Jolt/Box2D │
│  ECS/Reflection   RenderSystems       GLFW                  PhysicsSystem │
│  Assets/Animation RenderPipeline                             │
│  Audio/Math/UI    ImGui Backend                              │
└───────────────────────────────┬────────────────────────────┘
                                │
                RenderContext（命令缓冲队列）
                RenderThread / 同步模式（编辑器内嵌 HWND）
                RHI: Vulkan（默认）/ OpenGL（兼容）
```

更完整的模块边界、线程模型与调用时序见 [架构说明](./docs/架构说明.md)；C API 用法见 [C API 调用文档](./docs/C-API调用文档.md)。

---

## 开发约定

- **C API 是 Editor 与 Core 之间的唯一通道**：任何新增的编辑器功能都应优先以 C API 暴露，避免直接 `#include` 内部头文件。
- **手动复制链接库必须同步到 CMake**：每当手动向编辑器输出目录复制某个原生 DLL（例如新增第三方库），必须在 `core/CMakeLists.txt` 中通过 `gryce_copy_dll_to_editor(<target>)` 或等价的 `add_custom_command(TARGET ... POST_BUILD ...)` 添加自动复制规则，保证全新 CMake 构建即可完整部署。
- **公共头文件集中在模块目录**：`core/GryceCore/`、`core/GryceRenderer/`、`core/GrycePlatform/`、`core/GrycePhysics/`，新 API 的声明与实现一一对应。
- **提交信息遵循 Conventional Commits**（`feat:` / `fix:` / `docs:` / `perf:` 等）。

---

## 已知限制与下一步

- 编辑器 Viewport 目前以 **OpenGL 后端** 驱动（内嵌 GLFW HWND），Vulkan 后端由 demo 与运行时使用；Vulkan 编辑器集成待跟进。
- 世界空间变换查询（`GEntity_GetWorldPosition/Rotation/Scale`）尚未实现（返回 -1）。
- GameView 与 SceneView 目前共用同一管线/纹理；独立 GameView FBO 待实现。
- `GPhysics_Raycast` 暂未把命中体映射回实体（`out_entity` 恒为 0）。
- 渲染显示模式（线框等）、Gizmo 操作命令（`ECMD_GIZMO_*`）为占位实现。
- 大规模 3D 场景（>1000 entity）尚未启用 GPU Instancing。
- 脚本系统处于规划阶段（计划 LuaJIT/sol2，暴露 Scene/Entity/Component API）。

详见 [已实现功能](./docs/已实现功能.md)。

---

## 贡献

目前项目处于早期原型阶段，API 不稳定。欢迎提交 Issue 与 PR。

---

## 许可证

MIT License（详见 [LICENSE](./LICENSE)）。
