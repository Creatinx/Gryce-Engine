# 脚本运行时与游戏打包工具链设计方案（GryceSRT / GryceGC / GryceSPC）

> 状态：规划中（未开始实现）。
> 本文描述 GryceEngine 的两条产品线：
> ① **脚本支持** —— 运行时层 GryceSRT（Core 内嵌 Lua，Editor 只经 C API 命令缓冲通信）；
> ② **游戏打包运行** —— 工具链 GryceGC + 模板可执行文件，整套工具链统称 GryceSPC。

## 1. 术语与目标

| 名称 | 全称 | 定位 |
|---|---|---|
| **GryceSRT** | GryceEngine Script Runtime | 脚本运行时层：实体挂 `.lua` 脚本，播放模式下驱动逻辑。目的不变（见 §4）。 |
| **GryceGC** | GryceEngine Global Compiler | 打包器：把整个游戏（Core 产物 + 脚本 + 资源 + 模板生成的入口可执行文件）编译/组装成可发布的游戏目录并运行。 |
| **GryceSPC** | GryceEngine Platform Script Compiler | 整套工具链的统称：GryceGC 打包 + GryceSRT 脚本运行时 + 平台/渲染模板入口。 |

### 目标
- 实体可以挂一个 `Script` 组件，绑定项目内的 `.lua` 文件；播放模式下脚本驱动实体逻辑（每帧 `on_update`）。
- 编辑器支持：项目面板浏览/打开 `.lua`、添加 Script 组件、Inspector 查看脚本路径与暴露变量、脚本编辑窗口、脚本日志/错误进入控制台、保存后热重载。
- **一键打包**：通过 GryceGC 把游戏打包成一个可独立运行的目录/包 —— 内含模板生成的入口可执行文件、链接好的 Core 产物、`res://` 资源与脚本；双击即运行。
- 保持既有架构约束：**Core 与 Editor 完全分离**。脚本在 Core 内部（游戏线程）执行；Editor 只通过 `GryceCore.dll` 的 C API + 命令缓冲控制。打包后的游戏可执行文件与 Editor 无关。

### 非目标（首轮不做）
- 脚本调试器（断点/单步）、跨语言热重载的复杂语义、多脚本语言、可视化蓝图/节点脚本、网络/多人。
- 多租户沙箱隔离（首轮信任脚本，运行在引擎进程内；文档注明风险）。
- 移动端/Web 发布、代码签名/商店分发、增量热更新补丁包。

## 2. 技术选型

| 方案 | 结论 |
|---|---|
| **Lua 5.4 + 纯 C API** | **采用**（GryceSRT 的脚本语言）。稳定、可嵌入、开销小、MSVC/GCC 均无 ABI 问题；手写 `gryce.*` 绑定表简单可控。 |
| Lua + sol2（头文件模板库） | 备选。省绑定代码，但引入 C++ 模板层，出错信息复杂，与现有"零/少依赖"风格不一致。 |
| Python 嵌入（CPython） | 否决。体积大、GIL/初始化重、发行麻烦。 |
| JavaScript（QuickJS/Duktape） | 可作备选；生态/心智模型不如 Lua 通用。 |
| C#（Editor 同语言） | 否决。需在 C++ Core 内嵌 .NET 运行时，与"Core 独立"架构冲突。 |

Lua 源码 vendor 到 `third_party/lua`（仅核心库 + 裁剪后的标准库），遵循现有 `third_party/nlohmann_json`、`stb` 的模式，接入 CMake。

## 3. 总体架构

