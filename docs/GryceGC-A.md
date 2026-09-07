# GryceGC-A 项目与打包标准

> GryceGC-A（Standard A）是 Gryce Engine 的游戏项目组织 + 发布打包标准：
> 一个 **GryceGC-A 项目** 是磁盘上一个自描述的目录（唯一配置 `project.gproj`，
> 其中合并了项目清单、运行时设置 **与打包元数据**，外加分类资源子目录），由
> **GryceGC**（`../tools/GryceGC`，构建产物 `GryceGC.exe`）打包成可独立分发的游戏
> 目录（`exe + runtime/ + assets/*.gpkg + project.data`），再由 **GryceSPC 模板**
> （`templates/GameTemplates.cpp`，构建产物 `GryceGame.exe`）作为游戏入口运行。
>
> 本文档与实现代码同步维护；实现入口：
> [`../tools/GryceGC`](../tools/GryceGC/main.cpp)、
> [`templates/GameTemplates.cpp`](templates/GameTemplates.cpp)、
> [`src/api/core_api.cpp`](../src/api/core_api.cpp)（自动挂载 / 主场景 / 解密密钥）、
> [`src/assets/asset_manager.cpp`](../src/assets/asset_manager.cpp)（包内资源解析）、
> [`src/resources/pak_bundle.cpp`](../src/resources/pak_bundle.cpp)（GPAK v4 加密读写）。

---

## 1. 标准总览

```text
examples/<your-project-dir>（或任意游戏项目目录）
  │  1. 项目自描述：project.data（唯一文件 = 清单 + 运行时设置）
  │  2. 资源按类别分目录：scenes/ scripts/ shaders/ models/ textures/
  │     audio/ fonts/ config/ tilesets/ ...
  ▼
GryceGC（GryceGC.exe）
  │  打包：每个文件单独加密打包成一个 .gpkg（GPAK v4，随机 Base64 名，
  │        数据区 ChaCha20 加密）+ 拷贝模板 exe/runtime DLL，
  │        并把打包元数据与解密密钥合并进 project.data
  ▼
发布目录 <out>/<name>/
  ├─ <name>.exe          GryceGame 模板入口
  ├─ <name>.dll 等         运行时 DLL 的加载期闭包（MinGW 构建镜像到根目录）*
  ├─ runtime/            核心 DLL（Core/Renderer/Platform/Physics）+ GLFW + CRT 运行时的规范化存放
  ├─ assets/*.gpkg       每个资源一个加密资源包（随机 Base64 包名，无逻辑含义）
  └─ project.data        唯一状态文件（JSON）：清单 + 运行时设置 + 打包元数据
                         （源文件 SHA-256 记录 + 64 字节 SHA-512 密钥 + 作者 +
                         .gpkg 解密密钥 enc_key_hex）
```


- 引擎入口对核心 DLL 的加载方式取决于工具链：
  - **MSVC**：`/DELAYLOAD` 延迟加载，`main()` 先 `SetDllDirectoryW(runtime/)` 再首次调用；
  - **MinGW**(无 `/DELAYLOAD`)：exe 在进程启动期静态导入引擎 DLL，因此
    `grycegc` 会把 `runtime/` 的**完整加载闭包镜像到 exe 根目录**，保证加载期能解析；
- **CRT 解析**：MSVC 优先用 System32 的系统 CRT，缺失时回退到随包携带的副本（Debug
  版同时平铺到根目录，因为 CRT 是启动期静态导入）；MinGW 则用 libgcc/libstdc++/libwinpthread；
- Core 启动时自动挂载项目根与 `assets/` 下的 `.gpkg/.gpack`，`res:/` 资源统一走
  “真实文件优先、包内提取兜底”；
- 游戏启动自动进入主场景（`project.gproj` 的 `main_scene`，缺省
  `res:/scenes/main.gesc`），可用 `--scene` 覆盖；
- **加密**：发布时每个 `.gpkg` 的 data 区用随机 32 字节密钥（ChaCha20）加密；运行时
  从根目录 `project.gproj` 的 `enc_key_hex` 读取密钥并解密。`project.gproj` 另含由源记录
  派生的 64 字节 SHA-512 密钥（`key_sha512_hex`）用于校验包内容完整性。

