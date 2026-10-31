# GryceGC 工具链使用文档

`toolchain/` 是 Gryce-Engine 的命令行工具链，代号 **GryceGC**（C++23，自包含，零引擎 DLL 依赖）：把引擎里需要的源码（`aes_gcm` / `gpack` / `pak` / `glog` / `crypto`）直接编进 `grycegc_core` 静态库。它随主工程一起构建（`GRYCE_BUILD_TOOLCHAIN=ON` 默认开），也可以脱离引擎独立配置。

本文是**使用手册**：告诉你每个命令怎么用、参数是什么、会产出什么。命令的实现细节、加解密容器和打包格式的二进制布局见 `docs/zh/testing.md` 的工具链套件清单与源码注释。

---

## 1. 构建与安装

```bash
# 随主工程（默认开 GRYCE_BUILD_TOOLCHAIN）
cmake -S . -B build -G Ninja
cmake --build build --parallel

# 独立构建（不依赖主工程，不装 Python）
cmake -S toolchain -B toolchain/build -G Ninja
cmake --build toolchain/build --parallel
```

构建产物（两处都有）：

| 路径 | 说明 |
| --- | --- |
| `build/toolchain/GryceGC.exe` | 随主工程构建的 CLI |
| `toolchain/build/GryceGC.exe` | 独立构建的 CLI |
| `toolchain/build/GryceGCTests.exe` | 工具链自身单元测试 |

---

## 2. 通用约定

```bash
GryceGC <command> [options]
GryceGC help <command>     # 任意命令的详细参数（与下方用法一致）
GryceGC                   # 无参数 = help：列出全部命令
GryceGC help              # 同上
```

- **退出码**：命令成功返回 `0`；参数错误 / 找不到目标 / 校验失败一般返回 `1` 或 `2`（具体见各命令）。脚本里用它判断成败，别只靠 stderr 文本。
- **位置参数（positionals）**：排在选项之前的裸参数，多数命令只取第一个（如 `new <dir>`、`meta` 的子命令）。
- **透传 `--`**：`run` 这类启动宿主的命令，把 `--` 之后的所有参数**原样**交给宿主进程，GryceGC 不解析。
- **重复选项**：`--extra-dir`、`--target`、`-- <args>` 等可重复出现，会累积。
- **相对路径**：多数 `--build-dir` / `--package` 默认指向约定目录，给绝对路径更稳。

---

## 3. 命令参考

### 3.1 `version`

打印 GryceGC 自身版本。

```bash
GryceGC version
```

---

### 3.2 `new` — 创建项目骨架

生成一套**自包含**的项目文件（不需要你自己的 `main.cpp`，宿主固定，行为由 `main.js` 决定）：