```mermaid
flowchart LR
    subgraph Editor["Editor (WPF, .NET 4.8)"]
        PV["Project 面板 .lua 资源"]
        SE["脚本编辑器窗口"]
        IN["Inspector Script 组件"]
        CO["Console 日志/错误"]
        PK["打包按钮 → GryceGC"]
    end
    subgraph Core["Core (C++ DLL / 静态库)"]
        SRT["GryceSRT: Lua 运行时 + ScriptSystem"]
        SC["Script 组件"]
        API["gryce.* 绑定表"]
        ECS["World / Scene / Reflection"]
        PLT["Platform: 窗口创建"]
        REN["Renderer: 渲染上下文/后端"]
    end
    subgraph Game["打包产物（GryceGC 输出）"]
        EXE["模板入口可执行文件（main 模板）"]
        RES["res:// 资源 + .lua + .gesc"]
        CORE["Core 产物（链接）"]
    end
    Editor -- C API 命令缓冲 --> Core
    Core -- 回调（日志/实体列表/场景） --> Editor
    EXE --> PLT
    EXE --> REN
    EXE --> SRT
    CORE --> EXE
    RES --> EXE
    PK -- 调用 --> EXE
```

- **执行位置**：脚本只在打包后的游戏帧内运行（Editor 模式不跑）。Core 现有
  `GCore_BeginFrame` 中已有 `if (play_mode && !paused) world->update(dt);`，
  GryceSRT 的 ScriptSystem 作为 `ISystem` 挂进 World，天然满足"编辑器不跑、播放/打包后运行才跑"。
- **通信**：Editor → Core 全部走命令缓冲；Core → Editor 走既有回调
  （`GOnLogMessage` 用于脚本 print/错误，`GOnEntityListChanged` 用于运行时创建实体的刷新）。
- **发布**：打包后的游戏目录与 Editor 无关，由模板入口 exe 直接驱动 Core（§6）。

## 4. GryceSRT —— Core 侧脚本运行时设计

### 4.1 Lua 运行时
- 新增 `core/script/` 目录（GryceSRT 的 Core 部分）：
  - `lua_runtime.h/.cpp`：全局 `lua_State*` 的生命周期（引擎初始化时创建、关闭时销毁），标准库加载裁剪。
  - `lua_bindings.h/.cpp`：`gryce.*` API 表。
  - `script_component.h`：`Script` 组件。
  - `script_system.h/.cpp`：`ScriptSystem`（ISystem）。
- 全局一个 `lua_State`；**每个 Script 组件持有一个独立环境表**（`lua_newtable` + 设置元表指向全局），
  避免脚本间全局变量互相污染；脚本可显式访问 `_G` 通信（首轮允许）。

### 4.2 Script 组件
- 经 `ComponentFactory` 注册 `"Script"`（与既有组件一致）：
  - 反射字段（走 `core/reflection/reflection.h` 的 `GRYCE_REFLECT_*`）：
    - `script_path`（string，资源路径，如 `res://scripts/rotate.lua`）
    - `enabled`（继承自基类 `Component`）
  - 运行时字段（不序列化）：`lua_ref env_ref`、已加载 chunk 缓存、错误信息。
- `Add Component` 选择树新增分类 **Script**（图标、颜色、描述走现有 `ComponentCatalog`）。

### 4.3 ScriptSystem
- `priority` 放在动画系统之后、渲染之前；`on_init` 预加载所有 Script 组件；`on_update(scene, dt)` 逐个调用。
- 生命周期约定（Lua 侧约定函数，缺省为空）：
  ```lua
  function on_start() end          -- 实体创建/组件挂载、或播放开始时
  function on_update(dt) end       -- 每帧（play 模式/打包运行时）
  function on_destroy() end        -- 实体销毁/组件移除时
  ```
- 错误处理：所有调用包 `pcall`；错误 → `GLOG_ERROR` + `GOnLogMessage` 回调进编辑器控制台，
  组件标记 `error_state`（每帧只报一次，避免刷屏）。
- 场景切换/实体销毁时清理：按实体句柄缓存 env，销毁时 `luaL_unref`。

### 4.4 Lua API（`gryce.*`）

首轮最小集合：

