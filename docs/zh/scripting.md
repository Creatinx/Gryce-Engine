# 脚本系统（QuickJS）

本文描述引擎的 JavaScript 脚本能力：`ScriptComponent` 组件模型、生命周期、`engine.*` API、沙箱约束、热重载与测试。对应计划见 [`plans/quickjs-integration-plan.md`](plans/quickjs-integration-plan.md)。

## 1. 组成

| 部分 | 位置 | 说明 |
| --- | --- | --- |
| QuickJS 引擎 | `third_party/quickjs/` | 随仓库分发的 MIT 第三方静态库（版本 2026-06-04），`GryceQuickJS` 目标，原样未改 |
| 运行时封装 | `engine/script/js_runtime.{h,cpp}` | 进程级单例：一个 `JSRuntime` 共享，每组件一个 `JSContext`；内存上限、帧中断预算、pending job 驱动 |
| 值转换层 | `engine/script/js_value.h` | C++ 与 JS 值互转的唯一入口（标量 / 数学类型 / nlohmann::json） |
| 原生绑定 | `engine/script/js_bindings.{h,cpp}` | `engine.*` 命名空间（log / time / input / entity） |
| 脚本组件 | `engine/script/script_component.{h,cpp}` | `ScriptComponent`：装载、编译、生命周期调度、错误隔离、热重载 |

QuickJS 的 CLI 工具（`qjs.c` / `qjsc.c`）与 `quickjs-libc.c`（`std` / `os` 模块）不参与构建，沙箱口径见第 6 节。

## 2. 快速开始

演示场景 `test/demo_project/scenes/main.gesc` 的 `ScriptCube` 实体挂了 `res:/scripts/demo_spin.js`：

```json
{
  "type": "Script",
  "enabled": true,
  "script": "res:/scripts/demo_spin.js",
  "props": { "speed": 0.8 }
}
```

```js
export function start() {
    engine.log.info('demo_spin: start, speed=' + this.props.speed);
}

export function update(dt) {
    const half = 0.5 * this.props.speed * dt;
    const s = Math.sin(half);
    const delta = [0.0, s, 0.0, Math.cos(half)]; // 绕 Y 轴增量旋转
    engine.entity.set_rotation(quat_mul(delta, engine.entity.rotation()));
}
```

脚本路径走 `res:/` 前缀（相对工程根，Pak 包内优先），props 以 `this.props` 注入。

命令行验证（`GryceRenderSmoke`）需要 `--simulate` 才会进 Play 模式驱动组件 `on_update`——不加时只初始化场景并逐帧渲染，`start` 会执行、`update` 不执行：

```bash
GryceRenderSmoke.exe --api opengl --project test/demo_project \
    --scene test/demo_project/scenes/main.gesc --frames 60 --simulate \
    --shot shot.png --no-debug-ui --no-fps
```

## 3. 组件模型与生命周期

每个挂载 `ScriptComponent` 的实体拥有独立的 JS 全局环境（独立 `JSContext`，共享进程级 `JSRuntime`）。模块导出的函数即生命周期钩子，`this` 为组件实例对象：

| 钩子 | 调用时机 | 对应组件回调 |
| --- | --- | --- |
| `awake()` | 组件挂载到实体后（场景加载即触发） | `on_awake` |
| `start()` | 场景开始、所有组件初始化后 | `on_start` |
| `update(dt)` | 每帧，`dt` 为秒 | `on_update` |
| `_input(type, a, b, c)` | 每帧 `update` 之前，本帧输入事件逐条派发 | `on_update` 内 |
| `destroy()` | 组件销毁前 | `on_destroy` |

事件类型常量：`_input` 的 `type` 取 `INPUT_EVENT_KEY_DOWN=1` / `KEY_UP=2` / `MOUSE_MOVE=3` / `MOUSE_DOWN=4` / `MOUSE_UP=5`（定义在 `engine/runtime/engine_context.h`）。事件**不消费**——同一帧内所有脚本组件都会收到本帧事件，宿主在帧起点清理队列。

props（序列化字段，`nlohmann::json` 对象）在编译完成后注入为 `this.props`，场景格式：

```json
{ "type": "Script", "script": "res:/scripts/x.js", "props": { "speed": 2.5, "label": "demo" } }
```

反射注册覆盖 `script_path` 与 `reload_poll_interval` 两个标量字段；props 为嵌套结构，按反射既定口径不注册（编辑器数据来自序列化字段本身）。

## 4. `engine.*` API（v1）

| API | 说明 |
| --- | --- |
| `engine.log.info/warn/error/debug(msg)` | 写入 glog（编辑器控制台可见） |
| `console.log/warn/error/debug(...args)` | 同上（多参数按空格拼接），便于网页脚本直接迁移 |
| `engine.time.dt()` | 本帧 dt（秒） |
| `engine.time.frame()` | 组件帧计数（首个 `update` 为 1） |
| `engine.time.elapsed()` | 组件运行累计秒数 |
| `engine.input.key_down(code)` / `key_up(code)` | 按键状态（虚拟键码） |
| `engine.input.mouse_x()` / `mouse_y()` | 鼠标绝对位置 |
| `engine.input.mouse_delta()` | 本帧增量 `[dx, dy]`（float，亚像素精度） |
| `engine.input.mouse_button(i)` | 鼠标按键（0/1/2） |
| `engine.input.mouse_locked()` | 指针锁定状态 |
| `engine.entity.self()` | 宿主实体句柄（v1 恒为 1） |
| `engine.entity.name()` / `set_name(s)` | 实体名 |
| `engine.entity.position()` / `set_position([x,y,z])` | 本地位置 |
| `engine.entity.rotation()` / `set_rotation([x,y,z,w])` | 四元数旋转 |
| `engine.entity.scale()` / `set_scale([x,y,z])` | 缩放 |
| `engine.entity.translate([dx,dy,dz])` | 平移累加 |
| `engine.entity.set_enabled(bool)` | 实体开关 |

