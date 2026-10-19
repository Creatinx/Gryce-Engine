# 脚本系统（QuickJS）

本文是**组件级脚本**（`ScriptComponent`，挂在实体上）的完整用法与 API 参考，涵盖挂载方式、生命周期钩子、`engine.*` 基础接口、值约定、沙箱、热重载与排错。

另外两种形态见：

| 形态 | 作用域 | 文档 |
| --- | --- | --- |
| `ScriptComponent` | 实体级，随场景生命周期启停 | 本文 |
| `main.js` 入口脚本 | 进程级，一个进程一个，建窗/载场景/主循环 | [app-entry.md](app-entry.md) |
| `extension.cpp` 原生扩展 | 给入口脚本增补原生接口 | [extension.md](extension.md) |

三者共用同一套 `JSRuntime` 与基础绑定（`engine.log` / `time` / `input` / `entity`），差异在可用 API 面：组件脚本不提供窗口、场景与应用控制接口。

## 1. 组成

| 部分 | 位置 | 说明 |
| --- | --- | --- |
| QuickJS 引擎 | `third_party/quickjs/` | 随仓库分发的 MIT 第三方静态库（版本 2026-06-04），`GryceQuickJS` 目标，原样未改 |
| 运行时封装 | `engine/script/js_runtime.{h,cpp}` | 进程级单例：一个 `JSRuntime` 共享，每组件一个 `JSContext`；内存上限、执行预算、pending job 驱动 |
| 值转换层 | `engine/script/js_value.h` | C++ 与 JS 值互转的唯一入口（标量 / 数学类型 / `nlohmann::json`） |
| 基础绑定 | `engine/script/js_bindings.{h,cpp}` | `engine.log` / `time` / `input` / `entity`（组件视角）与 `console.*` |
| 脚本组件 | `engine/script/script_component.{h,cpp}` | `ScriptComponent`：装载、编译、钩子派发、错误隔离、热重载 |

QuickJS 的 CLI 工具（`qjs.c` / `qjsc.c`）与 `quickjs-libc.c`（`std` / `os` 模块）不参与构建，因此脚本没有文件、进程、网络与定时器能力，只能经 `engine.*` 接触引擎。

## 2. 快速开始

给实体挂 `ScriptComponent`，场景文件里写成：

```json
{
  "type": "Script",
  "enabled": true,
  "script": "res:/scripts/demo_spin.js",
  "props": { "speed": 0.8 }
}
```

`res:/scripts/demo_spin.js`：

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

命令行验证需要 `GryceRenderSmoke`（或 `GryceApp`）并加 `--simulate`，否则只执行 `start`、不驱动 `update`：

```bash
build/GryceRenderSmoke.exe --api opengl --project test/demo_project \
    --scene test/demo_project/scenes/main.gesc --frames 60 --simulate \
    --shot shot.png --no-debug-ui --no-fps
```

## 3. 挂载与序列化字段

`ScriptComponent` 参与场景序列化，字段如下（`engine/script/script_component.cpp` 的 `serialize` / `deserialize`）：

| 字段 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `type` | string | — | 固定 `"Script"`，组件工厂按此名创建 |
| `enabled` | bool | `true` | 组件启用位；实体级 `enabled` 由实体自身控制 |
| `script` | string | `""` | 脚本路径。支持 `res:/`（工程根相对，Pak 包内优先）与绝对路径；空串表示未配置，组件静默不加载 |
| `props` | object | `{}` | 任意 JSON 对象，编译后注入为 `this.props`；非 object 时按空对象处理 |

`props` 支持 JSON 的全部类型并被递归转换为 JS 值：`null` / bool / 整数 / 浮点 / 字符串 / 数组 / 嵌套对象。因此 props 里可以直接放向量与颜色：

```json
{ "type": "Script", "script": "res:/scripts/orbit.js",
  "props": { "center": [0, 2, 0], "radius": 3.5, "speed": 1.2, "label": "orbit" } }
```

```js
const c = this.props.center;   // [0, 2, 0]
const y = c[1] + Math.sin(engine.time.elapsed() * this.props.speed) * this.props.radius;
```

反射注册只覆盖 `script_path` 与 `reload_poll_interval` 两个标量字段；`props` 是嵌套结构，按反射既定口径不注册，编辑器数据直接来自序列化字段本身。

## 4. 生命周期钩子

模块导出（`export function`）的函数即钩子，`this` 是每实例的 `self` 对象（至少含 `props`）。全部钩子可省略。

