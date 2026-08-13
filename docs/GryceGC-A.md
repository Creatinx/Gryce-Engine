# GryceGC-A 项目与打包标准

> GryceGC-A（Standard A）是 Gryce Engine 的游戏项目组织 + 发布打包标准：
> 一个 **GryceGC-A 项目** 是磁盘上一个自描述的目录（`project.gryce` 清单 +
> `project_settings.json` 运行时设置 + 分类资源子目录），由 **GryceGC**（`tools/grycegc`，
> 构建产物 `grycegc.exe`）打包成可独立分发的游戏目录（`exe + runtime/ + assets/*.gpkg +
> gdata`），再由 **GryceSPC 模板**（`templates/game_main.cpp`，构建产物 `GryceGame.exe`）
> 作为游戏入口运行。
>
> 本文档与实现代码同步维护；实现入口：
> [`tools/grycegc/main.cpp`](../tools/grycegc/main.cpp)、
> [`templates/game_main.cpp`](../templates/game_main.cpp)、
> [`core/api/core_api.cpp`](../core/api/core_api.cpp)（自动挂载 / 主场景）、
> [`core/assets/asset_manager.cpp`](../core/assets/asset_manager.cpp)（包内资源解析）。

---

## 1. 标准总览

```text
examples/2dDemo（或任意游戏项目目录）
  │  1. 项目自描述：project.gryce + project_settings.json
  │  2. 资源按类别分目录：scenes/ scripts/ shaders/ models/ textures/
  │     audio/ fonts/ config/ tilesets/ ...
  ▼
GryceGC（grycegc.exe）
  │  打包：按类别生成 .gpkg（GPAK 格式）+ 拷贝模板 exe/runtime DLL + 生成 gdata
  ▼
发布目录 <out>/<name>/
  ├─ <name>.exe          GryceGame 模板入口（延迟加载 runtime/ 的 DLL）
  ├─ runtime/            核心 DLL（Core/Renderer/Platform/Physics）+ GLFW + CRT 运行时
  ├─ assets/*.gpkg       分类资源包（scenes/scripts/shaders/models/textures/audio/fonts/config/misc）
  └─ gdata               包元数据（源文件 SHA-256 记录 + 64 字节 SHA-512 密钥 + 作者）
```

“最新标准”的要点（与历史打包方式相比）：

- 不再复制 `res/` 目录，游戏内容全部进入 `.gpkg` 资源包；
- 游戏入口对核心 DLL **延迟加载**，从 `runtime/` 子目录解析；
- Core 启动时自动挂载项目根与 `assets/` 下的 `.gpkg/.gpack`，`res:/` 资源统一走
  “真实文件优先、包内提取兜底”；
- 游戏启动自动进入主场景（`project_settings.json` 的 `main_scene`，缺省
  `res:/scenes/main.gesc`），可用 `--scene` 覆盖；
- `gdata` 记录每个源文件的 SHA-256 / 大小，并由源记录派生 64 字节 SHA-512 密钥，
  用于校验包内容完整性。

---

## 2. 项目目录结构（GryceGC-A 项目）

一个合格的 GryceGC-A 项目是**项目根目录（project root）**本身，`res:/` 虚拟路径以它为根。

```text
<project>/
├── project.gryce            # 项目清单（名称/版本/入口场景/物理/窗口）
├── project_settings.json    # 运行时设置（渲染参数 + main_scene）
├── scenes/                  # 场景 .gesc
├── scripts/                 # Lua 脚本 .lua
├── shaders/                 # 着色器 .vert/.frag/.glsl/.spv ...
├── models/                  # 模型 .obj/.fbx/.gltf/...
├── textures/                # 贴图 .png/.jpg/.dds/...
├── audio/                   # 音频 .wav/.ogg/...
├── fonts/                   # 字体 .ttf/.otf/...
├── config/（或任意位置）     # 配置 .json/.gryce/.cfg/...
├── tilesets/                # Tilemap 瓦片集 JSON（按扩展名归入 config 类）
└── ...（其它资源自动归入 misc）
```

### 2.1 `project.gryce` — 项目清单

