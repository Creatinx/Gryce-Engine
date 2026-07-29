# Gryce Engine

[![Version](https://img.shields.io/badge/version-0.1.0-blue.svg)](./CMakeLists.txt)
[![Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](./CMakeLists.txt)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](./LICENSE)

> 一个处于原型阶段的 C++ 游戏引擎，采用 Vulkan 默认 / OpenGL 兼容的双渲染后端、ECS 架构、JSON 场景序列化，目标是为中小规模 2D/3D 游戏提供完整的运行时与编辑器工具链。

---

## 特性

- **双后端渲染（RHI）**
  - Vulkan 1.2：**默认后端**，完整 2D 批处理 + 3D PBR + Shadow + HDR + Bloom，支持验证层、VMA、multi-viewport 与扩展动态状态。
  - OpenGL 4.6：兼容后端（旧硬件/调试），同等功能集。
  - DirectX 11 / 12：枚举值已预留（WinNative），尚未实现。
  - PBR 材质工作流：albedo / normal / roughness / metallic / ao / emissive。
  - IBL 环境光照、天空盒、HDR/EXR 环境贴图、tonemapping（Reinhard / ACES）。
  - 阴影：光空间正交盒贴合相机视锥（纹素对齐、深度延伸覆盖屏外投射体）、着色器边缘淡出、自适应 bias + 硬件 slope-scaled depth bias。
  - Project Settings 内可配置渲染 API 与 Render Quality（阴影、环境光、HDR、tonemap、exposure、IBL 强度），持久化到 `project_settings.json`。
- **ECS + 场景系统**
  - Entity-Component-System 架构，类 Godot/Unity 的节点层级；每个场景有且仅有一个合成根节点（`.gesc` 格式版本 2，v1 兼容）。
  - 2D 父链变换：`world_transform_2d()` 组合祖先变换，`Node2D::top_level` 脱离父链，`z_index` 参与绘制排序。
  - `.gesc` JSON 场景格式，支持 `res:/` 虚拟路径、场景热重载与差异保存。
  - Prefab / Prefab Variant：嵌套、覆盖参数、还原模板、场景紧凑引用。
- **资源管线**
  - `AssetManager` 缓存 mesh / texture / material，支持引用计数与 LRU 卸载。
  - 异步加载（`AsyncLoader` 线程池）与 `.gpack` 资源包挂载。
  - 模型：OBJ 内置加载器 + Assimp（FBX / glTF / DAE / PLY / STL）。
  - 纹理：PNG / JPG / BMP / DDS / KTX（BC1~BC7 / ASTC / ETC2）、立方体贴图、HDR/EXR。
  - 字体：TTF 动态图集（stb_truetype）。
  - 材质资源 `.gmat`、导入设置 `.gimport`。
- **动画**
  - 骨骼动画：Skeleton / AnimationClip / Pose、CPU 插值 + GPU Skinning。
  - `SkinnedMeshRenderer` + `AnimatorSystem`，128 骨上限，GL/VK 双后端蒙皮 PBR。
- **物理**
  - 3D：Jolt Physics v5.2.0，刚体、静态体、角色控制器、Hinge/Fixed/Spring/Distance 关节、碎裂。
  - 2D：Box2D v3.0.0，刚体/静态体、圆形/多边形碰撞体、Distance/Spring 关节、角色控制器。
  - 统一 `IPhysicsWorld2D/3D` 抽象 + Raycast。
- **编辑器（MVP）**
  - ImGui Docking 布局：Scene / Game / Hierarchy / Inspector / Project / Console / Animation / Material / Terrain。
  - 场景编辑：自由飞行相机、网格线、AABB 点选拾取、ImGuizmo 移动/旋转/缩放。
  - Play Mode：进入/退出时场景快照与恢复。
  - 资源拖放：模型/纹理/Prefab/场景到视口或 Inspector。
  - 主题系统：Fluent Design 深色/浅色 + 强调色 + 自定义字体，持久化配置。
  - 多语言：中文/英文/日文运行时切换。
  - Godot 风格 Create Entity 对话框（收藏/最近/搜索/过滤/描述）。
  - Hierarchy / File Explorer 右键菜单与全局快捷键（Ctrl+X/C/V/D、F2、Del）。
  - Settings 四分区：主题、语言、VSync + 场景自动保存、快捷键改绑（冲突检测）。
  - Undo/Redo、快捷键体系（Ctrl+S/Z/Y、F 聚焦、Ctrl+P Play Mode）。
- **日志与性能**
  - 异步日志 AsyncLogger：`log()` 入队、worker 线程写出，`GLog` 自动包装 logger。
  - 热路径优化：每帧日志降为 DEBUG、Release 剔除 `GL_CHECK_ERROR`、DrawItem 跨帧复用、重复材质绑定跳过。
- **运行时 UI（2D）**
  - ColorRect、Label、Sprite2D、Circle、Polygon、TileMap、ParticleEmitter2D、ParallaxBackground。
  - 2D 光照：环境光、方向光、点光源、聚光灯、法线贴图、阴影/遮挡。
- **输入**
  - 键盘、鼠标、自定义光标、鼠标锁定（FPS 模式）。
- **工具与自动化**
  - 帧率限制、VSync、NVIDIA `WGL_NV_delay_before_swap`、GPU Busy Spin、截图。
  - 命令行参数：场景加载、无窗口截图、MP4 录制、相机预设。详见 [`docs/CLI.md`](./docs/CLI.md)。

---

## 快速开始

### 环境要求

| 项 | 说明 |
|---|---|
| 平台 | Windows 10/11（主要支持） |
| 编译器 | **MinGW-w64 GCC**（推荐 MSYS2 UCRT64）或 **MSVC**（VS 2022+）|
| 构建工具 | CMake ≥ 3.28，Ninja（推荐） |
| 显卡 | Vulkan 1.2（默认后端）/ OpenGL 4.6（兼容后端）兼容 |
| Vulkan SDK | 构建 Vulkan 后端（默认）所需；无 SDK 时仅 OpenGL 可用 |

> **注意**：本项目当前主要使用 **MSYS2 UCRT64 MinGW-w64** 工具链开发与测试。CMake 会优先自动选择 MinGW；若未找到则自动 fallback 到 MSVC（需打开 VS 2022 x64 Native Tools Prompt）。

### 安装依赖（MSYS2 UCRT64，推荐）

打开 **MSYS2 UCRT64** 终端（开始菜单中搜索 "MSYS2 UCRT64"）：

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-glew \
          mingw-w64-ucrt-x86_64-glfw
```

### 构建

#### 方式 A：MSYS2 UCRT64 终端（推荐）

在 MSYS2 UCRT64 终端中 cd 到项目目录后：

```bash
# Debug（默认 Vulkan 后端，--opengl 可切换兼容后端）
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug

# Release
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

#### 方式 B：普通 PowerShell / CMD（显式指定编译器）

若未使用 MSYS2 终端，CMake 会自动尝试检测系统默认编译器。为避免自动选中 MSVC 导致缺少 `rc.exe` 错误，请显式指定 MinGW 编译器：

```powershell
# 确保 gcc 在 PATH 中（如 C:\msys64\ucrt64\bin 已加入系统 PATH）
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build/debug
```

> 如果 `gcc` 不在 PATH 中，请使用完整路径：
> `-DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/g++.exe`

#### 方式 C：使用 build.py（Python 脚本，推荐）

```powershell
# 默认 Debug，自动下载缺失依赖
python build.py

# Release / RelWithDebInfo / MinSizeRel
python build.py Release

# 仅下载并解压依赖（不构建）
python build.py --setup-deps

# 清理构建产物（保留 deps/ 缓存）
python build.py --clean

# 完全清理（包括 deps/，下次构建会重新下载）
python build.py --clean-all

# 指定并行任务数
python build.py --jobs 8

# 自定义构建目录前缀（默认 build/）
python build.py --build-dir build-mingw

# 使用 CMake 默认编译器检测（不锁定 MinGW/MSVC）
python build.py --no-lock
```

> 首次构建时，`build.py` 会调用 `tools/deps_manager.py` 下载 assimp/glfw/box2d/jolt/googletest 等源码到 `build/deps/` 目录，原始 tar.gz 缓存到 `deps_cache/`。`deps_cache/` 与 `build/deps/` 均**不上传 Git**，首次 clone 后由脚本自动下载。

#### 方式 D：MSVC（Visual Studio 2022+）

打开 **x64 Native Tools Command Prompt for VS 2022** 后执行：

```powershell
# 方式 D1：直接 cmake
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug

# 方式 D2：使用 build.py（自动检测 cl.exe）
python build.py
```

> MSVC 已作为 CMake 的 auto-detect fallback 路径支持。部分 MinGW 特定逻辑（如 `libgcc` 运行时 DLL 复制）在 MSVC 下会被自动跳过。

#### 方式 E：Visual Studio（VS2026）

仓库根目录的 `CMakeSettings.json` 已内置 `x64-Debug` / `x64-Release`（Ninja）配置，可直接用 Visual Studio 的"打开文件夹"工作流；也可以命令行生成 VS2026 解决方案：

```powershell
cmake -S . -B out/vs -G "Visual Studio 18 2026" -A x64
# 生成 out/vs/GryceEngine.slnx，用 VS2026 打开即可
```

构建完成后，可执行文件位于（以 `build.py` 默认目录为例）：

- `build/Debug/bin/Debug/3dtest.exe`
- `build/Debug/bin/Debug/gt2dDemo.exe`
- `build/Debug/bin/Debug/gryce-engine.exe`
- `build/Debug/bin/Debug/gryce_tests.exe`

### 运行

```bash
# 3D 综合演示（默认 Vulkan 后端）
./build/Debug/bin/Debug/3dtest.exe

# 3D 综合演示（OpenGL 兼容后端）
./build/Debug/bin/Debug/3dtest.exe --opengl

# 2D 平台跳跃演示
./build/Debug/bin/Debug/gt2dDemo.exe

# 编辑器（项目根自动从可执行文件位置向上探测，可在 File > Load Project 中切换）
./build/Debug/bin/Debug/gryce-engine.exe

# 单元测试
./build/Debug/bin/Debug/gryce_tests.exe
```

---

## 控制说明

### 3D 演示（3dtest）

| 按键 | 功能 |
|------|------|
| `W/A/S/D` | 移动 |
| `Space` | 上升 |
| `Left Ctrl` | 下降 |
| `Left Shift` | 冲刺 |
| `鼠标移动` | 视角 |
| `Tab` | 锁定/释放鼠标 |
| `ESC` | 释放鼠标并暂停视角控制 |
| `R` | 重置场景 |
| `鼠标左键` | 拾取并拖拽物体（重力枪） |
| `F1` | 切换线框模式（OpenGL only） |
| `F2` | 触发 Cube 碎裂演示 |
| `关闭窗口` | 退出程序 |

### 2D 演示（gt2dDemo）

| 按键 | 功能 |
|------|------|
| `W/A/S/D` 或 `方向键` | 移动/跳跃 |
| `ESC` | 暂停/菜单 |

---

## 项目结构

```
Gryce-Engine/
├── cmake/                  # CMake 工具脚本（编译器选项、依赖解析）
├── core/                   # 引擎核心静态库（gryce_core）
│   ├── animation/          # 骨骼动画数据结构与 GPU Skinning
│   ├── assets/             # 资源加载器（OBJ、Assimp、纹理、字体）
│   ├── audio/              # 音频系统（miniaudio）
│   ├── components/         # ECS 组件（3D + 2D）
│   ├── ecs/                # ECS 系统（World、System、调度）
│   ├── math/               # 数学库（Vector、Matrix、Quaternion）
│   ├── physics/            # 物理抽象与 Box2D / Jolt 后端
│   ├── platform/           # 窗口、输入、光标
│   ├── reflection/         # 组件反射（编辑器 Inspector 前置）
│   ├── render/             # RHI、渲染管线、OpenGL/Vulkan 后端
│   ├── resources/          # 资源路径、项目根解析
│   ├── scene/              # Scene、Entity、Transform 层级、Prefab
│   └── utils/              # 日志（异步 AsyncLogger）、帧率限制、工具类
├── docs/                   # 文档（ARCHITECTURE、STATUS、PROJECT_LAYOUT、CORE_API、TODO、CLI）
├── editor/                 # 编辑器可执行文件（gryce-engine.exe）
│   ├── panels/             # 编辑器面板
│   ├── ui/                 # 编辑器窗口与主题
│   └── import/             # 导入设置编辑器
├── examples/               # 示例游戏项目
│   ├── common/             # 示例公共框架
│   ├── 3dtest/             # 3D 综合演示
│   ├── gt2dDemo/           # 2D 平台跳跃演示
│   ├── demo_sprite2d/      # 2D Sprite2D 演示
│   ├── demo_shapes2d/      # 2D 形状演示
│   ├── demo_lighting2d/    # 2D 光照演示
│   ├── demo_tilemap2d/     # 2D 瓦片地图演示
│   ├── demo_particles2d/   # 2D 粒子演示
│   ├── demo_physics2d/     # 2D 物理演示
│   ├── demo_character2d/   # 2D 角色控制器演示
│   ├── demo_joints2d/      # 2D 关节演示
│   ├── demo_physics3d/     # 3D 物理演示
│   ├── demo_character3d/   # 3D 角色控制器演示
│   ├── demo_joints3d/      # 3D 关节演示
│   ├── demo_fracture/      # 3D 碎裂演示
│   ├── demo_lighting3d/    # 3D 光照演示
│   ├── demo_audio3d/       # 3D 音频演示
│   ├── demo_scene_serializer/ # 场景序列化演示
│   └── demo_skinned3d/     # 3D 骨骼动画演示
├── tests/                  # 单元测试（GTest）
├── third_party/            # 第三方库（imgui、json、stb、miniaudio、imguizmo）
├── tools/                  # 工具脚本（deps_manager.py、gen_skybox.py）
├── deps_cache/             # 依赖源码本地缓存（gitignore）
├── CMakeLists.txt          # 根 CMake
├── README.md               # 本文件
└── build.py                # 一键构建脚本
```

---

## 架构概览

```
┌─────────────────────────────────────────────────────────────┐
│                        Application                          |
│                (3dtest / gt2dDemo / Editor)                 │
└─────────────────────────────┬───────────────────────────────┘
                              │
┌-────────────────────────────▼───────────────────────────────┐
│                      gryce_core                             │
│   ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌───────────────┐  │
│   │  Scene  │  │   ECS   │  │  Assets │  │    Input      │  │
│   │Entity   │  │Systems  │  │Pipeline │  │   Window      │  │
│   └────┬────┘  └────┬────┘  └────┬────┘  └───────┬───────┘  │
│        │            │            │               │          │
│        └────────────┴────────────┘               │          │
│                     │                            │          │
│        ┌────────────▼────────────┐               │          │
│        │    RenderContext        │◄──────────────┘          │
│        │  (Command Buffer Queue) │                          │
│        └────────────┬────────────┘                          │
│                    │                                        │
│        ┌────────────▼────────────┐                          │
│        │      Render Thread      │                          │
│        └────────────┬────────────┘                          │
│                     |                                       │
│        ┌────────────▼────────────┐                          │
│        │  RHI: Vulkan（默认）/ OpenGL（兼容）│                          │
│        └─────────────────────────┘                          │
└─────────────────────────────────────────────────────────────┘
```

更多细节参见 [`docs/ARCHITECTURE.md`](./docs/ARCHITECTURE.md)。

---

## 开发计划

详见 [`docs/STATUS.md`](./docs/STATUS.md)。

---

## 贡献

目前项目处于早期原型阶段，API 不稳定。欢迎提交 Issue 与 PR。

---

## 许可证

MIT License（详见 [LICENSE](./LICENSE)）。