```bash
GryceGC new MyGame                                 # 目录名即项目名
GryceGC new MyGame --name "My Game" --api vulkan --extension
GryceGC new . --force                              # 就地补全：覆盖同名文件，保留其它
```

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--name <name>` | 项目显示名（写进 `project.data` 与窗口标题） | 取目录名 |
| `--width <px>` / `--height <px>` | 默认窗口尺寸 | 1280 / 720 |
| `--api <api>` | 默认渲染后端 `opengl`/`vulkan`/`dx12` | `opengl` |
| `--extension` | 额外生成 `extension.cpp` 原生扩展模板 | 关 |
| `--force` | 目标目录非空也继续：只覆盖清单内的同名文件，不动其它 | 关 |

产物清单：

| 文件 | 作用 |
| --- | --- |
| `main.js` | 入口脚本：建窗、载场景、飞行相机（WASD+鼠标）、Esc 退出 |
| `project.data` | 项目配置：窗口尺寸、默认后端、渲染开关（HDR/色调映射/阴影/环境光/IBL…） |
| `scenes/main.gesc` | 场景（`.gesc` v3 即 JSON）：相机 / 平行光 / 立方体 / 地面 |
| `models/cube.obj` | 程序生成的单位立方体（每面独立顶点，平面法线正确，不依赖仓库资产） |
| `README.md` | 运行与打包命令 |
| `extension.cpp` | 仅 `--extension`：原生扩展模板（把引擎已有、脚本未开放的能力包成 `engine.*` 接口） |

要点：

- 着色器**不**随项目复制：`res:/shaders` 缺失时引擎回退到内置目录（GL 源码与 Vulkan 的 `.spv` 都命中）；要自定义再把 `engine/render/shaders` 拷到项目里改。
- 目标目录非空时默认拒绝；`--force` 只覆盖生成清单内的同名文件，其它文件原样保留——适合「在已有目录里补骨架」。
- 生成后 `build/GryceApp.exe --project <dir>` 即可运行（需先构建 `GryceApp` 目标）。
- 场景是纯 JSON，可直接手改或用编辑器保存；实体字段见 `docs/zh/app-entry.md`。

> 注意后端一致性：本命令的 `--api` 接受 `dx12`，但下面 `package` / `run` 的便捷启动旗只暴露 `opengl`/`vulkan`。实际运行后端由 `project.data` 的 `render_api` 字段决定，所以 `--api dx12` 生成的项目靠 `project.data` 走 D3D12；`package`/`run` 的 `--api` 只是给启动器加一个覆盖参数。

---

### 3.3 `deps` — DLL 依赖闭包

列出某个可执行文件的完整 DLL 依赖闭包，按 **core（引擎核心 DLL） / 依赖（第三方）** 分类，用于排查「为什么这个包在某台机器缺 DLL」。

```bash
GryceGC deps --build-dir build
GryceGC deps --exe build/GryceApp.exe --extra-dir third_party/some/bin
```

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--build-dir <dir>` | CMake 构建目录，自动探测其中 exe | `cmake-build-debug` |
| `--exe <path>` | 直接指定 exe（路径或构建目录内的文件名） | 自动探测 |
| `--extra-dir <dir>` | 额外 DLL 搜索目录（可重复） | 无 |

---

### 3.4 `verify` — 发布包自洽性校验

校验发布目录里所有被导入的 DLL 都能在包内解析（core 应在 exe 同级、其余应在 `dll/` 下）。配合 `package --verify` 自动跑，也可单独对已有包复查。

```bash
GryceGC verify --package dist/GryceEngine
GryceGC verify --package dist/GryceEngine --dll-folder libs
```

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--package <dir>` | 发布包目录（必填） | — |
| `--dll-folder <name>` | 依赖 DLL 所在子目录名 | `dll` |

---

### 3.5 `package` — 打包发布目录

把构建产物 + core DLL + 依赖 DLL + 资源项目 + 启动器 + README 打包成一个**可脱线运行**的目录（移植自旧的 `toolchain/main.py`）。

```bash
GryceGC package --build-dir build --project . --name MyGame --verify
GryceGC package --build-dir build --no-assets --clean --output out
```

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--build-dir <dir>` | CMake 构建目录 | `cmake-build-debug` |
| `--exe <path>` | 指定 exe（默认自动探测 `GryceRenderSmoke.exe`） | 自动 |
| `--output <dir>` | 输出根目录 | `dist` |
| `--name <name>` | 发布包文件夹名 | `GryceEngine` |
| `--project <dir>` | 资源项目根（会被整体拷进包） | `test/demo_project` |
| `--dll-folder <n>` | 依赖 DLL 文件夹名 | `dll` |
| `--api <api>` | 启动器默认后端 `opengl`/`vulkan` | `opengl` |
| `--extra-dir <dir>` | 额外 DLL 搜索目录（可重复） | 无 |
| `--no-assets` | 不打包 demo_project 资源 | 关 |
| `--clean` | 打包前清空输出目录 | 关 |
| `--verify` | 打包后立即跑 `verify` | 关 |

> 约束：`--api` 只接受 `opengl`/`vulkan`（见 3.2 的后端一致性说明）。

---

### 3.6 `run` — 跨平台启动器

注入 `dll/` 到 `PATH` 前端后启动宿主，替代一堆 `run*.bat`。支持从发布包或构建目录直接起。