声明式元数据（供工具链 / 编辑器 / 示例识别项目）。当前字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 项目名（示例中与目录名一致，如 `2dDemo`） |
| `version` | string | 项目版本（如 `0.1.0`） |
| `engine_version` | string | 引擎版本约束（如 `>=0.1.0`） |
| `entry_scene` | string | 入口场景 `res:/` 路径（默认约定 `res:/scenes/main.gesc`） |
| `physics.backend_2d` | string | 2D 物理后端（`box2d`） |
| `physics.backend_3d` | string | 3D 物理后端（`jolt`） |
| `window.width/height/title` | int/string | 默认窗口尺寸与标题 |

示例（`examples/2dDemo/project.gryce`）：

```json
{
  "name": "2dDemo",
  "version": "0.1.0",
  "engine_version": ">=0.1.0",
  "entry_scene": "res:/scenes/main.gesc",
  "physics": { "backend_2d": "box2d" },
  "window": { "width": 1280, "height": 720, "title": "Gryce Engine - 2D Platformer Demo" }
}
```

### 2.2 `project_settings.json` — 运行时设置

Core 与编辑器读取的设置文件。**Core 当前只消费 `main_scene` 字段**（其余渲染字段由编辑器
在启动/保存时维护）。

| 字段 | 类型 | 说明 |
|---|---|---|
| `render_api` | string | `opengl` / `vulkan` |
| `hdr` / `tone_map_mode` / `exposure` | bool/int/float | 后处理参数 |
| `shadow_enabled` / `shadow_map_size` | bool/int | 阴影开关与级联贴图尺寸 |
| `ambient_r/g/b` / `ibl_intensity` | float | 环境光 / IBL 强度 |
| `main_scene` | string | 主场景 `res:/` 路径；缺省 `res:/scenes/main.gesc` |

示例：

```json
{
  "render_api": "opengl",
  "hdr": true,
  "main_scene": "res:/scenes/main.gesc"
}
```

### 2.3 `res:/` 虚拟路径

- `res:/path/to/file` 解析为 `<project_root>/path/to/file`；
- 打包后没有真实文件时，同一路径会从挂载的 `.gpkg` 中提取（见 §5.3）；
- 统一 UTF-8，支持中文文件名。

### 2.4 资源分类（→ .gpkg）

打包时按扩展名把项目文件分组，每组生成一个 `<name>.<category>.gpkg`：

| 类别 | 扩展名 |
|---|---|
| `scenes` | `.gesc` `.scene` `.tscn` |
| `scripts` | `.lua` |
| `shaders` | `.vert` `.frag` `.geom` `.tesc` `.tese` `.comp` `.glsl` `.hlsl` `.spv` |
| `models` | `.obj` `.fbx` `.gltf` `.glb` `.dae` `.ply` `.stl` `.3ds` `.blend` |
| `textures` | `.png` `.jpg` `.jpeg` `.bmp` `.tga` `.dds` `.ktx` `.hdr` `.exr` `.gif` `.webp` |
| `audio` | `.wav` `.ogg` `.mp3` `.flac` `.aac` |
| `fonts` | `.ttf` `.otf` `.fnt` `.woff` `.woff2` |
| `config` | `.json` `.gryce` `.cfg` `.ini` `.toml` `.yaml` `.yml` `.mat` `.txt` |
| `misc` | 其它所有可打包扩展名 |

`--single` 模式下全部内容合并进单个 `<name>.gpkg`。

### 2.5 打包排除规则

以下内容**不会**进入资源包（源码 / 构建产物 / 仓库文件）：

- 目录：`.git` `.vs` `.idea` `__pycache__` `build` `bin` `obj` `x64` `out`；
- 扩展名：`.cpp` `.cc` `.cxx` `.c` `.h` `.hpp` `.hh` `.inl` `.py` `.md` `.sln`
  `.pdb` `.ilk` `.exp` `.lib` `.dll` `.exe`；
- 文件名（不区分大小写）：`cmakelists.txt` `.gitignore` `.gitattributes`
  `license` `readme.md`。

---

## 3. 打包（grycegc）

### 3.1 命令行