| 分组 | API | 说明 |
|---|---|---|
| 实体 | `gryce.self()` | 当前脚本所属实体句柄 |
| | `gryce.entity.get_name(h)` / `find(name)` | 场景查询 |
| | `gryce.entity.get_transform(h)` / `set_transform(h, pos, rot, scl)` | 变换读写（复用 `EntityAPI` 逻辑） |
| | `gryce.entity.create(name, parent)` / `destroy(h)` | 运行时实例化（可选首轮） |
| 组件 | `gryce.component.get(h, type_name, prop)` / `set(...)` | 走反射 `Registry::all_fields` 读写任意属性 |
| 输入 | `gryce.input.key_down(key)` / `mouse_pos()` | 复用 Core 已保存的输入状态（Editor 已推 `ECMD_INPUT_*`） |
| 时间 | `gryce.time.delta()` / `elapsed()` | |
| 日志 | `gryce.log.info/warn/error(...)` | 进控制台 |
| 场景 | `gryce.scene.load(path)` | 可选 |

绑定实现为普通 C 函数（`lua_CFunction`），内部调用 Core 已有模块，不做重实现。

### 4.5 命令缓冲扩展
- 新增 `ECMD_SET_SCRIPT`（payload：实体句柄 + script_path），用于 Editor 在 Inspector 中修改脚本路径；
  以及 `ECMD_RELOAD_SCRIPTS`（播放中保存后热重载）。
- Editor 侧 `EditorViewModel` 增加对应包装方法（与 `RenameEntity`/`SetTransform` 等一致：编码 payload → `GCore_PushCommand`）。
- 也可首轮直接复用 `ECMD_SET_PROPERTY`（`Script.script_path` 是反射字段），显式命令作为 Phase 1 收尾的清理项。

### 4.6 暴露属性（Inspector 显示脚本变量）
- 约定：脚本顶部定义 `gryce.props()` 返回表，或约定全局表 `props = { speed = 1 }`。
- Core 加载脚本后扫描 props 表，动态注册为 Script 组件的临时反射字段（前缀 `script.`），
  Inspector 复用现有属性 UI 读写，写回 Lua 环境；序列化时按反射字段落盘 `.gesc`。
- **阶段化**：Phase 1 只序列化 `script_path` + `enabled`；暴露属性放 Phase 3。

## 5. Editor 侧设计

### 5.1 项目面板
- `.lua` 文件图标（Fluent 图标 + 颜色，走现有 `FileItem` 图标映射）；双击打开脚本编辑器。

### 5.2 脚本编辑器（新增窗口/停靠面板）
- 文本编辑：等宽字体、行号、Ctrl+S 保存；基础 Lua 语法高亮（关键字/字符串/注释，用现有 WPF 控件实现，不引第三方编辑器）。
- 保存后向 Core 推 `ECMD_RELOAD_SCRIPTS`（编辑器中可"播放并验证"）。

### 5.3 Add Component
- 选择树新增 "Script" 分类；选择后实体名按现有"创建实体→进入新建组件"流程处理（实体改名 `Script`）。

### 5.4 Inspector
- Script 组件显示 `script_path`（文本框 + 从项目面板选择）、`enabled`、以及暴露属性（Phase 3）。

### 5.5 控制台
- 脚本 `print`/`error` 经 `GOnLogMessage` 进入现有控制台（级别颜色沿用）。

### 5.6 播放与热重载
- 播放模式进入时 GryceSRT 加载脚本；编辑器内保存脚本 → 自动重载（或工具栏"重载脚本"按钮）。

## 6. 打包与运行工具链（GryceGC / GryceSPC）

### 6.1 现状：Core 链接 + 可执行文件调用
- 当前逻辑：把 Core 编译产物链接进游戏编译后的目录；游戏可执行文件直接调用 Core。
- 可执行文件 = **模板入口**（main 模板）：初始化日志 → 调用 Core 的 **Platform** 创建窗口 →
  继续调用 **Renderer**（渲染后端/上下文、视口）→ 进入游戏循环（每帧 update + render）。
- 现有参考实现：`examples/common/app_launcher.cpp`（`run_demo`：`glog_initialize` →
  `platform::Window::init_sdk()` → 创建窗口 → `create_render_backend` + `RenderContext::init` →
  viewport → 游戏循环），以及 `core/GrycePlatform/window_api.h` 的 `GWindow_Create` 系列。

