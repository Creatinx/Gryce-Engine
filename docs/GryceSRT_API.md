# GryceSRT 脚本 API 与游戏打包手册

> GryceSRT = GryceEngine Script Runtime（Core 内嵌 Lua 5.4）。
> 脚本通过 `Script` 组件挂到实体上，播放/打包运行时由 ScriptSystem 驱动。

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
| | `engine.entity.get_transform(h)` | 返回 `{pos={x,y,z}, rot={x,y,z,w}, scale={x,y,z}}` |
| | `engine.entity.set_transform(h, pos, rot, scale)` | 写回变换（三个表均可省略） |
| 组件 | `engine.component.get/set`（规划中） | 反射读写任意组件属性 |
| 输入 | `engine.input.key_down(key)` | 按键是否按住（GLFW 键码，如 W=87） |
| | `engine.input.mouse_pos()` | 返回鼠标 x, y |
| | `engine.input.mouse_down(button)` | 鼠标键是否按住（0=左，1=右，2=中） |
| 时间 | `engine.time.delta()` / `engine.time.elapsed()` | 帧时间 / 累计运行时间（秒） |
| 日志 | `engine.log.info/warn/error(msg)` | 输出到引擎日志/编辑器控制台 |

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

### 5.1 构建游戏模板

```bat
cmake -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --target GryceGame --config Release
```

`templates/game_main.cpp`（GryceGame.exe）是一个独立入口：
Core 初始化 → 物理挂载 → Platform 创建窗口 → Renderer → 播放循环（脚本随 `GCore_BeginFrame` 运行）。

### 5.2 打包

```bat
python tools/grycegc.py --project examples/3dtest --name MyGame ^
    --build-dir build --config Release --out build/game
```

输出 `build/game/MyGame/`：`MyGame.exe` + 核心 DLL + `res/`（场景、脚本、资源）。

### 5.3 运行

```bat
cd build/game/MyGame
MyGame.exe --project res --scene res:/scenes/main.gesc
```

注意：`--project` 指向打包后的 `res/` 目录（`res:/` 以它为根）。编辑器菜单
「文件 → 打包运行（GryceGC）」会自动完成打包并启动。