---

## 2. 项目目录结构（GryceGC-A 项目）

一个合格的 GryceGC-A 项目是**项目根目录（project root）**本身，`res:/` 虚拟路径以它为根。

```text
<project>/
├── project.data            # 唯一配置（清单 + 运行时设置合并；打包时合并打包元数据）
├── scenes/                 # 场景 .gesc
├── scripts/                # JS 脚本 .js（ES Module，QuickJS 运行时）
├── shaders/                # 着色器 .vert/.frag/.glsl/.spv ...（示例项目共用 common/shaders/）
├── models/                 # 模型 .obj/.fbx/.gltf/...
├── textures/               # 贴图 .png/.jpg/.dds/...
├── audio/                  # 音频 .wav/.ogg/...
├── fonts/                  # 字体 .ttf/.otf/...
├── tilesets/               # Tilemap 瓦片集 JSON
└── ...（其它资源自动归入 misc）
```

### 2.1 `project.gproj` — 唯一配置 + 打包元数据

`project.gproj` 是项目根**唯一**的状态文件，内容为 **JSON**（不是二进制）。它合并了三类信息：

1. **项目清单**：`name/version/...` 等（§2.1.1）；
2. **运行时设置**：`render_api`/`main_scene` 等（§2.1.2，原独立配置 `project_settings.json` 的内容）；
3. **打包元数据**：源文件记录、完整性密钥、作者与 `.gpkg` 解密密钥（§3.3，原独立文件 `gdata` 的内容）。

> 后缀 `data`（原本是 `gproj`）仅表示约定名称，其内容始终是 JSON。开发期
> `project.gproj` 只有清单与运行时设置； **打包后**同文件会被 GryceGC 追加上打包元数据
> 字段（原 `gdata` 的内容），从而发布目录不再有单独的 `gdata` 文件。

#### 2.1.1 项目清单字段

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 项目名（示例中与目录名一致，如 `2dDemo`） |
| `version` | string | 项目版本（如 `0.1.0`） |
| `engine_version` | string | 引擎版本约束（如 `>=0.1.0`） |
| `entry_scene` | string | 入口场景 `res:/` 路径（默认约定 `res:/scenes/main.gesc`） |
| `physics.backend_2d` | string | 2D 物理后端（`box2d`） |
| `physics.backend_3d` | string | 3D 物理后端（`jolt`） |
| `window.width/height/title` | int/string | 默认窗口尺寸与标题 |

#### 2.1.2 运行时设置字段

| 字段 | 类型 | 说明 |
|---|---|---|
| `render_api` | string | `opengl` / `vulkan` |
| `scene_2d` | bool | 纯 2D 画布路径开关（模板 `project_is_2d` 读取） |
| `hdr` / `tone_map_mode` / `exposure` | bool/int/float | 后处理参数 |
| `shadow_enabled` / `shadow_map_size` | bool/int | 阴影开关与级联贴图尺寸 |
| `ambient_r/g/b` / `ibl_intensity` | float | 环境光 / IBL 强度 |
| `main_scene` | string | 主场景 `res:/` 路径；缺省 `res:/scenes/main.gesc` |

> Core 当前只消费 `main_scene` 字段（其余渲染字段由编辑器在启动/保存时维护）。

#### 2.1.3 打包元数据字段（发布后由 GryceGC 追加）

| 字段 | 类型 | 说明 |
|---|---|---|
| `format` | string | `"gryce_project_data"` |
| `version` | int | 元数据版本 `1` |
| `project` | string | 输出游戏名 |
| `author` | string | 作者（默认 `%USERNAME%`，`--author` 覆盖） |
| `created` | string | 打包时间（`YYYY-MM-DDTHH:MM:SS`） |
| `tool` | string | `"grycegc"` |
| `key_sha512_hex` | string | 由源记录派生的 64 字节 SHA-512（hex 128 字符），用于完整性校验 |
| `enc_key_hex` | string | **`.gpkg` 解密密钥**（32 字节 ChaCha20 密钥，hex 64 字符），运行时据此解密 |
| `sources` | array | 每个被打包文件：`{ path, sha256, size }` |