```text
grycegc --project <dir> [选项]
  --project <dir>   游戏项目目录（res:// 根）【必填】
  --name <name>     输出游戏名（默认 MyGame）
  --build-dir <dir> CMake 构建目录（默认 build；用于定位 GryceGame.exe 与 bin/）
  --config <cfg>    Debug 或 Release（默认 Release）
  --out <dir>       输出父目录（默认 build/game）
  --author <name>   gdata 中的作者（默认 %USERNAME%）
  --single          全部资源打包进单个 <name>.gpkg
```

前置条件：先构建 `GryceGame`（模板入口）与 `GryceGC` 目标：

```bat
cmake --build build --target GryceGame GryceGC --config Release
```

打包示例（2dDemo）：

```bat
build/bin/Release/grycegc.exe --project examples/2dDemo --name 2dDemo ^
    --build-dir build --config Release --out build/game --author "Your Name"
```

编辑器菜单「文件 → 打包运行（GryceGC）」会自动定位/构建 `grycegc.exe` 并执行打包。

### 3.2 输出布局

```text
<out>/<name>/
├── <name>.exe              # GryceGame 模板入口（对核心 DLL 延迟加载）
├── runtime/                # 核心运行时 DLL + GLFW + 编译器运行时
│   ├── GryceCore.dll（Debug 构建为 GryceCored.dll）
│   ├── GryceRenderer.dll（Debug 为 GryceRendererd.dll）
│   ├── GrycePlatform.dll（Debug 为 GrycePlatformd.dll）
│   ├── GrycePhysics.dll（Debug 为 GrycePhysicsd.dll）
│   ├── glfw3.dll（MSVC Debug 为 glfw3d.dll）
│   ├── MSVC CRT（vcruntime140/msvcp140... 或 Debug 变体 + ucrtbased.dll）
│   └── MinGW/GCC 运行时（libgcc_s_seh-1.dll / libstdc++-6.dll / libwinpthread-1.dll）
├── assets/
│   ├── <name>.scenes.gpkg
│   ├── <name>.scripts.gpkg
│   ├── <name>.shaders.gpkg
│   ├── <name>.textures.gpkg
│   ├── ...（每类别一个，--single 时只有 <name>.gpkg）
├── project_settings.json   # 项目根设置原样复制（游戏入口从 exe 目录读取 main_scene 等）
├── project.gryce           # 项目清单（文档用途，同时也在 config 包内）
└── gdata                  # 包元数据（JSON）
```

> 打包前会清空旧输出目录（`<out>/<name>`），避免残留文件混入新包。

### 3.3 `gdata` 元数据

`gdata` 是发布目录根部的 JSON 文件，格式 `gryce_gdata` v1：

```json
{
  "format": "gryce_gdata",
  "version": 1,
  "project": "<name>",
  "author": "<author>",
  "created": "2026-08-12T22:16:50",
  "tool": "grycegc",
  "key_sha512_hex": "<128 hex chars>",
  "sources": [
    { "path": "scenes/main.gesc", "sha256": "<64 hex chars>", "size": 12345 }
  ]
}
```

- `sources`：每个被打包文件的 `path`（相对项目根，正斜杠）+ SHA-256 + 字节数；
- `key_sha512_hex`：对排序后的 `path:size:sha256` 记录做 SHA-512 得到的 **64 字节密钥**
  （hex 编码 128 字符），可用于校验包内容是否被改动；
- `author` 默认取 `%USERNAME%`，可用 `--author` 覆盖。

---

## 4. 游戏入口（GryceSPC 模板）

`templates/game_main.cpp`（`GryceGame.exe`）是发布游戏的唯一入口，职责：

1. **启动早期扩展 DLL 搜索路径**：模板对核心 DLL 使用延迟加载（MSVC `/DELAYLOAD`），
   `main()` 先把 `<exe>/runtime/` 加入 DLL 搜索目录，再首次调用引擎 API；
2. **优先使用系统 VC++ 运行时**：从 System32 显式预加载 CRT；系统缺失时才回退到
   `runtime/` 中随包携带的运行时；
3. **确定项目根**：默认取 exe 所在目录（支持双击运行）；`--project <dir>` 覆盖；
4. **初始化引擎**：`GCore_Init` → 物理（Jolt 3D + Box2D 2D 同时挂载）→ 窗口 →
   渲染器（OpenGL）→ 进入 Play Mode；