### 6.2 模板可执行文件（发行入口）
- 新增 `templates/game_main.cpp`（从 `app_launcher.cpp` 抽取/精简），固定流程：
  1. 解析启动参数（游戏项目目录、`--opengl/--vulkan`、全屏/窗口大小等）；
  2. 定位 `res://`（相对可执行文件目录的 `res` 文件夹）；
  3. `GWindow_Create`（或 `GWindow_InitExternal`）创建窗口；
  4. `GRender_Init` + 每帧 `GRender_BeginFrame → RenderWorld → EndFrame`；
  5. 游戏循环内 `GCore_BeginFrame`（处理命令 + `world->update(dt)`，其中运行 GryceSRT 脚本）；
  6. 退出清理（`GRender_Shutdown`、`GWindow_Destroy`）。
- 模板不包含 Editor 逻辑；GryceSRT 以 Core 库的形式随游戏一起链接/分发。

### 6.3 GryceGC —— GryceEngine Global Compiler（打包器）
- 输入：
  - Core 产物（`GryceCore`/`GryceRenderer`/`GrycePlatform`/`GrycePhysics`，按发行配置取 Release）；
  - 模板生成的入口源码 → 编译出游戏可执行文件；
  - 游戏内容：`project.gryce`、`res://` 场景（.gesc）、脚本（.lua）、纹理/模型/音频；
  - 打包配置（目标目录、是否包含调试符号、渲染后端等）。
- 输出：一个可独立运行的**游戏目录/包**：
  ```
  build/game/MyGame/
    MyGame.exe              ← 模板入口（链接 Core）
    GryceCore.dll / GryceRenderer.dll / GrycePlatform.dll / GrycePhysics.dll
    res/
      project.gryce
      scenes/*.gesc
      scripts/*.lua         ← GryceSRT 运行时加载
      textures/ models/ audio/
  ```
- 职责边界：GryceGC 只做"编译 + 组装 + 拷贝"，不执行游戏逻辑；与 Editor 解耦，
  可由命令行（`grycegc --project . --out build/game/MyGame`）或 Editor 菜单"打包"按钮调用。

### 6.4 GryceSPC —— GryceEngine Platform Script Compiler（工具链统称）
- GryceSPC = **GryceGC 打包器 + GryceSRT 脚本运行时 + 平台/渲染模板入口** 的完整工具链。
- 工具链命令（规划）：
  - `grycegc`：打包（§6.3）；
  - `grycegc --run`：打包后直接运行产物；
  - `grycegc --dev`：只链接、不打包（输出到开发目录，配合 Editor 播放）。
- 与 Editor 的关系：Editor 的"播放"走 Core 内嵌窗口（现有 `GWindow_InitExternal` 路径）；
  "打包/运行"走 GryceGC + 模板入口。两者共用同一份 Core 与 GryceSRT，只是入口不同。

## 7. 实施阶段

### Phase 0 — Spike（验证嵌入可行性）
- vendor Lua 5.4 源码 + CMake 接入；引擎初始化/关闭时创建/销毁 `lua_State`。
- 冒烟：加载 `res://scripts/hello.lua`，执行 `gryce.log.info("hi")`，编辑器控制台可见。
- 验收：`examples/` 下新增 hello 脚本示例；控制台正确显示日志。

### Phase 1 — Core 基础（GryceSRT 主体）
- Script 组件 + ScriptSystem + 生命周期（on_start/on_update/on_destroy）+ pcall 错误上报。
- `gryce.self / entity 变换 / 时间 / 日志 / 输入查询` 绑定。
- 序列化（.gesc 保存 script_path/enabled）与场景加载恢复。
- 验收：给 Cube 挂脚本每帧自转；保存/重载场景后脚本仍工作；脚本抛错在控制台显示且不崩。

### Phase 2 — Editor 基础
- AddComponent 增加 Script；Inspector 显示 script_path（可改）；项目面板 .lua 图标；脚本日志进控制台。
- 新增 `ECMD_SET_SCRIPT` / `ECMD_RELOAD_SCRIPTS` 及 Editor 包装。
- 验收：编辑器内改脚本路径生效；播放时脚本运行。