开发期示例（`<your-project-dir>/project.data`）：

```json
{
  "name": "2dDemo",
  "version": "0.1.0",
  "engine_version": ">=0.1.0",
  "entry_scene": "res:/scenes/main.gesc",
  "physics": { "backend_2d": "box2d" },
  "window": { "width": 1280, "height": 720, "title": "Gryce Engine - 2D Platformer Demo" },
  "render_api": "opengl",
  "hdr": true,
  "main_scene": "res:/scenes/main.gesc"
}
```

打包后同一文件追加元数据字段（节选）：

```json
{
  "name": "2dDemo",
  "render_api": "opengl",
  "main_scene": "res:/scenes/main.gesc",
  "format": "gryce_project_data",
  "version": 1,
  "author": "Your Name",
  "created": "2026-09-06T18:29:37",
  "tool": "grycegc",
  "key_sha512_hex": "<128 hex chars>",
  "enc_key_hex": "<64 hex chars>",
  "sources": [
    { "path": "scenes/main.gesc", "sha256": "<64 hex chars>", "size": 12345 }
  ]
}
```

### 2.2 `res:/` 虚拟路径

- `res:/path/to/file` 解析为 `<project_root>/path/to/file`；
- 打包后没有真实文件时，同一路径会从挂载的 `.gpkg` 中提取（见 §5.3）；
- 统一 UTF-8，支持中文文件名。

### 2.3 资源收集（→ 每个文件一个 `.gpkg`）

打包时 **不再按扩展名分组**，而是**每个文件单独打包**成一个 `.gpkg`。收集规则（
`collect_project_files`，递归项目根）：排除以下内容后，其余**每个文件**各自生成一个
加密资源包。

- 排除目录：`.git` `.vs` `.idea` `__pycache__` `build` `bin` `obj` `x64` `out`；
- 排除扩展名：`.cpp` `.cc` `.cxx` `.c` `.h` `.hpp` `.hh` `.inl` `.py` `.md` `.sln`
  `.pdb` `.ilk` `.exp` `.lib` `.dll` `.exe`；
- 排除文件名（不区分大小写）：`cmakelists.txt` `.gitignore` `.gitattributes`
  `license` `readme.md`。

因此 `scenes/ scripts/ shaders/ models/ textures/ audio/ fonts/ config/ misc` 等各类
资源都会被打包，块名（`assets/*.gpkg`）是**随机 Base64**（32 字节随机数，43 字符，无
逻辑含义）；文件名到包内路径的映射由包内 manifest 完成（见 §4 / GPAK v4）。

---

## 3. 打包（GryceGC）

### 3.1 命令行

```text
GryceGC --init <dir> [--name <name>]    创建标准 GryceGC-A 项目骨架
GryceGC --project <dir> [选项]
  --project <dir>   游戏项目目录（res:// 根）【必填】
  --name <name>     输出游戏名（默认 MyGame；显式传入时保持原样）
  --build-dir <dir> CMake 构建目录（默认 build；用于定位 GryceGame.exe 与 bin/）
  --config <cfg>    Debug 或 Release（默认 Release）
  --out <dir>       输出父目录（默认 build/game）
  --game <exe>      GryceGame 模板 exe（默认用编译期内嵌路径，见下方"模板依赖"）
  --author <name>   project.data 中的作者（默认 %USERNAME%）
```

**模板依赖（GryceGC → GryceGame）**：GryceGC 打包时需把 `GryceGame` 模板 exe
拷贝进发布目录，二者在 CMake 层强耦合——`../tools/GryceGC` 声明
`add_dependencies(GryceGC GryceGame)` 保证 GryceGame 先于 GryceGC 构建，并把其
精确输出路径（`bin/<Release>/GryceGame.exe`）在配置期编译进工具
（`GRYCE_GC_GAME_TEMPLATE`）。工具运行时优先取 `--game` 显式指定的路径，其次
使用内嵌路径；仅当二者都缺（独立分发、脱离构建树的 GryceGC）才回退到
`dist/GryceGame/GryceGame.exe` 与 flat `bin/<cfg>/GryceGame.exe` 目录猜测。