5. **自动进入主场景**：`GCore_SetAutoLoadMainScene(true)`，启动即加载
   `project_settings.json` 的 `main_scene`。

运行参数：

```text
<name>.exe                          # 默认：项目根 = exe 目录，加载主场景
<name>.exe --scene res:/scenes/x.gesc   # 覆盖主场景
<name>.exe --project <dir>          # 覆盖项目根
<name>.exe --w 1920 --h 1080        # 覆盖窗口尺寸
```

---

## 5. 运行时行为

### 5.1 资源包自动挂载

`GCore_Init` 扫描项目根**和 `<root>/assets/`** 下所有 `.gpkg/.gpack` 文件并挂载到
`AssetManager`。旧布局（包直接放项目根）仍然兼容。

### 5.2 `res:/` 解析优先级

统一入口 `AssetManager::resolve_for_reading`：

1. 项目根下的真实文件优先；
2. 文件不存在时，从已挂载的包中按内部路径（`scenes/main.gesc`）提取到临时目录并返回；
3. 都找不到时返回空，调用方记录警告。

> 场景（`.gesc`）、Lua 脚本（`.lua`）、着色器、贴图、tileset JSON、模型等一律走该入口；
> 任何直接以 `ResourcePath::resolve` + 文件流打开的资源都不会在打包产物中生效。

### 5.3 主场景

- 主场景路径来自 `project_settings.json` 的 `main_scene`，缺省
  `res:/scenes/main.gesc`；
- 模板启动时 `GCore_SetAutoLoadMainScene(true)`；编辑器不启用（由编辑器自行管理场景）；
- `--scene` 覆盖时模板跳过自动加载，改为显式加载指定场景；
- 运行中切场景用 Lua：`engine.scene.load("res:/scenes/xxx.gesc")`。

### 5.4 物理

`GPhysics_Init` 总是同时创建 3D（Jolt）与 2D（Box2D）世界，
`GPhysics_AttachSystems` 同时注册 `PhysicsSystem3D` 与 `PhysicsSystem2D`，
因此打包的 2D/3D 游戏都能在 Play Mode 下真实模拟。

---

## 6. 符合性检查清单

项目要符合最新的 GryceGC-A 标准，需满足：

- [ ] 目录包含 `project.gryce`（`name` 与目录名一致）与 `project_settings.json`
      （含 `main_scene`）；
- [ ] `scenes/` 下有 `main.gesc`（或 `main_scene` 指向的实际场景文件）；
- [ ] 资源按类别目录组织，源文件（`.cpp/.h/...`）与构建产物不会混入资源目录；
- [ ] `grycegc --project <dir> --name <name> --build-dir build --config <cfg> --out build/game`
      打包成功（退出码 0）；
- [ ] 产物 `build/game/<name>/<name>.exe` 双击可启动，日志出现
      `main scene loaded`，且无 `failed to open / resolve` 类错误；
- [ ] 着色器、贴图、脚本等所有运行时资源都能从 `assets/*.gpkg` 解析（不依赖源目录）。

---

## 7. 仓库中的 GryceGC-A 项目

- `examples/3dtest` — 3D 综合演示（PBR/阴影/物理/关节/角色/碎裂/动画/音频）；
- `examples/2dDemo` — 2D 平台跳跃游戏（Tilemap/2D 光照/昼夜循环/粒子/视差/音效/角色控制器/
  敌人/枪械/关卡过关条件），关卡为编辑器可加载的 `.gesc` 场景（`scenes/level_*.gesc`），
  **玩法逻辑全部在 GryceSRT Lua 脚本**（`scripts/*.lua`）；打包产物的 GryceGame 模板会同步
  输入并运行同一套 Lua 玩法，可独立游玩。

两者同时保留“开发模式”可执行文件（直接链接引擎内部 C++ API 的 `3dtest.exe` /
`2dDemo.exe`）与发布产物（GryceGame 模板）跑同一套 Lua 玩法；发布走 GryceGC-A 打包流程。

---

> 相关文档：[GryceSRT 脚本 API](./GryceSRT_API.md)（Lua 脚本）、
> [已实现功能](./已实现功能.md)（功能状态）、[架构说明](./架构说明.md)（模块划分）。