| 钩子 | 签名 | 调用时机 | 对应组件回调 |
| --- | --- | --- | --- |
| `awake` | `awake()` | 组件挂载后、编译成功时。场景加载即触发；`set_script_path` 换脚本、`reload_now` 也会触发（未 `start` 过时） | `on_awake` |
| `start` | `start()` | 场景开始、所有组件初始化之后；热重载成功且此前已 `start` 过时立即补一次 | `on_start` |
| `_input` | `_input(type, a, b, c)` | 每帧 `update` 之前，本帧输入事件**逐条**派发 | `on_update` 内 |
| `update` | `update(dt)` | 每帧，`dt` 为秒（float） | `on_update` |
| `destroy` | `destroy()` | 组件销毁前（场景卸载、实体销毁、进程退出） | `on_destroy` |

模块顶层代码在编译时执行一次（热重载会重新执行），适合做常量与辅助函数定义；需要引擎状态的初始化放 `awake` / `start`。

`_input` 的事件参数（定义见 `engine/runtime/engine_context.h` 的 `InputEvent` 与 `api/core_api.cpp` 的填充逻辑）：

| `type` | 常量 | `a` | `b` | `c` |
| --- | --- | --- | --- | --- |
| 1 | `INPUT_EVENT_KEY_DOWN` | 键码 | 0 | 0 |
| 2 | `INPUT_EVENT_KEY_UP` | 键码 | 0 | 0 |
| 3 | `INPUT_EVENT_MOUSE_MOVE` | x | y | 0 |
| 4 | `INPUT_EVENT_MOUSE_DOWN` | 按键（0/1/2） | x | y |
| 5 | `INPUT_EVENT_MOUSE_UP` | 按键（0/1/2） | x | y |

事件**不消费**：同一帧内所有脚本组件都会收到本帧全部事件，宿主在帧起点清空队列。因此多个组件各自响应同一事件是正常行为，不需要（也无法）标记已处理。

## 5. `engine.*` 基础 API（组件视角）

以下接口由 `install_engine_bindings`（`engine/script/js_bindings.cpp`）安装，组件脚本与入口脚本都具备。表格中的「异常」列给出参数不合法时的行为：抛 `TypeError` 即调用点可以用 `try/catch` 捕获，并中断当前钩子。

### 5.1 日志

| 签名 | 返回 | 说明 |
| --- | --- | --- |
| `engine.log.info(msg)` | `undefined` | 写 glog info（编辑器控制台可见） |
| `engine.log.warn(msg)` | `undefined` | warn |
| `engine.log.error(msg)` | `undefined` | error |
| `engine.log.debug(msg)` | `undefined` | debug |
| `console.log(...args)` | `undefined` | 与 `engine.log.info` 同一通道，多参数按空格拼接 |
| `console.warn/error/debug(...args)` | `undefined` | 同上，对应级别 |

`engine.log.*` 只接受 1 个参数（0 个参数抛 `TypeError`）；`console.*` 接受任意个数，非标量参数会转成 JSON 文本，便于从网页脚本直接迁移。

### 5.2 时间

| 签名 | 返回 | 说明 |
| --- | --- | --- |
| `engine.time.dt()` | `number` | 本帧 dt（秒），与 `update(dt)` 的实参一致 |
| `engine.time.frame()` | `number` | 组件帧计数，首个 `update` 时为 1 |
| `engine.time.elapsed()` | `number` | 组件运行累计秒数（`update` 调用时已包含本帧 dt） |

时间来源是 `ScriptHost::host_dt/host_frame/host_elapsed`，组件每帧 `on_update` 开头更新；未挂载或已 failed 时不推进。

### 5.3 输入

输入状态来自 `gryce_core::g_input_state`（`EngineContext::input` 的引用目标）：宿主经 `ECMD_INPUT_*` 命令更新，`GCore_BeginFrame` 在帧起点清空事件队列并清零鼠标增量。未接命令总线（例如单元测试里直接驱动组件）时，所有查询返回零值。

| 签名 | 返回 | 说明 |
| --- | --- | --- |
| `engine.input.key_down(code)` | `bool` | 该键当前是否**按住**（持续为 true，不是"本帧按下"） |
| `engine.input.key_up(code)` | `bool` | 该键当前是否**未按住**（注意：不是"本帧抬起"） |
| `engine.input.mouse_x()` | `number` | 鼠标绝对 x（整数像素） |
| `engine.input.mouse_y()` | `number` | 鼠标绝对 y（整数像素） |
| `engine.input.mouse_delta()` | `number[2]` | 本帧累计位移 `[dx, dy]`，float 保留亚像素精度 |
| `engine.input.mouse_button(i)` | `bool` | 鼠标按键按下状态，`i` 取 0/1/2；越界恒 `false` |
| `engine.input.mouse_locked()` | `bool` | 指针锁定状态 |