```bash
GryceGC run --package dist/GryceEngine --play
GryceGC run --build-dir build --api vulkan -- --frames 120 --shot out.png
```

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--package <dir>` | 发布包目录（含启动器的目录） | — |
| `--build-dir <dir>` | 构建目录（直接用构建产物起） | — |
| `--api <api>` | 渲染后端 `opengl`/`vulkan` | `opengl` |
| `--play` | 交互模式（WASD + 鼠标） | 关 |
| `--` | 之后参数原样透传给宿主 | — |

退出码原样传播宿主的退出码。

---

### 3.7 `copyright` — 版权 / 许可印章

给发布物盖版权章：写 `COPYRIGHT.txt`、在 README 末尾补版权页脚、向 exe 写 PE trailer（可选再写 `VERSIONINFO`）。

```bash
GryceGC copyright --package dist/GryceEngine --holder "Your Name" --license AGPL-3.0-or-later
GryceGC copyright --exe build/GryceApp.exe --holder "You" --version-info \
    --product "My Game" --version "1.2.0"
```

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--package <dir>` / `--exe <file>` | 作用于整个包目录，或单个 PE 文件（二选一必填） | — |
| `--holder <name>` | 版权持有者（必填） | — |
| `--year <yyyy>` | 年份 | `2026` |
| `--license <id>` | SPDX 或自定义许可标识（如 `MIT`） | 无 |
| `--license-file <f>` | 许可全文文件，追加进 `COPYRIGHT.txt` | 无 |
| `--version-info` | 同时写 PE `VERSIONINFO` 资源（仅 Windows） | 关 |
| `--product <name>` | `VERSIONINFO` ProductName | 无 |
| `--version <ver>` | `VERSIONINFO` ProductVersion | 无 |

---

### 3.8 `meta` — 元数据读写

读写三类元数据：`gryce.meta.json`（项目元信息）、`project.data`（项目配置）、PE trailer（exe 内嵌版本戳）。

```bash
GryceGC meta set   --target ./MyGame --project --key engine_version --value ">=0.2.0"
GryceGC meta get   --target ./MyGame --project --key name
GryceGC meta show  --target ./MyGame
GryceGC meta stamp-git --target ./MyGame
```

子命令：

| 子命令 | 作用 |
| --- | --- |
| `set` | 写键值（`--key` + `--value` 必填） |
| `get` | 读键值并打印（`--key` 必填） |
| `show` | 打印目标上所有可读元数据 |
| `stamp-git` | 写入 git HEAD 短戳与提交日期 |

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--key <k>` | 键名（set/get 必填） | — |
| `--value <v>` | 值（set 必填） | — |
| `--target <path>` | 目录 / PE 文件 | 当前目录 |
| `--project` | 把 target 当作 `<target>/project.data` 处理 | 关 |

---

### 3.9 `encrypt` / `decrypt` — 加解密

对称加解密，支持 **AES-256-GCM** 与 **ChaCha20**，与引擎 `aes_gcm_buf` 字节兼容，可用于加密 `.pak` 之外任意资源。

```bash
GryceGC keygen --bytes 32 --hex --out key.hex
GryceGC encrypt --in a.bin --out a.enc --key @key.hex --algo aes-gcm --aad "hdr"
GryceGC decrypt --in a.enc --out a.bin --key @key.hex
```

`encrypt` 选项：

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--in <f>` / `--out <f>` | 输入 / 输出文件（必填） | — |
| `--key <spec>` | 密钥：**64 位 hex** / `@文件`（读文件） / `env:VAR` / `prompt:提示语` | — |
| `--algo <a>` | `aes-gcm`（默认） / `chacha20` | `aes-gcm` |
| `--aad <text>` | 附加认证数据（不参与加密，参与完整性校验） | 空 |
| `--raw` | 不写 `GRYCENC1` 容器头，直接 `iv||ct||tag`（引擎裸格式兼容） | 关 |

`decrypt` 自动识别 `GRYCENC1` 容器；无容器时按 `--algo` 走裸格式（此时也认 `--aad`）。

`--key` 的四种取法：

- `0011...ff`（64 个 hex 字符）：直接当 32 字节密钥。
- `@key.bin`：从文件读原始字节作为密钥。
- `env:MY_KEY`：从环境变量读。
- `prompt:请输入密钥`：交互式输入（不在日志/历史里留明文）。

---

