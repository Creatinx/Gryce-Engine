# 构建

本文覆盖依赖发现、配置选项与构建产物。架构总览见 [architecture.md](architecture.md)，测试与 CI 见 [testing.md](testing.md)，Web 构建见 [webgl-backend.md](webgl-backend.md)。

## 1. 快速开始

```bash
# 1. 准备依赖（任选其一，缺了也会自动拉源码兜底）
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

# 3. 跑测试（全部通过才算绿）
ctest --test-dir build --output-on-failure

# 4. 运行（交互模式：WASD 移动，鼠标转视角）
build/GryceRenderSmoke.exe --api opengl --play
```

## 2. 依赖

| 依赖 | 来源 | 说明 |
| --- | --- | --- |
| GCC / gcc-libs | MSYS2 UCRT64 / CLion MinGW | 需支持 C++26 |
| CMake ≥ 3.24、Ninja | MSYS2 UCRT64 / CLion MinGW | 构建系统 |
| GLFW | 系统包或 FetchContent | 窗口与输入 |
| GLEW | 系统包或 FetchContent | OpenGL 扩展加载 |
| Box2D | 系统包或 FetchContent | 2D 物理，导入目标 `box2d::box2d` |
| Jolt Physics | 系统包或 FetchContent | 3D 物理，导入目标 `Jolt::Jolt` |
| Assimp | 可选，系统包或 FetchContent | 模型导入（FBX / glTF / DAE / PLY / STL / 3DS / Blend；OBJ 走内置加载器），缺失时非 OBJ 格式不可用 |
| Vulkan SDK | 可选 | 缺失时自动关闭 Vulkan 后端（CTest 不依赖它） |
| Amplitude Audio SDK | 仓库内置 `third_party/amplitude` | 预编译静态库，随仓库分发 |
| stb / tinyexr / nlohmann_json / imgui / quickjs | 仓库内置 `third_party` | 图像、EXR、JSON、编辑器 UI、脚本 |

### 依赖的三种来源策略

CMake 本身不会自动下载依赖，必须显式使用 `FetchContent`。本工程由 `GRYCE_DEP_FETCH` 控制：

| 取值 | 行为 |
| --- | --- |
| `AUTO`（默认） | 系统包优先；某项系统里找不到，就只把那一项拉源码编译 |
| `ON` | 忽略系统包，全部拉源码编译（版本最可控，首次配置要下载） |
| `OFF` | 只用系统包，完全离线（CI / 受控环境） |

源码按固定版本拉取：GLFW `3.4`、GLEW `glew-2.2.0`、Box2D `v3.1.1`、Jolt Physics `v5.3.0`、Assimp `v5.4.3`。统一走 tarball 而非 `git clone`（git 智能 HTTP 协议穿过代理时易被掐断；tarball 可 SHA256 锁定且不需要本机装 git）。下载物缓存在 `build/_deps/tarballs`，重配置不重复下载。

源码地址默认先试直连 GitHub，不通自动换镜像（`ghfast.top` / `gh-proxy.com` / `gh.llkk.cc`），探测结果写入 CMake 缓存。手动指定：`-DGRYCE_DEP_GIT_BASE=https://ghfast.top/https://github.com/`。反复失败时可换镜像、用本地源码（`-DFETCHCONTENT_SOURCE_DIR_JOLT=D:/deps/JoltPhysics`）或装系统包。

### 依赖前缀自动推断

无需手写路径，按以下顺序推断：

1. `-DGRYCE_DEP_PREFIX=<prefix>` 或环境变量 `GRYCE_DEP_PREFIX`（多路径用 `;` 分隔）
2. 环境变量 `CMAKE_PREFIX_PATH`
3. `MSYS2_ROOT` + `MSYSTEM`（如 `D:/msys64` + `UCRT64`）
4. MSYS2 自带的 `MSYSTEM_PREFIX` / `MINGW_PREFIX`
5. C/C++ 编译器路径反推：`<root>/<msystem>/bin/g++.exe` → `<root>/<msystem>`（CLion MinGW 即由此命中）

| 场景 | 做法 |
| --- | --- |
| MSYS2 装在非默认盘符 / 非 UCRT64 | 在对应 MSYS2 终端里配置即可；也可 `-DGRYCE_DEP_PREFIX=D:/msys64/ucrt64` |
| CLion / JetBrains 自带 MinGW | 无需操作，前缀由编译器路径反推 |
| vcpkg | `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake` |
| 自定义前缀 | `-DGRYCE_DEP_PREFIX=<prefix>` 或 `export GRYCE_DEP_PREFIX=<prefix>` |

