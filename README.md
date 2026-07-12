# Gryce Engine

[![Version](https://img.shields.io/badge/version-0.1.0-blue.svg)](./CMakeLists.txt)
[![Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](./CMakeLists.txt)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](./LICENSE)

> 一个处于原型阶段的 C++ 游戏引擎，采用 OpenGL / Vulkan 双渲染后端、ECS 架构、JSON 场景序列化，目标是为中小规模 2D/3D 游戏提供完整的运行时与编辑器工具链。

---

## 特性

- **双后端渲染**
  - OpenGL 4.6：完整 2D 批处理 + 3D PBR + Shadow + HDR。
  - Vulkan 1.2：同等功能集，支持验证层与扩展动态状态。
- **ECS + 场景系统**
  - Entity-Component-System 架构，类 Godot/Unity 的节点层级。
  - `.gesc` JSON 场景格式，支持 `res:/` 虚拟路径。
- **资源管线**
  - AssetManager 缓存 mesh / texture / material。
  - OBJ 模型加载、PNG/JPG/BMP 纹理、TTF 字体图集。
  - 材质系统支持 albedo / normal / roughness / metallic / ao。
- **物理**
  - 3D：刚体、静态体、AABB 碰撞、碎裂（DestructibleBody）。
  - 2D：基础刚体/静态体/AABB 碰撞。
- **UI**
  - ImGui 编辑器面板与调试界面。
  - 运行时 2D UI 组件：ColorRect、Label、Sprite2D、Circle、Polygon、TileMap 等。
- **输入**
  - 键盘、鼠标、自定义光标、鼠标锁定（FPS 模式）。
- **工具**
  - 帧率限制、VSync、GPU Busy Spin、截图。

---

## 快速开始

### 环境要求

| 项 | 说明 |
|---|---|
| 平台 | Windows 10/11（主要支持） |
| 编译器 | **MinGW-w64 GCC**（推荐 MSYS2 UCRT64）或 **MSVC**（VS 2022+）|
| 构建工具 | CMake ≥ 3.28，Ninja（推荐） |
| 显卡 | OpenGL 4.6 / Vulkan 1.2 兼容 |
| Vulkan SDK | 可选，仅当使用 `--vulkan` 时需要 |

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
# Debug（默认 OpenGL 后端）
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
# 默认 Debug，自动预下载缺失依赖（带进度条）
python build.py

# Release
python build.py Release

# 清理后重新构建
python build.py --clean

# 指定并行任务数
python build.py --jobs 8

# 不使用预下载（让 CMake 自行下载）
python build.py --no-prefetch

# 自定义缓存目录
python build.py --cache-dir D:/gryce_deps_cache
```

> 首次构建时，`build.py` 会预下载 assimp/glfw 等 tar.gz 依赖到 `deps_cache/` 目录，并显示进度条。若安装了 `rich` 则使用彩色进度条，否则显示 ASCII 百分比条。安装 rich：`pip install rich`。

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

构建完成后，可执行文件位于：

- `build/debug/bin/Debug/3dtest.exe`
- `build/debug/bin/Debug/gt2dDemo.exe`
- `build/debug/bin/Debug/gryce_tests.exe`

### 运行

```bash
# 3D 物理/渲染演示（OpenGL 默认）
./build/debug/bin/Debug/3dtest.exe

# 3D 演示（Vulkan 后端）
./build/debug/bin/Debug/3dtest.exe --vulkan

# 2D 平台跳跃演示
./build/debug/bin/Debug/gt2dDemo.exe

# 单元测试
./build/debug/bin/Debug/gryce_tests.exe
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
├── cmake/                  # CMake 工具脚本
├── core/                   # 引擎核心静态库（gryce_core）
│   ├── assets/             # 资源加载器（OBJ、纹理、字体）
│   ├── audio/              # 音频系统
│   ├── components/         # ECS 组件（3D + 2D）
│   ├── ecs/                # ECS 系统（World、System、调度）
│   ├── math/               # 数学库（Vector、Matrix、Quaternion）
│   ├── physics/            # 物理抽象与实现
│   ├── platform/           # 窗口、输入、光标
│   ├── render/             # RHI、渲染管线、OpenGL/Vulkan 后端
│   ├── resources/          # 资源路径、项目根解析
│   ├── scene/              # Scene、Entity、Transform 层级
│   └── utils/              # 日志、帧率限制、工具类
├── docs/                   # 文档（ARCHITECTURE、STATUS、PROJECT_LAYOUT、CORE_API）
├── editor/                 # 编辑器入口
├── examples/               # 示例游戏项目
│   ├── 3dtest/             # 3D 演示项目
│   └── gt2dDemo/           # 2D 演示项目
├── tests/                  # 单元测试
├── third_party/            # 第三方库（imgui、json、stb、miniaudio）
├── CMakeLists.txt          # 根 CMake
├── README.md               # 本文件
└── build.py                # 一键构建脚本
```

---

## 架构概览

```
┌─────────────────────────────────────────────────────────────┐
│                        Application                           │
│                 (3dtest / gt2dDemo / Editor)                 │
└─────────────────────────────┬───────────────────────────────┘
                              │
┌─────────────────────────────▼───────────────────────────────┐
│                       gryce_core                             │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌───────────────┐  │
│  │  Scene  │  │   ECS   │  │  Assets │  │    Input      │  │
│  │Entity   │  │Systems  │  │Pipeline │  │   Window      │  │
│  └────┬────┘  └────┬────┘  └────┬────┘  └───────┬───────┘  │
│       │            │            │               │          │
│       └────────────┴────────────┘               │          │
│                    │                            │          │
│       ┌────────────▼────────────┐               │          │
│       │    RenderContext        │◄──────────────┘          │
│       │  (Command Buffer Queue) │                          │
│       └────────────┬────────────┘                          │
│                    │                                        │
│       ┌────────────▼────────────┐                          │
│       │      Render Thread      │                          │
│       └────────────┬────────────┘                          │
│                    │                                        │
│       ┌────────────▼────────────┐                          │
│       │  RHI: OpenGL / Vulkan   │                          │
│       └─────────────────────────┘                          │
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