### 3.10 `keygen` — 生成随机密钥

生成密码学安全的随机密钥。

```bash
GryceGC keygen                  # 默认 32 字节，base64/原始打印
GryceGC keygen --bytes 16 --hex --out key.hex
```

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--bytes <N>` | 密钥长度（字节） | `32` |
| `--hex` | 以十六进制输出（便于 `--key @file` 复用） | 关 |
| `--out <file>` | 写入文件（否则打印到 stdout） | stdout |

---

### 3.11 `pak` — 资源包管理

创建 / 列出 / 解包 / 校验资源包。支持两种格式：

- `.gpack`（容器标识 `GPAK`，v1）：未加密资源包。
- `.pak`（v3 + v4）：v4 支持 AES 加密（`--key hex`）。

```bash
GryceGC pak create  --out game.gpack  assets/ models/ logo.png
GryceGC pak create  --out game.pak --key <hex> assets/        # v4 加密
GryceGC pak list    --file game.pak
GryceGC pak extract --file game.pak --out extracted/
GryceGC pak verify  --file game.pak                            # CRC32 校验
```

| 子命令 | 选项 |
| --- | --- |
| `create` | `--out <f>` + 一个或多个输入文件/目录；`--key <hex>`（仅 `.pak` v4 加密） |
| `list` | `--file <f>`；`--key <hex>`（加密包需要） |
| `extract` | `--file <f>` + `--out <dir>`；`--key <hex>` |
| `verify` | `--file <f>`；`--key <hex>`（CRC32 完整性校验） |

---

### 3.12 `debug` — 调试启动器

以一组预设调试环境变量启动宿主（D3D12 debug layer / trace / probe / glog），不用手敲一长串 `set GRYCE_...`。

```bash
GryceGC debug --build-dir build --d3d12 --gpu-validation
GryceGC debug --package dist/GryceEngine --trace-frame 10 --probe-ssr --with-debugger windbg
```

| 选项 | 设置的环境变量 |
| --- | --- |
| `--d3d12` | `GRYCE_D3D12_DEBUG=1` |
| `--gpu-validation` | `GRYCE_D3D12_GPU_VALIDATION=1` |
| `--serialize` | `GRYCE_D3D12_SERIALIZE=1` |
| `--trace-hdr` | `GRYCE_TRACE_HDR=1` |
| `--trace-frame <N>` | `GRYCE_DX12_TRACE_FRAME=N` |
| `--trace-bind` | `GRYCE_DX12_TRACE_BIND=1` |
| `--probe-ssr` | `GRYCE_DX12_PROBE_SSR=1` |
| `--probe-frame <N>` | `GRYCE_DX12_PROBE_FRAME=N` |
| `--probe-cbv <NAME>` | `GRYCE_DX12_PROBE_CBV=NAME` |
| `--ring` | `GRYCE_DX12_PROBE_RING=1` |
| `--glog <N>` | `GLOG_minloglevel=N` |
| `--with-debugger <D>` | `cdb` / `windbg` / `x64dbg`（启动后挂调试器） |
| `--api <api>` | `opengl` / `vulkan` / `dx12` |

---

### 3.13 `bench` — 帧耗时 A/B 基准

连续跑若干帧、测平均/最小帧耗时，输出单行 KV（移植自 `bench_ab.sh`），适合「改之前 vs 改之后」对比。

```bash
GryceGC bench --build-dir build --w 1280 --h 720 --runs 5 --api opengl
```

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--build-dir <dir>` | 构建目录（必填） | — |
| `--w <N>` / `--h <N>` | 渲染分辨率 | — |
| `--runs <N>` | 重复轮数 | `3` |
| `--api <api>` | 后端 | `opengl` |

> 提示：单轮基准不可信，至少 3~4 轮交替取最小值才有意义。

---

### 3.14 `font` — 重新生成字体数据

把 TTF 烘焙成 `engine/render/imgui_font_data.h`（imgui / 内置 UI 用的内置字体，避免运行时读文件）。

