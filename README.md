# Gryce Engine

[![CI](https://github.com/Creatinx/Gryce-Engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Creatinx/Gryce-Engine/actions/workflows/ci.yml)

Gryce Engine 是一个面向 Windows 的 C++26 游戏引擎，核心为**带反射系统的 ECS 架构**与**双后端（OpenGL / Vulkan）渲染器**。仓库当前以渲染器为主线，已打通场景序列化、资源系统、2D/3D 物理、音频与动画，并提供可直接运行的冒烟/演示程序 `GryceRenderSmoke`。

> 目标平台：Windows + MSYS2 UCRT64（也支持 CLion 自带 MinGW / vcpkg / 任意自定义前缀）。构建脚本不含任何机器专属的绝对路径：依赖前缀由 `cmake/GryceDependencyPaths.cmake` 从环境变量与编译器路径自动推断，装好依赖即可直接配置。

---

## 特性概览

**架构**

- 反射驱动的 ECS：组件通过反射系统注册，可被场景序列化、编辑器与脚本统一访问
- 场景以 JSON 文本格式（`.gesc`）存储，实体由 `name` / `uuid` / `transform` / `components` 组成
- 运行期热重载：场景、着色器、材质与网格支持重载，无需重启进程
- Play 模式：进入后世界驱动系统更新（物理、动画），退出后恢复编辑态快照
- 稳定的 C API 边界（`engine/api`），引擎以 DLL 形式对外提供能力

**渲染**

- 双后端：OpenGL 与 Vulkan，共用上层管线与着色器源
- Clustered Forward 前向管线、HDR、色调映射、自动曝光
- PBR 材质与预设（如 `Chrome` 等金属/镜面预设）
- 阴影：Shadow Atlas、VSM / ESM、PCSS 软阴影、点光源阴影、接触阴影
- 屏幕空间效果：SSR（反射）、SSAO、SSIL
- 全局光照与环境：SDFGI、VoxelGI、Reflection Probe、IBL、体积雾
- 后处理：Bloom、TAA、运动模糊、Bokeh DOF、FSR2、次表面散射、水面
- 2D 管线：精灵、图块地图、视差背景、2D 粒子、2D 天空盒、文本
- 3D 扩展：3D 粒子、线渲染、拖尾、实例化网格、Billboard、3D 文本、LOD、体积光

**子系统**

- 物理：Box2D（2D 刚体、风力区、浮力区）+ Jolt Physics（3D 刚体、碰撞体）
- 音频：Amplitude Audio SDK（3D 空间音频、总线、事件）
- 动画：骨骼、动画剪辑、姿态混合
- 资源：`res:/` 虚拟路径、Pak / Gpack 打包、AES-GCM 加密、异步加载
- 资产格式：OBJ、DDS、KTX、EXR、PNG/JPG 等

---

## 目录结构

```
Gryce-Engine/
├── CMakeLists.txt              根构建脚本：依赖发现、测试注册、DLL 同步
├── cmake/                      构建辅助模块，无硬编码路径
│   ├── GryceDependencyPaths.cmake   依赖前缀自动推断 + 缺失时的排查提示
│   └── GryceFetchDeps.cmake         缺失依赖的 FetchContent 源码构建兜底
├── engine/                     引擎本体（命名空间 gryce_engine）
│   ├── api/                    C API 边界（core / entity / scene / component / render ...）
│   ├── math/                   数学库与相机
│   ├── scene/                  实体、场景、预制体、序列化、UUID
│   ├── ecs/                    World、ComponentStore 与各 System
│   │   └── systems/            animator / hierarchy / physics_2d / physics_3d /
│   │                           audio / render_2d / render_3d / subviewport
│   ├── components/             组件定义（2d / 3d / common）
│   ├── reflection/             反射注册与内建类型反射
│   ├── render/                 渲染抽象层与两个后端
│   │   ├── opengl/             OpenGL 后端
│   │   ├── vulkan/             Vulkan 后端（运行时加载 vulkan-1.dll）
│   │   ├── renderer_rd/        管线实现：forward_clustered / shadow / effects / environment
│   │   ├── storage_rd/         GPU 资源存储
│   │   └── shaders/            着色器源
│   ├── assets/                 OBJ / DDS / KTX / EXR / STB 加载与异步加载
│   ├── resources/              项目、资源路径、Pak / Gpack、加密
│   ├── animation/              骨骼、剪辑、姿态
│   ├── platform/               窗口、输入、光标
│   └── utils/                  glog 日志、帧率限制等
├── test/
│   ├── renderer_smoke.cpp      冒烟 / 演示宿主
│   ├── demo_project/           演示工程（project.data + 场景 / 模型 / 着色器）
│   └── tools/                  Python 校验工具（后端一致性、SSR 精度）
├── third_party/                第三方依赖（见下）
├── toolchain/                  Python 辅助脚本
└── .github/workflows/ci.yml    CI：构建 + CTest 冒烟测试
```

---

## 依赖

| 依赖 | 来源 | 说明 |
| --- | --- | --- |
| GCC / gcc-libs | MSYS2 UCRT64 | 需支持 C++26 |
| CMake ≥ 3.24、Ninja | MSYS2 UCRT64 | 构建系统 |
| GLFW | MSYS2 UCRT64 | 窗口与输入 |
| GLEW | MSYS2 UCRT64 | OpenGL 扩展加载 |
| Box2D | MSYS2 UCRT64 | 2D 物理，导入目标 `box2d::box2d` |
| Jolt Physics | MSYS2 UCRT64 | 3D 物理，导入目标 `Jolt::Jolt` |
| Vulkan SDK | 可选 | 缺失时自动关闭 Vulkan 后端与对应测试 |
| Amplitude Audio SDK | 仓库内置 `third_party/amplitude` | 预编译静态库，随仓库分发 |
| stb / tinyexr / nlohmann_json / imgui / quickjs | 仓库内置 `third_party` | 图像、EXR、JSON、编辑器 UI、脚本 |

这些依赖**不是必须手动安装**的：CMake 优先在系统里找，找不到会自动用 `FetchContent` 拉源码随工程一起编译（见下方"依赖的两种来源"）。想完全离线或统一版本，可在 MSYS2 UCRT64 终端里装系统包：

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-gcc-libs \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-glfw \
  mingw-w64-ucrt-x86_64-glew \
  mingw-w64-ucrt-x86_64-box2d \
  mingw-w64-ucrt-x86_64-jolt-physics
```

### 依赖的两种来源

注意：**CMake 本身不会自动下载依赖**。`find_package` / `find_library` 只在本机磁盘上搜索已安装的东西；要"自动下载"必须显式使用 `FetchContent`（CMake 内置）或 CPM / vcpkg / conan。本工程用 CMake 内置的 `FetchContent` 做兜底，策略由 `GRYCE_DEP_FETCH` 控制：

| 取值 | 行为 |
| --- | --- |
| `AUTO`（默认） | 系统包优先；某一项系统里找不到，就只把那一项拉源码编译 |
| `ON` | 忽略系统包，全部拉源码编译（版本最可控，首次配置要下载） |
| `OFF` | 只用系统包，完全离线（CI / 受控环境） |

```bash
cmake -S . -B build -G Ninja -DGRYCE_DEP_FETCH=AUTO
```

源码按固定版本拉取：GLFW `3.4`、GLEW `glew-2.2.0`（官方 release 包，因为 git 仓库里没有构建期生成的头文件）、Box2D `v3.1.1`、Jolt Physics `v5.3.0`（CMake 入口在其 `Build/` 子目录）。

**统一走 tarball，不走 `git clone`**：git 的智能 HTTP 协议一次 clone 要发多轮请求，穿过代理/加速器时极易被掐断（Jolt 全量 clone 约 60 MB，实测反复 `Connection was reset`；改走 tarball 后只有 18 MB，且是一次性 HTTPS GET）。tarball 还能用 SHA256 锁定内容，也不需要本机装 git。

下载物缓存在 `build/_deps/tarballs`，重配置不会重复下载；校验和不匹配会自动重新下载。

**源码地址自动探测**：默认先试直连 GitHub，不通就自动换镜像（`ghfast.top` / `gh-proxy.com` / `gh.llkk.cc`），探测结果写进 CMake 缓存，后续配置不再重试。配置时能看到类似输出：

```
-- Probing dependency source base: https://github.com/
-- Probing dependency source base: https://ghfast.top/https://github.com/
-- Dependency source base: https://ghfast.top/https://github.com/
```

想自己指定就传 `-DGRYCE_DEP_GIT_BASE=<前缀>`（例如 `https://ghfast.top/https://github.com/`）。若某个包反复下载失败，自动重试 3 次后报错，此时可：

```bash
# 1. 换镜像
cmake -S . -B build -DGRYCE_DEP_GIT_BASE="https://ghfast.top/https://github.com/"

# 2. 用本地已有源码，完全离线（CMake 原生变量，跳过下载）
cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_JOLT=D:/deps/JoltPhysics

# 3. 装系统包（pacman 见上），构建时会自动优先使用
```

已知代价：Jolt Physics 编译最慢；若不想等，装系统包即可让 CMake 跳过它。

依赖前缀（`<prefix>` = 含 `include/` `lib/` `bin/` 的目录）按以下顺序推断，**无需手写路径**：

1. `-DGRYCE_DEP_PREFIX=<prefix>` 或环境变量 `GRYCE_DEP_PREFIX`（多路径用 `;` 分隔）
2. 环境变量 `CMAKE_PREFIX_PATH`
3. `MSYS2_ROOT` + `MSYSTEM`（如 `D:/msys64` + `UCRT64`）
4. MSYS2 自带的 `MSYSTEM_PREFIX` / `MINGW_PREFIX`（POSIX 风格 `/ucrt64`，用 `cygpath` 转原生路径）
5. C/C++ 编译器路径反推：`<root>/<msystem>/bin/g++.exe` → `<root>/<msystem>`

常见场景：

| 场景 | 做法 |
| --- | --- |
| MSYS2 装在非默认盘符 / 非 UCRT64 | 无需操作（在对应的 MSYS2 终端里配置即可）；也可 `-DGRYCE_DEP_PREFIX=D:/msys64/ucrt64` |
| CLion / JetBrains 自带 MinGW | 无需操作，前缀由编译器路径反推到 `<CLion>/bin/mingw` |
| vcpkg | `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` |
| 自定义前缀 | `-DGRYCE_DEP_PREFIX=<prefix>` 或 `export GRYCE_DEP_PREFIX=<prefix>` |

Vulkan SDK 为可选项：装好 LunarG SDK 后 `VULKAN_SDK` 环境变量会指向它，未安装时自动关闭 Vulkan 后端与对应测试。

---

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel
```

构建产物：

| 目标 | 说明 |
| --- | --- |
| `GryceCore.dll` | 抽象层 + 场景 / ECS / 组件 / 反射 / 资源 / 渲染抽象 + 物理与音频 |
| `GryceRenderer.dll` | 渲染管线与 OpenGL / Vulkan 后端 |
| `GrycePlatform.dll` | 窗口、输入、光标 |
| `GryceRenderSmoke.exe` | 冒烟 / 演示宿主 |

构建完成后，`GryceSyncRuntimeDlls` 目标会把引擎 DLL 及第三方运行库（Box2D / Jolt / GLFW / GLEW）复制到 `GryceRenderSmoke.exe` 同目录，脱离 MSYS2 终端也能直接运行。

可选构建开关：

- `-DGRYCE_ENABLE_IMGUI=OFF` 关闭 ImGui 编辑器叠加层
- `-DBUILD_TESTING=OFF` 关闭 CTest 用例

---

## 运行演示

`GryceRenderSmoke` 默认加载 `test/demo_project` 工程：

```bash
build/GryceRenderSmoke.exe --api opengl
```

常用参数：

| 参数 | 说明 |
| --- | --- |
| `--api opengl\|vulkan` | 选择渲染后端 |
| `--project <dir>` / `--scene <res:/...>` | 指定工程根与入口场景 |
| `--frames N` | 驱动 N 帧后退出（交互模式下表示第 N 帧截图） |
| `--shot <png>` / `--w N` / `--h N` | 截图输出路径与视口尺寸 |
| `--play` | 交互模式：鼠标锁定转视角，WASD 移动，空格 / 左 Ctrl 升降，Shift 加速，Esc 退出 |
| `--simulate` | 进入 Play 模式驱动系统更新（物理 / 动画），用于脚本化演示与截图 |
| `--cam-pos x y z` / `--cam-look x y z` | 覆盖相机位姿 |
| `--ssr` / `--no-ssr`、`--ssao` / `--no-ssao`、`--ssil` / `--no-ssil`、`--pcss` / `--no-pcss` | 逐项开关屏幕空间与阴影效果 |
| `--no-vsync` / `--vsync`、`--fps N` / `--no-fps` | 垂直同步与帧率限制 |
| `--list` | 打印场景实体、相机、光源与网格统计 |
| `--help` | 查看完整参数列表 |

示例：以 Vulkan 后端进入 Play 模式并在第 120 帧截图。

```bash
build/GryceRenderSmoke.exe --api vulkan --simulate --frames 120 --shot build/frame.png
```

---

## 测试

```bash
ctest --test-dir build --output-on-failure -C Release
```

- `renderer_smoke_opengl` / `renderer_smoke_vulkan`：初始化双后端、走帧管线、截图并校验退出码
- `test/tools/backend_parity.py`：同一场景下对比 OpenGL 与 Vulkan 的逐像素差异，并检查 SSR / SSAO 等效果是否真正生效（需 `numpy`、`Pillow`）
- `test/tools/ssr_accuracy.py`：用解析几何复核 SSR 反射的掩码一致性与命中误差

---

## 演示工程

`test/demo_project` 是最小可运行工程，结构如下：

- `project.data`：工程配置，含入口场景、窗口尺寸、HDR / 色调映射 / 曝光、阴影与环境、SSR / SSAO / SSIL / PCSS 等开关
- `scenes/main.gesc`：主场景，包含地面、若干 PBR 材质立方体（含镜面 `Chrome`）、火焰粒子、相机与光源，以及一个**带镜面材质的自由下坠立方体**（由 Jolt 刚体驱动，落到静态地面碰撞体上）
- `models/`：单位立方体与四边形网格
- `shaders/`：着色器源与预编译 SPIR-V 兜底产物

---

## 许可

本项目主体采用 **MIT License**，详见 [LICENSE](LICENSE)。

`third_party/` 下的第三方库遵循各自原有的许可证（如 `third_party/imgui/LICENSE.txt`、`third_party/quickjs/LICENSE` 等）。