**创建标准 GryceGC-A 项目（`--init`）**：在指定目录生成完整的标准骨架
（`project.gproj` 唯一配置文件、`scenes/main.gesc` 空主场景，以及
`scenes/ scripts/ shaders/ models/ textures/ audio/ fonts/ tilesets/` 分类目录），
项目名默认取目录名（自动归一化为 PascalCase，如 `my-game` → `MyGame`），也可用
`--name` / `--window-title` 覆盖。创建后即可直接打包：

```bat
build/bin/Release/GryceGC.exe --init my-game
build/bin/Release/GryceGC.exe --project my-game --name my-game ^
    --build-dir build --config Release --out build/game
```

前置条件：先构建 `GryceGame`（模板入口）与 `GryceGC` 目标：

```bat
cmake --build build --target GryceGame GryceGC --config Release
```

打包示例（2dDemo）：

```bat
build/bin/Release/GryceGC.exe --project <your-project-dir> --name MyGame ^
    --build-dir build --config Release --out build/game --author "Your Name"
```

编辑器菜单「文件 → 打包运行（GryceGC）」会自动定位/构建 `GryceGC.exe` 并执行打包。

### 3.2 输出布局

```text
<out>/<name>/
├── <name>.exe              # GryceGame 模板入口
├── GryceCore[d].dll 等       # 运行时 DLL 的加载期闭包镜像（见下注）
├── runtime/                # 核心运行时 DLL + GLFW + 编译器运行时的规范化存放
│   ├── GryceCore.dll（Debug 构建为 GryceCored.dll）
│   ├── GryceRenderer.dll（Debug 为 GryceRendererd.dll）
│   ├── GrycePlatform.dll（Debug 为 GrycePlatformd.dll）
│   ├── GrycePhysics.dll（Debug 为 GrycePhysicsd.dll）
│   ├── glfw3.dll（Debug 为 glfw3d.dll）
│   └── MSVC/MinGW/GCC 编译器运行时（vcruntime/msvcp 或 libgcc/libstdc++/libwinpthread）
├── assets/
│   └── <random-base64>.gpkg   # 每个资源一个加密 .gpkg（GPAK v4，随机 Base64 名）
├── project.data             # 唯一状态文件（JSON）：清单 + 运行时设置 + 打包元数据
└── （无独立的 gdata 文件）
```

> **根目录运行时闭包**：`runtime/` 是引擎 DLL/GLFW/CRT 的**规范化存放地**。但进程加载
> 优先级是「exe 同目录 > runtime/」：
> - **MSVC**：exe 对核心 DLL 用 `/DELAYLOAD`，`main()` 先加 `runtime/` 到搜索路径，无需
>   根目录副本；仅 **CRT**（启动期静态导入）会额外平铺一份到根目录；
> - **MinGW**：无 `/DELAYLOAD`，exe 与各引擎 DLL 的静态导入在**进程启动期**就需解析，
>   `main()` 的 `SetDllDirectoryW` 尚未执行，故 `grycegc` 将 `runtime/` 的**完整闭包
>   （引擎 DLL + glfw/glew + libgcc/libstdc++/libwinpthread）镜像到 exe 根目录**，保证
>   开机可解析。根目录与 `runtime/` 的重复副本无害，模板的延迟加载钩子仍会命中
>   `runtime/`。
>
> 打包前会清空旧输出目录（`<out>/<name>`），避免残留文件混入新包。

### 3.3 元数据（合并于 `project.gproj`）

原独立的 `gdata` 文件已废弃，其内容**合并进根目录的 `project.gproj`**（见 §2.1.3）。
GryceGC 打包后，`project.gproj` 追加：