**键码是 GLFW 键码**：可打印字符用 ASCII 大写（`A=65`、`D=68`、`W=87`、`S=83`、`E=69`、`Q=81`），空格 `32`，功能键在 `256` 以上（`ESC=256`、`LEFT_SHIFT=340`、`LEFT_CONTROL=341`）。平台层只轮询 GLFW 定义的 `32..348` 区间，超出范围的键码查询恒为 `false`。

`key_up` 的语义是"未按住"，因此判断"抬起瞬间"要自己记状态：

```js
let was_down = false;
export function update() {
    const down = engine.input.key_down(32);
    if (was_down && !down) engine.log.info('space released');
    was_down = down;
}
```

### 5.4 宿主实体

组件脚本的 `engine.entity.*` 隐式作用于**宿主实体**（挂载该组件的实体），不需要传句柄；没有宿主实体时（在入口脚本里误用同一套接口，或组件未挂载）抛 `TypeError`。

| 签名 | 返回 | 说明 |
| --- | --- | --- |
| `engine.entity.self()` | `number` | 宿主实体句柄。**v1 恒为 1**，仅作占位标识，不要拿它去别的接口做通用句柄 |
| `engine.entity.name()` | `string` | 实体名 |
| `engine.entity.set_name(s)` | `undefined` | 改名；走命令缓冲，下一帧生效 |
| `engine.entity.position()` | `number[3]` | 本地位置 `[x,y,z]` |
| `engine.entity.set_position(x,y,z)` / `set_position([x,y,z])` | `undefined` | 写本地位置 |
| `engine.entity.rotation()` | `number[4]` | 本地旋转四元数 `[x,y,z,w]` |
| `engine.entity.set_rotation(x,y,z,w)` / `set_rotation([x,y,z,w])` | `undefined` | 写本地旋转 |
| `engine.entity.scale()` | `number[3]` | 本地缩放 |
| `engine.entity.set_scale(x,y,z)` / `set_scale([x,y,z])` | `undefined` | 写本地缩放 |
| `engine.entity.translate(dx,dy,dz)` / `translate([dx,dy,dz])` | `undefined` | 位置累加 |
| `engine.entity.set_enabled(bool)` | `undefined` | 实体启用位 |

位置/旋转/缩放与实体启用位直接写组件宿主实体的 `transform()` / `enabled`，当帧即生效（不经过命令缓冲）；只有 `set_name` 走命令队列，下一帧生效。`set_position` 等同时接受「分量」与「数组」两种形式，都支持时不额外校验元素个数，缺的元素按 `undefined` 处理（转换失败则该分量保持原值）。

组件脚本**没有** `engine.entity.find/all/count/spawn/destroy`——那组接口需要 `Scene` 全量枚举，属入口脚本权限（见 [app-entry.md](app-entry.md) 第 4 节）。

## 6. 值约定

- 向量与四元数用 JS 数组：`[x,y,z]`、`[x,y,z,w]`；矩阵为 16 元素数组，**列主序内存序**（`m[col*4+row]`，与 GLSL / `Matrix4f::m` 一致）。
- 实体句柄是 int（`EntityID` 是 uint64，不进 JS 以避免超过 2^53 丢精度）。
- `undefined` / `null` 转 C++ 标量会失败，调用方按"参数缺失"处理（多数接口因此抛 `TypeError`）；`NaN` 与 `±Infinity` 走 ECMAScript `ToInt32` 语义（`NaN → 0`）。
- Promise 可用：每帧所有 `update` 之后统一排空 pending job（上限 1024 个/帧），没有独立事件循环，也没有 `setTimeout` / `setInterval`。
- 字符串按 UTF-8 处理；`engine.log` 输出的内容以 `[script]` 为前缀。

## 7. 沙箱与限制

- **无文件 / 进程 / 网络 / 定时器**：`std` / `os` 模块不编译，`require` 不存在，`setTimeout` 不存在。脚本只能经 `engine.*` 接触引擎。
- **内存上限**：默认 256MB（`ScriptRuntime::set_memory_limit`），必须在创建任何 context 之前调用才生效；超限的分配失败并以异常形式暴露给脚本。
- **执行预算（opcode + 墙钟）**：每次脚本入口（模块求值、钩子调用、pending job）**各自**计时，默认 64M opcode / 50ms（`ScriptRuntime::set_execution_budget`），由 `JS_SetInterruptHandler` 执行；超预算的入口被中断并记为失败。语义是"单次入口有界"：一个钩子里的死循环会在预算内中断，不会波及其他入口。
- **错误隔离**：编译失败或任一钩子抛错 → 记录带 stack 的日志并置组件为 failed，后续帧不再执行该组件；热重载轮照常运行，修好文件即恢复。同一错误消息只打印一次，之后每 100 次重复打印一行带 `(xN identical)` 的摘要。
- 每组件一个 `JSContext`，全局对象天然隔离（`let` / 全局变量不跨组件共享）；`JSRuntime` 进程级共享，GC 手动触发，无后台线程——Emscripten 无 pthread 环境下同样成立。

