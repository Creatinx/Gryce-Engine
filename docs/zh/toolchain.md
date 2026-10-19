# GryceGC 工具链

`toolchain/` 是引擎的命令行工具链（C++23，自包含，零引擎 DLL 依赖）：把需要的 engine 源码（aes_gcm / gpack / pak / glog / crypto）直接编进 `grycegc_core` 静态库。随主工程一起构建（`GRYCE_BUILD_TOOLCHAIN=ON` 默认开），也可独立配置。

## 1. 构建

```bash
# 随主工程（默认）
cmake -S . -B build -G Ninja && cmake --build build --parallel

# 独立构建（不装 Python）
cmake -S toolchain -B toolchain/build -G Ninja
```

## 2. 命令面

`GryceGC help <command>` 查看完整参数。

| 命令 | 作用 |
| --- | --- |
| `version` | 打印 GryceGC 版本 |
| `help` | 显示帮助（无参数运行等价于 `help`） |
| `new` | 创建项目骨架（`main.js` + `project.data` + 场景 + 立方体模型） |
| `deps` | 列出 exe 的完整 DLL 依赖闭包（按 core / 依赖分类） |
| `verify` | 校验发布包自洽性（所有导入 DLL 在包内可解析） |
| `package` | 打包成可脱线运行的发布目录（exe + core DLL 同级 + `dll/` + `demo_project` + 启动器 + README） |
| `run` | 跨平台启动器：注入 `dll/` 到 PATH 后启动宿主（替代 `run*.bat`） |
| `copyright` | 版权印章：COPYRIGHT.txt + README 页脚 + PE trailer（可选 VERSIONINFO） |
| `meta` | 元数据读写：`gryce.meta.json` / `project.data` / PE trailer / git 戳 |
| `encrypt` / `decrypt` | AES-256-GCM / ChaCha20 加解密（与引擎 `aes_gcm_buf` 字节兼容） |
| `keygen` | 生成密码学安全随机密钥（默认 32 字节） |
| `pak` | 资源包 create / list / extract / verify（`.gpack` v1 / `.pak` v3+v4 加密） |
| `debug` | 以预设调试环境变量启动宿主（D3D12 debug layer / trace / probe / glog） |
| `bench` | 帧耗时 A/B 基准（单行 KV 输出） |
| `font` | 重新生成 `engine/render/imgui_font_data.h` |
| `build` | cmake configure + build 封装 |
| `webpack` | 把 Emscripten 多文件 Web 产物打成一个自包含 HTML |

## 3. 创建项目（`new`）

```bash
GryceGC new MyGame                        # 目录名即项目名
GryceGC new MyGame --name "My Game" --api vulkan --extension
GryceGC new . --force                     # 就地补全：覆盖同名文件，保留其它
```

产物：

| 文件 | 作用 |
| --- | --- |
| `main.js` | 入口脚本：建窗、载场景、飞行相机、Esc 退出 |
| `project.data` | 项目配置：窗口尺寸、默认后端、渲染开关 |
| `scenes/main.gesc` | 场景（`.gesc` v2 就是 JSON）：相机 / 平行光 / 立方体 / 地面 |
| `models/cube.obj` | 程序生成的单位立方体，不依赖仓库资产 |
| `README.md` | 运行与打包命令 |
| `extension.cpp` | 仅 `--extension`：原生扩展模板 |

要点：

- 项目不需要自己的 `main.cpp`；宿主固定，行为由 `main.js` 决定（见 [app-entry.md](app-entry.md)）。
- 不复制着色器：`res:/shaders` 缺失时引擎回退到内置目录（GL 源码与 Vulkan 的 `.spv` 都命中），要自定义再把 `engine/render/shaders` 拷进项目。
- 目标目录非空时默认拒绝；`--force` 只覆盖生成清单内的同名文件，不动其它文件。
- 生成后 `build/GryceApp.exe --project <dir>` 即可运行（需先构建 `GryceApp`）。

## 4. 典型发布流程

```bash
GryceGC package --build-dir build --verify
GryceGC copyright --package dist/GryceEngine --holder "Your Name" --license AGPL-3.0-or-later
GryceGC run --package dist/GryceEngine -- --frames 120 --shot out.png
```

## 5. 单文件 Web 分发（`webpack`）

CMake 的 Web 构建只产多文件（`.html` + `.js` + `.wasm` + `.data`）。要"一个 html 双击即用"的形态时用打包器做链接后处理，不碰构建系统、不重新链接：

```bash
toolchain/build/GryceGC.exe webpack --build-dir build-webgl --name GryceRenderSmoke
# 产物：build-webgl/GryceRenderSmoke.single.html（~5.1MB，零外部引用）
```

实现（`toolchain/src/core/web_pack.cpp`）：内联 Emscripten 的 `.js` 进 html 壳，再经 `Module.locateFile`（`.wasm` 返回 base64 data URL）与 `Module.getPreloadedPackage`（`.data` 直接作为 ArrayBuffer 交给 file packager）消除剩余网络请求。渲染输出与多文件 HTTP 版逐字节相同。详细对比与约束见 [webgl-backend.md](webgl-backend.md) 第 1 节。

## 6. 测试

`grycegc_*` 系列 CTest 用例覆盖命令面与加解密 / 打包的正确性，清单见 [testing.md](testing.md)。