输入状态来自 `gryce_core::g_input_state`（`EngineContext::input` 的引用目标）：宿主经 `ECMD_INPUT_*` 命令更新，帧起点由 `GCore_BeginFrame` 清理事件队列。未接命令总线时输入恒为零值。

`set_position` / `set_rotation` / `set_scale` / `translate` 同时接受数组与分量参数两种形式。

不在 v1 的 API：跨实体查询（`engine.find`）、`engine.physics`——二者需要组件到 `Scene` 的反向指针，当前 `Entity` 不持有，留待后续增量。

## 5. 编写约定

- 每个 `.js` 一个组件实例，模块顶层代码在装载时执行一次（热重载会重新执行）。
- 向量 / 四元数用 JS 数组：`[x,y,z]` / `[x,y,z,w]`；矩阵为 16 元素数组，**列主序内存序**（`m[col*4+row]`，与 GLSL 一致）。
- 实体句柄是 int（`EntityID` 是 uint64，不进 JS 以避免超过 2^53 丢精度）。
- Promise 可用：每帧所有 `update` 之后统一排空 pending job（上限 1024 个/帧），没有独立事件循环，也没有 `setTimeout`。

## 6. 沙箱与限制

- **无文件 / 进程 / 网络访问**：`std` / `os` 模块不编译，`require` 不存在；脚本只能经 `engine.*` 绑定接触引擎。
- **内存上限**：默认 256MB（`ScriptRuntime::set_memory_limit`），超限的分配失败并以异常形式暴露给脚本。
- **执行预算（opcode + 墙钟）**：每次脚本入口（模块求值、钩子调用、pending job）各自计时，默认 64M opcode / 50ms（`ScriptRuntime::set_execution_budget`），由 `JS_SetInterruptHandler` 执行；超预算的入口被中断并记为失败。语义是"单次入口有界"：一个钩子里的死循环会在预算内中断，不会影响其他入口。
- **错误隔离**：编译失败或任一钩子抛错 → 记录带 stack 的日志并置组件为 failed，后续帧不再执行该组件；热重载轮照常运行，修好文件即恢复。
- 每组件一个 `JSContext`，全局对象天然隔离；`JSRuntime` 进程级共享，GC 手动触发（无后台线程，Emscripten 无 pthread 环境下同样成立）。

## 7. 热重载

`reload_poll_interval`（默认 0.5s，`<=0` 关闭）到时对脚本文件做一次变更检测；发现变化即重新读取源码、重建 `JSContext` 并重新执行模块。检测用 **mtime + 文件大小** 双判据：部分文件系统（FAT/exFAT、网络盘、部分虚拟盘）的写入时间粒度达 1~2 秒，编辑器连续保存会落在同一 tick 内，只比 mtime 会漏检；同 tick 内的改动通常长度也变，大小判据能兜住。props 是组件序列化字段，不随 context 销毁丢失，重编译后重新注入；若组件已 `start` 过，重编译成功会立即补一次 `start`。装载失败（文件缺失）时按轮询周期重试，文件出现即装载，并在同一帧内照常派发 `update`。

命令面（编辑器经命令缓冲驱动，实现在 `api/core_api.cpp` 的 `process_command`）：

| 命令 | payload | 行为 |
| --- | --- | --- |
| `ECMD_SET_SCRIPT` | `{ GEntityHandle, char script_path[120] }` | 给目标实体的 `ScriptComponent` 换脚本并立即装载（props 保留）；空串表示卸载 |
| `ECMD_RELOAD_SCRIPTS` | 无 | 对当前场景内所有启用的脚本组件强制重载（忽略 mtime / 大小判据） |

## 8. 排错表

| 现象 | 原因 | 处置 |
| --- | --- | --- |
| 场景加载后脚本不执行，日志有 compile 错误 | 语法错误 / 文件不存在 | 看日志里的 stack；修复文件后热重载自动恢复 |
| 日志反复出现同一脚本错误 | failed 组件不再执行；错误消息变化才会再次打印 | 修复文件；热重载恢复 |
| 脚本 update 报 interrupted | 单次入口超过执行预算（默认 64M opcode / 50ms） | 优化脚本；必要时调大 `ScriptRuntime::set_execution_budget` |
| 脚本读不到输入 | 宿主未派发 `ECMD_INPUT_*` 命令 | 经命令总线喂输入；`engine.input.mouse_delta()` 等仍返回零值 |
| Web 端脚本不执行 | `res:/scripts/*.js` 未随工程预加载 | 确认文件在工程目录内（MEMFS 预载整个工程目录） |

## 9. 测试与许可

CTest 新增 `script` 四个套件（`GryceScriptTests.exe`，21 条总门禁的一部分）：`value_convert`（转换层往返与边界）、`lifecycle`（钩子顺序 / props / 输入派发）、`error_isolation`（语法错误 / 运行期抛错 / 死循环中断）、`hot_reload`（文件变更重编译与延迟出现）。与 math 测试同构：被测源码直编、不链引擎 DLL、与 GPU 无关。

QuickJS 本体为 MIT 许可（`third_party/quickjs/LICENSE`），随仓库分发、未修改；引擎主体为 AGPL-3.0-or-later，third_party 各库保留原许可。
