# Gryce Engine

一个 C++23 游戏引擎：Vulkan / OpenGL 双渲染后端、ECS 架构、JSON 场景序列化。
核心按模块拆分为多个 DLL（`GryceCore` / `GryceRenderer` / `GrycePlatform` / `GrycePhysics`），
通过纯 C API（`extern "C"`，`GCore_*`）对外服务；上层 `GryceEngineUtils` 提供 C++ 薄封装，
编辑器与游戏宿主均基于该层构建。

## 文档

| 文档 | 内容 |
|---|---|
| [架构说明](./docs/架构说明.md) | 模块边界、链接方向、对外接口、构建产物、资源打包 |
| [代码规范](./docs/代码规范.md) | 注释风格与工程约定 |

## 特性

- **双后端渲染**：Vulkan（默认）/ OpenGL（兼容）两套后端，PBR 材质、IBL、阴影、HDR、Bloom、后处理。
- **ECS + 场景系统**：Entity-Component-System，类 Godot/Unity 的节点层级；`.gesc` JSON 场景、Prefab、热重载。
- **物理**：Jolt（3D）/ Box2D（2D）双后端，统一抽象。
- **骨骼动画**：Skeleton / AnimationClip / Pose，GPU Skinning。
- **脚本**：QuickJS 唯一脚本运行时（GryceSRT），ES Module 驱动实体生命周期、props 双向同步、热重载。
- **音频**：miniaudio（AudioSource / AudioListener，3D 空间音效、变速不变调）。
- **运行时 UI**：ColorRect / Label / Sprite2D / TileMap / ParticleEmitter2D / Camera2D、2D 光照。
- **编辑器**：纯 C++ ImGui Docking 编辑器（Hierarchy / Inspector / FileExplorer / Console / Play Mode / Undo-Redo）。

## 快速开始

### 环境要求

| 项 | 说明 |
|---|---|
| 平台 | Windows 10/11（主要支持）；核心与 Linux 兼容 |
| 编译器 | MinGW-w64 GCC（推荐 MSYS2 UCRT64）或 MSVC |
| 构建工具 | CMake ≥ 3.28，Ninja（推荐） |
| Vulkan SDK | 构建 Vulkan 后端所需；无 SDK 时仅 OpenGL |

### 安装依赖（MSYS2 UCRT64，推荐）

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc \
          mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-glew \
          mingw-w64-ucrt-x86_64-glfw
```

首次构建时 `build.py` / `tools/deps_manager.py` 自动下载外部依赖源码（GLFW、GLEW、Assimp、Box2D、
Jolt、GoogleTest）到 `build/deps/`（不入 Git）。仓库内置 imgui、imguizmo、nlohmann/json、stb、
miniaudio、tinyexr、quickjs。

### 构建（推荐用 build.py）

```powershell
python build.py                 # Debug，自动下载缺失依赖
python build.py Release         # Release
python build.py --setup-deps    # 仅下载依赖
python build.py --clean         # 清理产物（保留 deps/）
python build.py --jobs 8        # 并行数
```

也可直接用 CMake：

```bash
cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Debug
```

### 构建产物

构建结果统一为自包含的 `dist` 目录，每目标含可执行文件与其依赖 DLL，可直接拷贝分发：

```text
build/<Config>/bin/<Config>/dist/
├── GryceCore.dll / GryceRenderer.dll / GrycePlatform.dll / GrycePhysics.dll   # 核心模块
├── GryceEngineUtils/{lib, include}      # 上层包装层（库 + 头）
├── GryceEditor/GryceEditor.exe          # ImGui 编辑器
├── GryceGame/GryceGame.exe              # 游戏入口（GryceGC 打包宿主）
├── GryceGC/GryceGC.exe                  # 资源打包工具
└── GryceTests/GryceTests.exe            # 单元测试
```

### 运行测试

```powershell
ctest --test-dir build/Debug
# 或
./build/Debug/bin/Debug/dist/GryceTests/GryceTests.exe
```

### 打包游戏（GryceGC）

```powershell
build/bin/Release/GryceGC.exe --project <项目目录> --name MyGame --build-dir build --config Release --out build/game
```

产物布局：`<out>/<name>/MyGame.exe` + `runtime/`（引擎 DLL）+ `assets/*.gpkg`（资源包）+ `project.data`（唯一配置）。

## 项目结构

```text
Gryce-Engine/
├── src/                    # 引擎核心源码（模块化 DLL + 子系统）
│   ├── GryceCore/          # GryceCore 公共 C API 头
│   ├── GryceRenderer/      # GryceRenderer 公共 C API 头
│   ├── GrycePlatform/      # GrycePlatform 公共 C API 头
│   ├── GrycePhysics/       # GrycePhysics 公共 C API 头
│   ├── api/                # C API 实现
│   ├── animation/ assets/ audio/ components/ ecs/ math/
│   ├── physics/ platform/ reflection/ render(+opengl/vulkan/)/ resources/
│   ├── scene/ script/ ui/ runtime/ utils/
│   └── CMakeLists.txt      # 模块目标定义（模块边界见 docs/架构说明）
├── editor/                 # C++ ImGui 编辑器源码（src/ + CMakeLists.txt）
├── include/GryceEngineUtils/   # 上层 C++ 薄封装头
├── templates/              # 游戏入口模板（GryceGame.exe）
├── tools/                  # GryceGC、deps_manager 等工具
├── tests/                  # 单元测试（GTest）
├── third_party/            # 内置第三方库
├── build/                  # 构建产物 + 外部依赖源码（gitignore）
├── docs/                   # 文档（架构说明 / 代码规范）
├── CMakeLists.txt  build.py  CMakePresets.json  CMakeSettings.json
└── LICENSE
```

## 开发约定

- **模块边界**：依赖方向单向指向 `GryceCore`；新增能力优先以 C API 暴露。详见 [架构说明](./docs/架构说明.md)。
- **注释**：统一中文，解释「为什么」而非「做了什么」，见 [代码规范](./docs/代码规范.md)。
- **提交**：遵循 Conventional Commits。

## 许可证

MIT License（见 [LICENSE](./LICENSE)）。