- `sources`：每个被打包文件的 `path`（相对项目根，正斜杠）+ SHA-256 + 字节数；
- `key_sha512_hex`：对排序后的 `path:size:sha256` 记录做 SHA-512 得到的 **64 字节密钥**
  （hex 编码 128 字符），可用于校验包内容是否被改动；
- `enc_key_hex`： **.gpkg 的 ChaCha20 解密密钥**（32 字节，hex 64 字符）——运行时挂载
  包前从 `project.gproj` 读出并解密每个加密 `.gpkg`；
- `author` 默认取 `%USERNAME%`，可用 `--author` 覆盖。

---

## 4. GPAK v4 资源包格式（GryceCore `pak_bundle`）

打包与运行时共用 GryceCore 的 `PakWriter` / `PakReader`（`src/resources/pak_bundle.cpp`），
因此磁盘格式始终与读取端一致。格式 GPAK v4：

```text
Header:
  magic[4] = "GPAK"
  uint32 version = 4
  uint32 entry_count
  uint64 manifest_offset
  uint64 manifest_size
  uint8  flags            // bit0=1：data 区已 ChaCha20 加密
  uint8  nonce[12]        // 加密时随机生成的 nonce
Entry (entry_count 次):
  uint32 path_len
  char   path[path_len]            // 随机 Base64 名称（无逻辑含义）
  uint64 data_size
  uint64 data_offset               // 文件起始偏移
  uint32 crc32                     // 明文数据的 CRC32
Data: 各 entry 的（密文）数据依次存放，流偏移与文件内 data_offset 对齐
Manifest:
  uint32 entry_count
  ManifestEntry (entry_count 次):
    uint32 orig_path_len / char orig_path[...]     // 原始路径，如 "models/cube.obj"
    uint32 random_path_len / char random_path[...] // 随机 Base64 名称
```

- 每个资源文件单独一个 `.gpkg`，包内 manifest 把**原始路径**（如 `models/cube.obj`）
  映射到**随机 Base64 存储名**；
- 设置 32 字节密钥后写出 GPAK **v4**（data 区 ChaCha20 加密）；未设置密钥时写出
  不加密的 **v3**；
- `PakReader` 兼容 v1 / v3 / v4；GryceGC 打包后读回逐文件解密比对源字节，校验
  加解密环回一致。

---

## 5. 游戏入口与运行时行为

### 5.1 游戏入口（GryceSPC 模板）

`templates/GameTemplates.cpp`（`GryceGame.exe`）是发布游戏的唯一入口，职责：

1. **扩展 DLL 搜索路径**：`main()` 先把 `<exe>/runtime/` 加入 DLL 搜索目录，再首次调用
   引擎 API。MSVC 构建的核心 DLL 为 `/DELAYLOAD` 延迟加载，靠此路径解析；
   **MinGW 构建则无需此设置**——`grycegc` 已把运行时闭包镜像到 exe 根目录，进程启动期
   静态导入即可解析；
2. **优先使用系统 VC++ 运行时**：从 System32 显式预加载 CRT；系统缺失时才回退到
   `runtime/` 中随包携带的运行时；MinGW 的 CRT（libgcc/libstdc++/libwinpthread）由
   `grycegc` 复制，启动期在根目录解析；
3. **确定项目根**：默认取 exe 所在目录（支持双击运行）；`--project <dir>` 覆盖；
4. **初始化引擎**：`GCore_Init` → 读取 `project.gproj`（解密密钥 + `main_scene`）→
   物理（Jolt 3D + Box2D 2D 同时挂载）→ 窗口 → 渲染器（OpenGL）→ 进入 Play Mode；
5. **2D 判定**：`project.gproj` 的 `scene_2d` 为真时渲染器走纯 2D 画布路径；
6. **自动进入主场景**：`GCore_SetAutoLoadMainScene(true)`，启动即加载
   `project.gproj` 的 `main_scene`。

运行参数：

```text
<name>.exe                          # 默认：项目根 = exe 目录，加载主场景
<name>.exe --scene res:/scenes/x.gesc   # 覆盖主场景
<name>.exe --project <dir>          # 覆盖项目根
<name>.exe --w 1920 --h 1080        # 覆盖窗口尺寸
```

