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

- Windows 10/11（当前主要支持平台）
- [MSYS2 UCRT64](https://www.msys2.org/) 或 MinGW-w64
- CMake ≥ 3.28
- Ninja（推荐）或 Make
- OpenGL 4.6 / Vulkan 1.2 兼容显卡
- Vulkan SDK（可选，仅当使用 `--vulkan` 时）

### 安装依赖（MSYS2 UCRT64）

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-glew \
          mingw-w64-ucrt-x86_64-glfw
```

### 构建

```bash
# Debug（默认 OpenGL 后端）
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug

# Release
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
```

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
├── core/                   # 引擎核心库
│   ├── assets/             # 资源加载器（OBJ、纹理、字体）
│   ├── audio/              # 音频系统（占位）
│   ├── components/         # ECS 组件（3D + 2D）
│   ├── ecs/                # ECS 系统（World、System、调度）
│   ├── math/               # 数学库（Vector、Matrix、Quaternion）
│   ├── physics/            # 物理工具与常量
│   ├── platform/           # 窗口、输入、光标
│   ├── render/             # RHI、渲染管线、OpenGL/Vulkan 后端
│   ├── resources/          # 资源路径、项目根解析
│   ├── scene/              # Scene、Entity、Transform 层级
│   ├── server/             # 渲染服务器/线程
│   └── utils/              # 日志、帧率限制、工具类
├── docs/                   # 文档（Todo、架构解析等）
├── editor/                 # 编辑器入口（占位）
├── res/                    # 运行时资源
│   ├── fonts/
│   ├── models/
│   ├── scenes/
│   ├── shaders/
│   ├── textures/
│   └── tilesets/
├── tests/                  # 测试与演示程序
│   ├── 3dtest.cpp          # 3D 演示
│   ├── gt2dDemo.cpp        # 2D 演示
│   ├── *_test.cpp          # 单元测试
│   └── ui/                 # 演示共享 UI（DebugPanel）
├── third_party/            # 第三方库（imgui、json、stb、miniaudio）
├── CMakeLists.txt          # 根 CMake
├── README.md               # 本文件
└── ROADMAP.md              # 开发路线图
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

更多细节参见 [`docs/PROJECT_ANALYSIS.md`](./docs/PROJECT_ANALYSIS.md)。

---

## 开发计划

详见 [`docs/TODO.md`](./docs/TODO.md) 与 [`ROADMAP.md`](./ROADMAP.md)。

主要里程碑：

1. P0：渲染器收尾（RHI 稳定、双后端干净退出、性能优化）。
2. P1：组件系统完善（生命周期、Prefab、Inspector）。
3. P2：物理系统接入专业库（Jolt/Box2D）。
4. P3：资源管线（Assimp、材质文件、热重载）。
5. P4：编辑器基础（Hierarchy、Inspector、Viewport Gizmo）。
6. P5：脚本系统（Lua/C#）。
7. P6：音频系统。
8. P7~P9：高级渲染、光追、发布。

---

## 贡献

目前项目处于早期原型阶段，API 不稳定。欢迎提交 Issue 与 PR。

---

## 许可证

MIT License（详见 [LICENSE](./LICENSE)）。
