# Gryce Engine

[![CI](https://github.com/Creatinx/Gryce-Engine/actions/workflows/ci.yml/badge.svg)](https://github.com/Creatinx/Gryce-Engine/actions/workflows/ci.yml)

面向 Windows 的 **C++26 游戏引擎**，核心为带反射系统的 **ECS 架构** 与 **三后端（OpenGL / Vulkan / D3D12）渲染器**。当前主线在渲染器：已打通场景序列化、资源系统、2D/3D 物理、音频与动画，并附带可直接运行的演示程序 `GryceRenderSmoke`。

> **平台与工具链**：Windows 为主目标。构建脚本不含任何机器专属绝对路径——依赖前缀由 `cmake/GryceDependencyPaths.cmake` 从环境变量与编译器路径自动推断。已验证可直接用于 **MSYS2 UCRT64**、**CLion / JetBrains 自带 MinGW**、**vcpkg** 及任意自定义前缀；缺依赖时 `FetchContent` 自动拉源码兜底，拉不到还会自动换镜像，因此克隆下来即可构建。

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

**渲染**

- 三后端：OpenGL / Vulkan / D3D12，共用上层管线与着色器源（Vulkan、D3D12 的函数与着色器链均运行时加载，不绑定 MSVC 导入库；D3D12 现状见 `docs/d3d12-backend-plan.md`）
- 内置调试 UI：左上角 FPS / 帧时间 / 限速状态叠加（带帧时间曲线），以及 ImGui 调试窗口（F1 开关）——可热开关垂直同步、帧率上限（软件锁帧）、省电模式、GPU 计时，以及阴影 / GPU 剔除 / SSR / IBL 等渲染开关
- Clustered Forward 前向管线、HDR、色调映射、自动曝光
- PBR 材质与预设（如 `Chrome` 等金属 / 镜面预设）
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
│   ├── render/                      渲染抽象层与两个后端
│   │   ├── opengl/                  OpenGL 后端
│   │   ├── vulkan/                  Vulkan 后端（运行时加载 vulkan-1.dll）
│   │   ├── renderer_rd/             管线实现：forward_clustered / shadow / effects / environment
│   │   ├── storage_rd/              GPU 资源存储
│   │   └── shaders/                GLSL 着色器源
│   ├── assets/                      OBJ / DDS / KTX / EXR / STB 加载与异步加载
│   ├── resources/                   工程、资源路径、Pak / Gpack、加密
│   ├── animation/                   骨骼、剪辑、姿态
│   ├── platform/                    窗口、输入、光标
│   └── utils/                       日志、帧率限制等
├── test/
│   ├── math_tests.cpp               数学层单元测试（CTest 唯一门禁）
│   ├── renderer_smoke.cpp           演示宿主源码（GryceRenderSmoke.exe，不参与 CTest）
│   └── demo_project/                演示工程（project.data + 场景 / 模型 / 着色器）
├── third_party/                     仓库内置第三方依赖（Amplitude、imgui、stb ...）
├── toolchain/
│   └── grycegc/                     GryceGC 命令行工具链（C++，替代原 Python 脚本）
└── .github/workflows/ci.yml         CI：构建 + CTest（全部通过才算通过）
```

### GryceGC 工具链

`toolchain/grycegc` 是引擎的命令行工具链（C++23，自包含零引擎 DLL 依赖，
与 GryceMathTests 同构——把需要的 engine 源码直接编进 `grycegc_core` 静态库）。
随主工程一起构建（`GRYCE_BUILD_TOOLCHAIN=ON` 默认开，也可独立配置）。

```bash
# 独立构建（不装 Python）
cmake -S toolchain/grycegc -B build-grycegc -G Ninja
```

常用命令（`GryceGC help <command>` 查看完整参数）：

| 命令 | 作用 |
| --- | --- |
| `package` | 打包成可脱线运行的发布目录（exe + core DLL 同级 + `dll/` + `demo_project` + 启动器 + README） |
| `run` | 跨平台启动器：注入 `dll/` 到 PATH 后启动宿主（替代 `run*.bat`） |
| `verify` | 校验发布包自洽性（所有导入 DLL 在包内可解析） |
| `deps` | 列出 exe 的完整 DLL 依赖闭包（core / 依赖分类） |
| `copyright` | 版权印章：COPYRIGHT.txt + README 页脚 + PE trailer（可选 VERSIONINFO） |
| `meta` | 元数据读写：`gryce.meta.json` / `project.data` / PE trailer / git 戳 |
| `encrypt` / `decrypt` / `keygen` | AES-256-GCM / ChaCha20 加解密（与引擎 `aes_gcm_buf` 字节兼容） |
| `pak` | 资源包 create/list/extract/verify（`.gpack` v1 / `.pak` v3+v4 加密） |
| `debug` | 以预设调试环境变量启动宿主（D3D12 debug layer / trace / probe / glog） |
| `bench` | 帧耗时 A/B 基准（单行 KV 输出） |
| `font` | 重新生成 `engine/render/imgui_font_data.h` |
| `build` | cmake configure + build 封装 |

```bash
# 典型发布流程
GryceGC package --build-dir build --verify
GryceGC copyright --package dist/GryceEngine --holder "Your Name" --license MIT
GryceGC run --package dist/GryceEngine -- --frames 120 --shot out.png
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

### 依赖的两种来源

**CMake 本身不会自动下载依赖**。`find_package` / `find_library` 只在本机搜索已安装的东西；要"自动下载"必须显式用 `FetchContent`。本工程用 CMake 内置的 `FetchContent` 做兜底，由 `GRYCE_DEP_FETCH` 控制：

| 取值 | 行为 |
| --- | --- |
| `AUTO`（默认） | 系统包优先；某项系统里找不到，就只把那一项拉源码编译 |
| `ON` | 忽略系统包，全部拉源码编译（版本最可控，首次配置要下载） |
| `OFF` | 只用系统包，完全离线（CI / 受控环境） |

```bash
cmake -S . -B build -G Ninja -DGRYCE_DEP_FETCH=AUTO
```

源码按固定版本拉取：GLFW `3.4`、GLEW `glew-2.2.0`（官方 release 包，因为 git 仓库里没有构建期生成的头文件）、Box2D `v3.1.1`、Jolt Physics `v5.3.0`（CMake 入口在其 `Build/` 子目录）、Assimp `v5.4.3`（静态库，importer 按需裁剪为 FBX/OBJ/glTF/Collada/PLY/STL/3DS/Blend）。

**统一走 tarball，不走 `git clone`**：git 的智能 HTTP 协议一次 clone 要发多轮请求，穿过代理 / 加速器时极易被掐断（Jolt 全量 clone 约 60 MB，实测反复 `Connection was reset`；tarball 只有 18 MB，一次性 HTTPS GET）。tarball 还能用 SHA256 锁定内容，也不需要本机装 git。下载物缓存在 `build/_deps/tarballs`，重配置不会重复下载；校验和不匹配会自动重新下载。

**源码地址自动探测**：默认先试直连 GitHub，不通就自动换镜像（`ghfast.top` / `gh-proxy.com` / `gh.llkk.cc`），探测结果写入 CMake 缓存，后续配置不再重试。手动指定：`-DGRYCE_DEP_GIT_BASE=https://ghfast.top/https://github.com/`。若某个包反复下载失败（自动重试 3 次后报错），可换镜像、用本地源码（`-DFETCHCONTENT_SOURCE_DIR_JOLT=D:/deps/JoltPhysics`）或装系统包。

**依赖前缀自动推断**（无需手写路径）：

1. `-DGRYCE_DEP_PREFIX=<prefix>` 或环境变量 `GRYCE_DEP_PREFIX`（多路径用 `;` 分隔）
2. 环境变量 `CMAKE_PREFIX_PATH`
3. `MSYS2_ROOT` + `MSYSTEM`（如 `D:/msys64` + `UCRT64`）
4. MSYS2 自带的 `MSYSTEM_PREFIX` / `MINGW_PREFIX`（POSIX 风格 `/ucrt64`，用 `cygpath` 转原生路径）
5. C/C++ 编译器路径反推：`<root>/<msystem>/bin/g++.exe` → `<root>/<msystem>`（CLion MinGW 即由此命中）

| 场景 | 做法 |
| --- | --- |
| MSYS2 装在非默认盘符 / 非 UCRT64 | 无需操作（在对应 MSYS2 终端里配置即可）；也可 `-DGRYCE_DEP_PREFIX=D:/msys64/ucrt64` |
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

构建产物（DLL 形式的引擎 + 演示宿主）：

| 目标 | 说明 |
| --- | --- |
| `GryceCore.dll` | 抽象层 + 场景 / ECS / 组件 / 反射 / 资源 / 渲染抽象 + 物理与音频 |
| `GryceRenderer.dll` | 渲染管线与 OpenGL / Vulkan 后端 |
| `GrycePlatform.dll` | 窗口、输入、光标 |
| `GryceRenderSmoke.exe` | 演示宿主（人工运行，非 CTest 用例） |
| `GryceMathTests.exe` | 数学层单元测试宿主，仅 `BUILD_TESTING=ON` 时构建 |

构建完成后，始终执行的 `GryceSyncRuntimeDlls` 目标会把引擎 DLL、第三方运行库（Box2D / Jolt / GLFW / GLEW）以及 **MinGW 运行时**（`libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll`）复制到 `GryceRenderSmoke.exe` 同目录——**脱离 MinGW / MSYS2 终端也能直接运行**。

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

**CI 的通过条件只有一条：`ctest` 里的用例全部通过。** 任一用例失败，`ctest` 返回非零，job 即判定失败（CI 里没有 `continue-on-error`，也不会跳过用例）。

```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

当前注册的用例全部来自 `test/math_tests.cpp`，是**纯计算结果的数值核对**，由 `GryceMathTests.exe` 承载：

| 用例 | 核对内容 |
| --- | --- |
| `math_vector` | `Vector2/3/4` 的加减乘除、点积叉积、长度与归一化，以及右手坐标系约定（X×Y=Z） |
| `math_matrix` | `Matrix4` 的 translate / scale / rotate / multiply / transpose / inverse，以及 perspective、ortho、look_at、billboard 的投影结果 |
| `math_quaternion` | 轴角构造、共轭与逆、乘法、slerp、`rotate_vector`，以及 `to_matrix` / `from_rotation_matrix` / 欧拉角往返的一致性 |
| `math_camera` | 默认朝向与右手正交基、`look_at` 反解 yaw/pitch、WASD 前后左右移动、pitch 限幅、视图与投影矩阵 |
| `math_ray` | `Ray::at` 求值、slab 法 AABB 求交（命中 / 未命中 / 内部起点）与命中距离 |

设计约束（也是这类测试能成为门禁的前提）：

- **不链接任何引擎 DLL**：被测的 `engine/math/*.cpp` 直接编译进测试可执行文件，因此与 GPU、驱动、窗口 / 无头环境完全解耦，CI 上毫秒级出结果，也不会因缺 `glfw3.dll` / MinGW 运行时卡到超时。
- **确定性**：不读外部资源、不依赖浮点之外的随机量，同一份代码在任何机器上结果一致。

老的像素比对用例（`renderer_smoke_opengl` / `renderer_smoke_vulkan`）及其 Python 工具（`test/tools/backend_parity.py`、`test/tools/ssr_accuracy.py`）已删除：它们靠"截图里非背景像素占比"判定，受无头环境 / 软件渲染 / 驱动版本影响很大，既不反映构建正确性也不反映计算正确性。`GryceRenderSmoke.exe` 本身保留为人工运行的演示宿主。

新增 suite 时：在 `GRYCE_MATH_TEST_SUITES`（根 `CMakeLists.txt`）里加名字即可自动注册成一条 CTest。

---

## 演示工程

`test/demo_project` 是最小可运行工程：

- `project.data`：工程配置，含入口场景、窗口尺寸、HDR / 色调映射 / 曝光、阴影与环境、SSR / SSAO / SSIL / PCSS 等开关
- `scenes/main.gesc`：主场景，含地面、若干 PBR 材质立方体（含镜面 `Chrome`）、火焰粒子、相机与光源，以及一个**带镜面材质的自由下坠立方体**（由 Jolt 刚体驱动，落到静态地面碰撞体上）
- `models/`：Wavefront OBJ 基础网格 —— `cube.obj`（1×1×1 原点居中）、`quad.obj`（y=0 平面 2×2 地面，场景按 6× 缩放为 12×12，与 `GroundCollider` 半长 6 对齐）、`sphere.obj`（r=0.5 UV 球，备用）。三者均为右手系、逆时针外翻，与 `glFrontFace(GL_CCW)` / `VK_FRONT_FACE_COUNTER_CLOCKWISE` 一致

  > ⚠️ 根 `.gitignore` 忽略 MSVC 中间产物的 `*.obj` 规则会连带吞掉模型文件，靠文件末尾的 `!test/demo_project/models/*.obj` 例外放行。新增模型目录时记得同步例外规则。

- `shaders/`：预编译 SPIR-V（`shaders/spirv/`、`shaders/forward_clustered/spirv/`）作为兜底产物。GLSL 源统一在 `engine/render/shaders/`（见 `engine/render/core_shaders.cpp` 的运行时查找逻辑）；`*.spv.hash` 仅为本地缓存指纹，不入库

---

## 许可

项目主体采用 **MIT License**，详见 [LICENSE](LICENSE)。`third_party/` 下的第三方库遵循各自原有许可证（如 `third_party/imgui/LICENSE.txt`、`third_party/quickjs/LICENSE` 等）。
