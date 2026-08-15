# GryceSRT 脚本 API 与游戏打包手册

> GryceSRT = GryceEngine Script Runtime（Core 内嵌 Lua 5.4）。
> 脚本通过 `Script` 组件挂到实体上，播放/打包运行时由 ScriptSystem 驱动。
> 玩法逻辑（角色控制、AI、关卡流程等）可以完全用 Lua 编写；
> `examples/2dDemo` 就是一套完整的 Lua 驱动的平台跳跃游戏。

## 1. 脚本生命周期

在 `.lua` 中定义以下可选函数：

```lua
props = { speed = 1.0, label = "hello" }  -- 暴露属性（Inspector 可编辑、随场景保存）

function on_start() end          -- 实体创建/组件挂载、或播放开始时调用一次
function on_update(dt) end       -- 每帧调用，dt 为帧时间（秒）
function on_destroy() end        -- 组件移除/场景关闭/重载前调用
```

脚本顶层代码在加载时执行一次（相当于 `require`）。每个实体拥有独立环境，全局变量互不污染。

## 2. `engine.*` API

| 分组 | 函数 | 说明 |
|---|---|---|
| 运行时 | `engine.version()` | 返回运行时版本字符串 |
| 实体 | `engine.self()` | 当前脚本所属实体句柄（0 表示无） |
| | `engine.entity.get_name(h)` | 实体名 |
| | `engine.entity.find(name)` | 按名字查找实体，返回句柄（0 = 未找到） |
| | `engine.entity.find_all(prefix)` | 查找名字为 `prefix` 或 `prefix<数字>` 的全部实体，返回句柄数组 |
| | `engine.entity.create(name)` | 在当前场景创建实体，返回句柄（脚本遍历期间安全） |
| | `engine.entity.destroy(h)` | 延迟销毁实体（本帧脚本遍历结束后生效），返回 bool |
| | `engine.entity.aabb(h)` | 实体 AABB `{x, y, w, h}`（中心+尺寸；优先取碰撞盒） |
| | `engine.entity.get_transform(h)` | 返回 `{pos={x,y,z}, rot={x,y,z,w}, scale={x,y,z}}` |
| | `engine.entity.set_transform(h, pos, rot, scale)` | 写回变换（三个表均可省略） |
| 组件 | `engine.component.has(h, type)` | 实体是否有该类型组件 |
| | `engine.component.get(h, type, prop)` | 读取组件字段（number/string/bool/`{x,y}`/`{x,y,z,w}`/`{r,g,b,a}`/int） |
| | `engine.component.set(h, type, prop, value)` | 写组件字段；**组件不存在时自动创建**（含 Script/Sprite2D/RigidBody2D 等所有注册类型） |
| 状态 | `engine.state.get/set/has(key)` | 跨实体、跨场景共享的游戏状态表（任意 Lua 值） |
| 输入 | `engine.input.key_down(key)` | 按键是否按住（GLFW 键码，如 W=87） |
| | `engine.input.mouse_pos()` | 返回鼠标 x, y |
| | `engine.input.mouse_down(button)` | 鼠标键是否按住（0=左，1=右，2=中） |
| 时间 | `engine.time.delta()` / `engine.time.elapsed()` | 帧时间 / 累计运行时间（秒） |
| 日志 | `engine.log.info/warn/error(msg)` | 输出到引擎日志/编辑器控制台 |
| 场景 | `engine.scene.load(path)` | 切换到指定场景（`res:/...`），经命令队列延迟到本帧结束后生效；返回 `0` 成功 / `-1` 失败 |
| | `engine.scene.current()` | 当前场景的 `res:/` 路径；无场景时返回 `nil` |
| 音频 | `engine.audio.play_on(h)` | 播放实体上 `AudioSource` 组件，返回 bool |
| 特效 | `engine.fx.burst(h)` | 实体上 `ParticleEmitter2D` 爆发一次，返回 bool |
| JSON | `engine.json.read(path)` | 读取项目内 JSON（支持从 .gpkg 提取），返回 Lua 表 / `nil` |
| 物理 | `engine.physics.set_gravity(x, y)` / `get_gravity()` | 设置/读取当前 2D 物理世界重力（脚本可在关卡切换时调整） |