### Phase 3 — 脚本编辑器 + 暴露属性
- 脚本编辑窗口（行号/高亮/保存/重载）；props 反射进 Inspector 并可编辑。
- 验收：编辑脚本保存后热重载；Inspector 改脚本变量即时生效并随场景保存。

### Phase 4 — 打包工具链（GryceGC / GryceSPC）
- 抽取 `templates/game_main.cpp`（§6.2），打通"模板 → 链接 Core → 运行"的发行入口。
- 实现 `grycegc`：命令行解析、项目根/`res://` 定位、Release 产物拷贝、入口编译、输出游戏目录。
- Editor "打包"菜单调用 GryceGC；`grycegc --run` 冒烟运行示例游戏（含脚本）。
- 验收：一个含脚本的场景能从 `build/game/MyGame/MyGame.exe` 独立启动、播放脚本逻辑、正常退出；
  目标机器无 Engine 源码/Editor 依赖。

### Phase 5 — 打磨
- 热重载健壮性（播放中重载保留实体状态）、示例脚本集（旋转/移动/点击销毁/计时器）、
  性能（同脚本多实体共享 chunk）、文档（脚本 API 手册 + 打包手册）。

## 8. 风险与权衡

- **脚本状态与场景生命周期**：实体销毁/场景切换时须清理 Lua 引用，防止悬垂句柄/泄漏 —— ScriptSystem 按实体句柄管理 env，销毁回调兜底。
- **线程模型**：Lua 只在 Core 游戏帧（`GCore_BeginFrame` 内 `world->update`）执行；Editor UI 线程不触碰 Lua。约定所有绑定函数只允许在游戏线程调用（文档注明，首轮不加锁）。
- **性能**：每组件独立 env 表 + `pcall` 开销可控；大量实体复用同一脚本时共享编译产物（`luaL_loadbuffer` 缓存），只复制闭包。
- **依赖与构建**：Lua 是唯一新增第三方，纯 C、MSVC/GCC 通用；接入现有 CMake（参考 nlohmann_json/stb）。
- **安全**：首轮信任脚本（与引擎同进程、同权限），不做沙箱；文档明确风险，后续可加白名单 API 或子进程。
- **打包发布**：
  - 链接方式：发行默认动态链接（DLL 与 exe 同目录），避免静态链接的 ABI/许可复杂性；后续可按需支持静态。
  - `res://` 定位：以可执行文件目录为基准，避免依赖绝对路径/环境变量（参考 `find_project_root` 的实现，改为发行版相对路径）。
  - 发布裁剪：只拷贝被引用资源（首轮可全量拷贝，Phase 5 做引用扫描）。
- **ABI**：纯 C API 绑定，无 C++ ABI 跨编译器问题（Editor 侧仅经 `GryceCore.dll` C 接口）。

## 9. 涉及现有代码位置

- 组件注册：`core/components/component_factory.cpp`
- 反射字段：`core/reflection/builtin_reflections.cpp`、`core/reflection/reflection.h`
- 命令缓冲：`core/api/core_api.cpp`（`process_command`、`GCore_BeginFrame`）、`core/GryceCore/types.h`（`ECMD_*`）
- 世界/系统：`core/ecs/world.cpp`、`core/ecs/system.h`、`core/scene/scene.cpp`
- 序列化：`core/scene/`（.gesc JSON）
- 平台/窗口：`core/GrycePlatform/window_api.h`、`core/api/platform_api.cpp`
- 渲染入口：`core/api/render_api.cpp`、`core/GryceRenderer/render_api.h`
- 模板参考：`examples/common/app_launcher.cpp`、`examples/common/app_launcher.h`
- C API 头：`core/GryceCore/*.h`、`core/api/*.cpp`
- Editor：`editor/src/Services/EngineService.cs`、`editor/src/ViewModels/EditorViewModel.cs`、
  `editor/src/Views/{ProjectView,InspectorView,AddComponentDialog,ConsoleView,MainWindow}.xaml(.cs)`