Vulkan SDK 为可选项：装好 LunarG SDK 后 `VULKAN_SDK` 环境变量会指向它，未安装时自动关闭 Vulkan 后端与对应测试。

## 3. 构建选项

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `GRYCE_ENABLE_IMGUI` | `ON` | ImGui 编辑器叠加层 |
| `BUILD_TESTING` | `ON` | CTest 用例 |
| `GRYCE_BUILD_EDITOR` | `OFF` | 编辑器整套（`GryceEditor.exe` + `GryceEditorExtension.dll` + 项目，见 [../editor/README.md](../../editor/README.md)）。开发期工具，产物不进运行时目录、不参与门禁；界面走引擎内置轨 |
| `GRYCE_BUILD_TOOLCHAIN` | `ON` | GryceGC 命令行工具链（见 [toolchain.md](toolchain.md)） |
| `GRYCE_ENABLE_D3D12` | `ON` | D3D12 后端；Windows + GCC/Clang 时探测 mingw 自带或 Windows SDK 的 `d3d12.h`，都没有则静默跳过 |
| `GRYCE_ENABLE_WEBGL` | `OFF` | WebGL（Emscripten）构建，检测到 Emscripten 工具链时自动启用 |
| `GRYCE_ENABLE_LTO` | `OFF` | 对非 Debug 构建启用过程间优化 |
| `GRYCE_ENABLE_X86_V3` | `ON` | x86-64-v3 目标 ISA（AVX2 / FMA / BMI / F16C / LZCNT / POPCNT）；老于 Haswell 的机器用 `OFF` 关闭 |
| `GRYCE_WEBGL_DEBUG` | `ON` | Web 构建保留 abort 断言与异常信息 |

其他常用变量：`GRYCE_DEP_FETCH`、`GRYCE_DEP_PREFIX`、`GRYCE_DEP_GIT_BASE`（见上）。

## 4. 构建产物

| 目标 | 说明 |
| --- | --- |
| `GryceCore.dll` | 抽象层 + 场景 / ECS / 组件 / 反射 / 资源 / 渲染抽象 + 物理与音频 |
| `GryceRenderer.dll` | 渲染管线与 OpenGL / Vulkan / D3D12 后端 |
| `GrycePlatform.dll` | 窗口、输入、光标 |
| `GryceRenderSmoke.exe` | 渲染演示台（人工运行，非 CTest 用例） |
| `GryceApp.exe` | 应用运行时（`--project <dir>` 启动，入口 `res:/main.js`，见 [app-entry.md](app-entry.md)） |
| `GryceMathTests.exe` | 数学与资源规则测试宿主，仅 `BUILD_TESTING=ON` 时构建 |
| `GryceGC.exe` | 工具链可执行文件，仅 `GRYCE_BUILD_TOOLCHAIN=ON` 时构建 |

构建完成后，`GryceSyncRuntimeDlls` 目标把引擎 DLL、第三方运行库（Box2D / Jolt / GLFW / GLEW）以及 MinGW 运行时（`libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll`）复制到 `GryceRenderSmoke.exe` 同目录，脱离 MinGW / MSYS2 终端也能直接运行。引擎 DLL 自身另挂 POST_BUILD 拷贝，单独构建某个引擎库时 exe 目录不会留下旧 DLL。

工具链也可独立构建（不装 Python）：

```bash
cmake -S toolchain -B toolchain/build -G Ninja
```

编辑器同样可独立构建（先构建好引擎，它需要引擎的导入库）：

```bash
cmake -S . -B build -G Ninja && cmake --build build   # 先有引擎树
cmake -S editor -B editor/build -G Ninja                # 再单独构建编辑器
cmake --build editor/build --target GryceEditor
```

`editor/` 自带一整套 CMake 工程，与 `toolchain/` 同构：既能单独配置（上面那条），
也能通过根工程的 `GRYCE_BUILD_EDITOR=ON` 接进根构建树（此时目标名与产物位置
完全一致）。日常迭代编辑器只改 `editor/ext/` 下的源码时，独立构建不用重配整个
引擎。

## 5. CI

CI 在 Windows / MSYS2 UCRT64 上执行 configure + build + CTest，Release 配置，门禁只有一条：CTest 全部通过。配置细节见 [testing.md](testing.md)。