> `require("common")` 可加载 `res:/scripts/` 下的模块（打包产物中脚本位于 .gpkg 内也能
> require）。2dDemo 的 `scripts/common.lua` 是共享工具模块示例。

## 2.1 主场景（Main Scene）

Core 增加“主场景”概念：游戏启动时自动进入主场景。

- 主场景配置：`project_settings.json` 的 `"main_scene"` 字段（如 `"main_scene":"res:/scenes/main.gesc"`），缺省为 `res:/scenes/main.gesc`。
- 游戏入口（GryceGame 模板）在 `GCore_Init` 前调用 `GCore_SetAutoLoadMainScene(true)`，Core 初始化完成后自动加载主场景；命令行 `--scene <path>` 可覆盖。
- 编辑器默认加载主场景：打开项目后编辑器自动加载 `main_scene` 指定的场景（缺省
  `res:/scenes/main.gesc`），新建项目会立即把脚手架生成的 `main.gesc` 保存为主场景。
- 运行中切换场景用 Lua：`engine.scene.load("res:/scenes/xxx.gesc")`。

示例（在脚本里按下某个键切换到另一个场景）：

```lua
function on_update(dt)
    if engine.input.key_down(32) then          -- Space
        engine.scene.load("res:/scenes/level2.gesc")
    end
end
```

## 3. 暴露属性（props）

脚本顶部的 `props` 表会被同步到组件并序列化：

```lua
props = {
    speed = 2.5,        -- 浮点，Inspector 显示数字框
    label = "player"    -- 字符串，Inspector 显示文本框
}
```

- Inspector 修改后立即写回 Lua 环境（下一帧生效）。
- 随场景保存（.gesc）。
- 热重载（保存脚本/`ReloadScripts`）会保留 Inspector 里改过的值。

## 4. 示例脚本（`examples/3dtest/scripts/`）

- `rotate.lua`：绕 Z 轴自转，速度由 `props.speed` 控制。
- `move.lua`：WASD + Space/Ctrl 移动，速度 `props.speed`。
- `timer.lua`：按 `props.interval` 秒定时打日志。

## 5. GryceGC 打包（GryceSPC）

> GryceGC-A 项目组织与打包标准的完整说明见 [GryceGC-A 标准](./GryceGC-A.md)。

### 5.1 构建游戏模板

```bat
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --target GryceGame --config Release
```

`templates/game_main.cpp`（GryceGame.exe）是一个独立入口：
Core 初始化 → 物理挂载 → Platform 创建窗口 → Renderer → 播放循环（脚本随 `GCore_BeginFrame` 运行）。

### 5.2 打包

```bat
build/bin/Release/grycegc.exe --project examples/3dtest --name MyGame ^
    --build-dir build --config Release --out build/game --author "Your Name"
```

输出 `build/game/MyGame/`（无 `res/` 目录）：

```text
MyGame.exe            游戏入口（对核心 DLL 延迟加载，从 runtime/ 解析）
runtime/              核心运行时 DLL（GryceCore / Renderer / Platform / Physics / glfw）
                      + MSVC/MinGW/GCC 运行时（vcruntime/msvcp140 或 libgcc/libstdc++ 等，兜底用）
assets/*.gpkg         资源包（GPAK 格式，场景、脚本、着色器、模型、纹理等）
gdata                 包元数据：源文件记录（path + SHA-256 + size）、
                      64 字节 SHA-512 密钥（key_sha512_hex）、作者/项目/时间
```

`gdata` 中的密钥由打包的源文件记录派生（SHA-512，64 字节，hex 编码 128 字符），
可用于校验包内容是否被改动；作者等信息通过 `--author` 传入（默认取 `%USERNAME%`）。

### 5.3 运行

```bat
cd build/game/MyGame
MyGame.exe
MyGame.exe --scene res:/scenes/script_test.gesc   # 覆盖主场景
```

项目根默认取 exe 所在目录（`res:/` 以它为根），Core 启动时自动挂载
`assets/` 下的 `.gpkg`，并进入主场景（`project_settings.json` 的 `main_scene`）。
编辑器菜单「文件 → 打包运行（GryceGC）」会自动完成打包。

运行时加载策略：优先使用系统安装的 VC++ 运行时（System32）；系统缺失时，
引擎 DLL 回退到 `runtime/` 里打包的运行时。