```bash
GryceGC font                                   # 用默认 Cousine 源
GryceGC font path/to/MyFont.ttf --out engine/render/imgui_font_data.h
```

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| 位置参数 | 源字体 `.ttf` | `third_party/imgui/misc/fonts/Cousine-Regular.ttf` |
| `--out <file>` | 输出头文件路径 | `engine/render/imgui_font_data.h` |

---

### 3.15 `build` — cmake 封装

封装 `cmake configure + build`，复用主工程工具链（避免手敲一长串 `-D` 开关）。

```bash
GryceGC build --preset release --d3d12 on --lto --target GryceApp --jobs 8
GryceGC build --clean --test
```

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--build-dir <dir>` | 构建目录 | `build` |
| `--preset <p>` | `debug`（默认） / `release` | `debug` |
| `--target <name>` | 构建目标（可重复） | 全部 |
| `--d3d12 <on|off>` | D3D12 后端开关 | `on` |
| `--lto` | 开启 LTO（链接期优化） | 关 |
| `--test` | 构建后跑 CTest | 关 |
| `--clean` | 构建前清空目录 | 关 |
| `--jobs <N>` | 并行度 | 自动 |

---

### 3.16 `webpack` — 单文件 Web 打包

把 Emscripten 的多文件 Web 产物（`.html` + `.js` + `.wasm` + `.data`）合并成一个**自包含** `.single.html`，`file://` 双击即用，零外部网络请求。

```bash
GryceGC webpack --build-dir build-webgl --name GryceRenderSmoke
# 产物：build-webgl/GryceRenderSmoke.single.html（零外部引用）
```

原理：内联 Emscripten 的 `.js` 进 HTML 壳，再经 `Module.locateFile`（`.wasm` 返回 base64 data URL）与 `Module.getPreloadedPackage`（`.data` 直接作为 ArrayBuffer 交给 file packager）消除运行时 `fetch`。属于**链接后处理**，不碰构建系统、不需要重新链接。渲染输出与多文件 HTTP 版逐字节相同。详细对比与约束见 `docs/zh/webgl-backend.md` 第 1 节。

| 选项 | 含义 | 默认 |
| --- | --- | --- |
| `--build-dir <dir>` | 构建目录 | `build-webgl` |
| `--name <name>` | 应用基名 / Emscripten 输出前缀 | `GryceRenderSmoke` |
| `--html <file>` | 入口 html | `<build-dir>/<name>.html` |
| `--js <file>` | Emscripten JS | `<build-dir>/<name>.js` |
| `--wasm <file>` | wasm | `<build-dir>/<name>.wasm` |
| `--data <file>` | file packager 数据（可缺） | `<build-dir>/<name>.data` |
| `--output <file>` | 输出单文件 html | `<build-dir>/<name>.single.html` |

---

## 4. 典型工作流

```bash
# 1. 新建项目骨架
GryceGC new MyGame --name "My Game" --api vulkan --extension
cd MyGame

# 2. 构建宿主（需要先构建 GryceApp）
cmake --build ../build --target GryceApp
../build/GryceApp.exe --project . --frames 60 --shot shot.png

# 3. 打包发布目录并自校验
GryceGC package --build-dir ../build --project . --name MyGame --verify

# 4. 盖版权章
GryceGC copyright --package dist/MyGame --holder "Your Name" --license AGPL-3.0-or-later

# 5. 脱线运行（交互模式）
GryceGC run --package dist/MyGame --play

# 6. 若要发 Web 单文件
GryceGC webpack --build-dir build-webgl --name MyGame
```

加密资源流程（游戏内敏感资产）：

```bash
GryceGC keygen --bytes 32 --hex --out key.hex
GryceGC pak create --out game.pak --key @key.hex assets/      # v4 加密包
GryceGC pak verify --file game.pak --key @key.hex             # 校验
```

---

## 5. 测试

`grycegc_*` 系列 CTest 用例覆盖命令面与加解密 / 打包的正确性，清单见 `docs/zh/testing.md`。

```bash
# 工具链用例并行会互相干扰（共享临时目录与进程级状态），本地请串行跑
ctest -R grycegc -j1 --output-on-failure
```

宿主 `GryceGCTests.exe` 也可直接跑单个套件：

```bash
toolchain/build/GryceGCTests.exe --suite grycegc_new
```