## 8. 热重载

`reload_poll_interval`（默认 0.5s，`<=0` 关闭）到时对脚本文件做一次变更检测，发现变化即重新读取源码、**重建 `JSContext`**、重新执行模块并重新提取导出。

检测用 **mtime + 文件大小** 双判据：部分文件系统（FAT/exFAT、网络盘、部分虚拟盘）的写入时间粒度达 1~2 秒，编辑器连续保存会落在同一 tick 内，只比 mtime 会漏检；同 tick 内的改动通常长度也变，大小判据能兜住。长度与 mtime 都不变的改动无法被发现，此时用命令面强制重载。

重载后的状态语义：

- `props` 是组件序列化字段，不随 context 销毁丢失，重编译后重新注入 `this.props`。
- 组件此前已 `start` 过 → 重编译成功立即补一次 `start`（不重放 `awake`）。
- 此前装载失败（文件缺失）→ 按轮询周期重试，文件出现当帧即装载并照常派发 `update`。
- `update` 内的模块级变量、闭包、Promise 全部丢弃（context 重建）；需要跨重载保留的状态放 `props`。

轮询发生在 `update` 派发**之前**，所以保存文件后最多一个轮询周期 + 一帧即可看到新行为。

命令面（编辑器经命令缓冲驱动，实现在 `api/core_api.cpp` 的 `process_command`）：

| 命令 | payload | 行为 |
| --- | --- | --- |
| `ECMD_SET_SCRIPT` | `{ GEntityHandle, char script_path[120] }` | 给目标实体的 `ScriptComponent` 换脚本并立即装载（props 保留）；空串表示卸载 |
| `ECMD_RELOAD_SCRIPTS` | 无 | 对当前场景内所有启用的脚本组件强制重载（忽略 mtime / 大小判据） |

## 9. 排错表

| 现象 | 原因 | 处置 |
| --- | --- | --- |
| 场景加载后脚本不执行，日志有 compile 错误 | 语法错误 / 文件不存在 / 路径前缀不对 | 看日志里的 `name: message` 与 stack；修复文件后热重载自动恢复 |
| 日志反复出现同一脚本错误 | failed 组件不再执行；同消息只在首次与每 100 次重复时打印 | 修复文件；热重载恢复 |
| 脚本 `update` 报 interrupted | 单次入口超过执行预算（默认 64M opcode / 50ms） | 优化脚本；必要时调大 `ScriptRuntime::set_execution_budget` |
| 脚本读不到输入 | 宿主未派发 `ECMD_INPUT_*` 命令（未调 `GInput_SyncToCore`），或命令行为缺 `--simulate` | 确认宿主帧序；`engine.input.*` 会如实返回零值 |
| `engine.input.key_up` 行为不对 | 它的语义是"未按住"，不是"本帧抬起" | 自己维护上一帧状态做边沿检测 |
| `engine.entity.*` 抛 `no host entity` | 在入口脚本里用了组件视角的接口，或组件未挂载 | 入口脚本用 `engine.entity.find(name)` 拿实体代理 |
| 改了脚本没生效 | 改动落在同一 mtime tick 且长度不变 | 触发 `ECMD_RELOAD_SCRIPTS`，或加长/改变文件内容 |
| Web 端脚本不执行 | `res:/scripts/*.js` 未随工程预加载 | 确认文件在工程目录内（MEMFS 预载整个工程目录） |

## 10. 测试与许可

`GryceScriptTests`（`ctest -R script`）覆盖五个套件：

| 套件 | 覆盖 |
| --- | --- |
| `value_convert` | 转换层往返与边界（NaN / Infinity / undefined / 数组 / 矩阵） |
| `lifecycle` | 钩子顺序、props 注入、输入事件派发 |
| `error_isolation` | 语法错误、运行期抛错、死循环中断后其他组件不受影响 |
| `hot_reload` | 文件变更重编译、装载失败后文件出现即装载 |
| `app_entry` | 入口脚本钩子派发、热重载换模块、扩展 `add_function` 注册 |

被测源码直编、不链引擎 DLL、与 GPU 无关，与 math 测试同构。

QuickJS 本体为 MIT 许可（`third_party/quickjs/LICENSE`），随仓库分发、未修改；引擎主体为 AGPL-3.0-or-later，third_party 各库保留原许可。