### 5.2 资源包自动挂载与解密

1. `GCore_Init` 扫描项目根**和 `<root>/assets/`** 下所有 `.gpkg/.gpack` 文件；
2. 挂载前先从根目录 `project.gproj` 读取 `enc_key_hex` 并解码为 32 字节密钥；
3. 每个 `.gpkg` 用**包内 nonce + 项目密钥** 解密 data 区后交给 `AssetManager`，
   旧布局（包直接放项目根）仍然兼容。

### 5.3 `res:/` 解析优先级

统一入口 `AssetManager::resolve_for_reading`：

1. 项目根下的真实文件优先；
2. 文件不存在时，从已挂载的包中按 manifest 内部路径（`scenes/main.gesc`）读取并
   解密，提取到临时目录返回；
3. 都找不到时返回空，调用方记录警告。

> 场景（`.gesc`）、JS 脚本（`.js`）、着色器、贴图、tileset JSON、模型等一律走该入口；
> 任何直接以 `ResourcePath::resolve` + 文件流打开的资源都不会在打包产物中生效。

### 5.4 主场景

- 主场景路径来自 `project.gproj` 的 `main_scene`，缺省 `res:/scenes/main.gesc`；
- 模板启动时 `GCore_SetAutoLoadMainScene(true)`；编辑器不启用（由编辑器自行管理场景）；
- `--scene` 覆盖时模板跳过自动加载，改为显式加载指定场景；
- 运行中切场景用 JS：`engine.scene.load("res:/scenes/xxx.gesc")`。

### 5.5 物理

`GPhysics_Init` 总是同时创建 3D（Jolt）与 2D（Box2D）世界，
`GPhysics_AttachSystems` 同时注册 `PhysicsSystem3D` 与 `PhysicsSystem2D`，
因此打包的 2D/3D 游戏都能在 Play Mode 下真实模拟。

---

## 6. 符合性检查清单

项目要符合最新的 GryceGC-A 标准，需满足：

- [ ] 目录包含唯一配置文件 `project.gproj`（`name` 与目录名一致，且含 `main_scene`）；
- [ ] `scenes/` 下有 `main.gesc`（或 `main_scene` 指向的实际场景文件）；
- [ ] 资源按类别目录组织，源文件（`.cpp/.h/...`）与构建产物不会混入资源目录；
- [ ] `GryceGC --project <dir> --name <name> --build-dir build --config <cfg> --out build/game`
      打包成功（退出码 0）；
- [ ] 产物 `build/game/<name>/<name>.exe` 双击可启动，日志出现
      `main scene loaded`，且无 `failed to open / resolve` 类错误；
- [ ] 着色器、贴图、脚本等所有运行时资源都能从 `assets/*.gpkg` 解析（不依赖源目录），
      `assets/` 下为随机 Base64 命名的加密包，根目录无独立 `gdata` 文件。

---

## 7. 仓库中的 GryceGC-A 项目

- **`examples/parkourDemo/`**：3D 跑酷示例， **规范的纯资源项目**（GryceGC-A 标准）——
  不含宿主 `main.cpp`，仅含项目文件：唯一配置 `project.gproj` 与按类别
  分目录的资源（静态场景 `scenes/main.gesc` + 玩法脚本 `game.js` 等）。它不是 CMake 目标，
  而是通过 `GryceGC --project examples/parkourDemo --name ParkourRun` 打包为独立运行的游戏
  目录（`build/game/ParkourRun/`），由 GryceGame 固定运行时加载并运行。
- 新建项目用 `GryceGC --init <dir>`（见 §3.1），即得完整标准骨架。

---

> 相关文档：[脚本 API 参考](./SCRIPT_API_REFERENCE.md)（QuickJS）、
> [ECS 脚本指南](./ECS_SCRIPT_GUIDE.md)、[.uif DSL 规范](./UI_DSL_SPEC.md)、
> [迁移指南](./MIGRATION_GUIDE.md)、[已实现功能](./已实现功能.md)（功能状态）、
> [架构说明](./架构说明.md)（模块划分）。