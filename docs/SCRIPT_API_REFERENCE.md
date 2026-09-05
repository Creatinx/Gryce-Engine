# 脚本 API 参考（SCRIPT_API_REFERENCE）

> GryceEngine 唯一脚本运行时为 QuickJS（`src/script/runtime/script_vm.cpp`）。
> 本文档列出 `engine.*`、`math.*`、`big.*` 全部已注册函数，
> 以及 C++ ↔ JS 类型映射与 ScriptVM 使用方式。

## 1. C++ ↔ JS 类型映射

| C++ 侧 | JS 侧 |
|---|---|
| `bool` | boolean |
| `int32_t` | number（整数） |
| `double` / `float` | number（浮点） |
| `std::string` / `const char*` | string |
| `std::vector<JSValueWrapper>` | 函数参数列表 |
| `ScriptResult.result` | 返回值的字符串表示（异常时为 `error_msg`） |
| `JSValue module_ns` | ES Module 命名空间对象 |

C++ 参数包装（`JSValueWrapper`）：

```cpp
using namespace GryceEngineUtils::script;
JSValueWrapper(true);          // Bool
JSValueWrapper(42);            // Int
JSValueWrapper(3.14);          // Float
JSValueWrapper("hello");       // String
```

调用示例：

```cpp
ScriptVM vm;
vm.init();
JSValue ns = JS_UNDEFINED;
vm.eval_module("export function add(a, b) { return a + b; }", "mod.js", &ns);
auto r = vm.call_module_function(ns, "add", {JSValueWrapper(2), JSValueWrapper(3)});
// r.success == true, r.result == "5"
vm.shutdown();
```

## 2. ScriptVM 接口摘要

| 方法 | 说明 |
|---|---|
| `bool init()` | 初始化 QuickJS 运行时与上下文，重定向 `console.log/warn/error` 到引擎日志；内存 64MB / 栈 256KB |
| `void shutdown()` | 释放所有资源 |
| `eval(code, filename)` | 全局作用域执行 |
| `eval_module(code, filename, &ns)` | 模块模式执行（ES Module `export` 语法），返回模块命名空间 |
| `compile_script(code, filename, as_module, &err)` | 编译为平台无关字节码（打包器用） |
| `eval_bytecode(bytecode, filename, &ns)` | 加载并执行 `compile_script` 产物 |
| `call_module_function(ns, name, args)` | 调用模块导出的函数 |
| `call_function(name, args)` | 调用全局函数 |
| `register_function(name, JSCFunction*)` | 注册 C++ 函数到 JS 全局 |
| `format_exception(val)` / `consume_exception()` | 异常格式化 / 提取 |

## 3. `engine.*`

### 3.1 日志与运行时

| 函数 | 返回 | 说明 |
|---|---|---|
| `engine.log.info(msg)` / `engine.log.warn(msg)` / `engine.log.error(msg)` | — | 输出到引擎日志 / 编辑器控制台 |
| `engine.version()` | string | `"GryceEngine 0.1.0 (QuickJS)"` |
| `engine.self()` | number | 当前脚本所属实体句柄（0 表示无） |

### 3.2 实体

| 函数 | 返回 | 说明 |
|---|---|---|
| `engine.entity.get_name(handle)` | string | 实体名 |
| `engine.entity.find(name)` | number | 按名字查找实体（0 = 未找到） |
| `engine.entity.find_all(prefix)` | array | 查找 `prefix` 或 `prefix<数字>` 的全部实体句柄 |
| `engine.entity.create(name, parent?)` | number | 在当前场景创建实体（脚本遍历期间安全） |
| `engine.entity.destroy(handle)` | bool | 延迟销毁实体（本帧脚本遍历结束后生效） |
| `engine.entity.aabb(handle)` | object \| null | AABB `{x, y, z, w, h, d}` |
| `engine.entity.get_transform(handle)` | object | `{position, rotation, scale}`（各为 `{x,y,z}`） |
| `engine.entity.set_transform(handle, transform)` | — | 写回变换 |

### 3.3 组件

| 函数 | 返回 | 说明 |
|---|---|---|
| `engine.component.has(handle?, type)` | bool | 实体是否有该类型组件；`handle` 省略时用 `engine.self()` |
| `engine.component.get(handle?, type)` | object \| null | 读取组件字段 `{prop: value}` |
| `engine.component.set(handle?, type, props)` | bool | 写组件字段 |

### 3.4 状态 / 输入 / 时间

| 函数 | 返回 | 说明 |
|---|---|---|
| `engine.state.get(key)` | string | 读取共享状态 |
| `engine.state.set(key, value)` | — | 写入共享状态 |
| `engine.state.has(key)` | bool | 状态是否存在 |
| `engine.input.key_down(key)` | bool | 按键是否按住（GLFW 键码，如 W=87） |
| `engine.input.mouse_pos()` | string | 鼠标 x, y |
| `engine.input.mouse_down(button)` | bool | 鼠标键是否按住（0=左，1=右，2=中） |
| `engine.time.delta()` | number | 帧时间（秒） |
| `engine.time.elapsed()` | number | 累计运行时间（秒） |

### 3.5 场景 / 音频 / 特效 / JSON / 物理

| 函数 | 返回 | 说明 |
|---|---|---|
| `engine.scene.load(path)` | — | 切换到指定场景（`res:/...`） |
| `engine.scene.current()` | string | 当前场景 `res:/` 路径 |
| `engine.audio.play_on(handle)` | bool | 播放实体上 `AudioSource` 组件 |
| `engine.fx.burst(handle)` | bool | 实体上 `ParticleEmitter2D` 爆发一次 |
| `engine.json.read(path)` | object | 读取项目内 JSON |
| `engine.physics.set_gravity(x, y)` | — | 设置 2D 物理世界重力 |
| `engine.physics.get_gravity()` | string | 读取当前重力 |

### 3.6 UI 兼容别名

| 函数 | 返回 | 说明 |
|---|---|---|
| `engine.loadScene(path)` | — | `engine.scene.load` 的 UI 别名 |
| `engine.playSound(path)` | — | 播放音效（UI 层） |
| `engine.showDialog(id)` / `engine.closeDialog()` | — | 打开 / 关闭 UI 对话框 |
| `engine.getString(key)` | string | 读取本地化字符串（无匹配时原样返回 key） |
| `engine.bind(prop, value)` | — | 数据绑定（UI 层） |

## 4. `math.*`

| 函数 | 参数 | 说明 |
|---|---|---|
| `math.lerp(a, b, t)` | 3 | 线性插值 `a + (b-a)*t` |
| `math.inv_lerp(a, b, v)` | 3 | 反插值，把 `v` 映射到 `[0,1]` |
| `math.remap(v, a, b, c, d)` | 5 | 把 `v` 从 `[a,b]` 重映射到 `[c,d]` |
| `math.clamp(v, lo, hi)` | 3 | 限制到 `[lo, hi]` |
| `math.smoothstep(e0, e1, x)` | 3 | Hermite 平滑阶跃 |
| `math.smootherstep(e0, e1, x)` | 3 | Quintic 平滑阶跃 |
| `math.sign(v)` | 1 | 返回 -1 / 0 / 1 |
| `math.wrap(v, len)` | 2 | 循环取模，始终落在 `[0, len)` |
| `math.pingpong(t, len)` | 2 | 0→len→0 往复 |
| `math.move_towards(cur, target, max_delta)` | 3 | 以最大步长逼近目标 |
| `math.damp(cur, target, lambda, dt)` | 4 | 指数衰减逼近（帧率无关） |
| `math.angle_delta(a, b)` | 2 | 最短路径角度差（弧度） |
| `math.angle_lerp(a, b, t)` | 3 | 角度插值（弧度） |
| `math.round(v)` | 1 | 四舍五入 |
| `math.snap(v, step)` | 2 | 按步长吸附 |
| `math.approximately(a, b, eps?)` | 3 | 近似相等（默认 1e-6） |

缓动曲线（均为 1 参，`t ∈ [0,1]`）：

```
ease_linear(t)
ease_in_quad / ease_out_quad / ease_in_out_quad
ease_in_cubic / ease_out_cubic / ease_in_out_cubic
ease_in_sine  / ease_out_sine  / ease_in_out_sine
ease_in_expo  / ease_out_expo  / ease_in_out_expo
ease_in_back  / ease_out_back  / ease_in_out_back
ease_in_elastic / ease_out_elastic / ease_in_out_elastic
ease_in_bounce  / ease_out_bounce  / ease_in_out_bounce
```

示例：

```js
let t = 0.5;
let v = math.lerp(0, 10, t);        // 5
let c = math.clamp(15, 0, 10);      // 10
let e = math.ease_out_cubic(0.5);   // 缓动曲线值
```

## 5. `big.*` 超高精度数值

> 大整数与高精度十进制模块。所有函数以字符串返回结果（避免 JS number 精度损失）。

### 大整数（`big.int`）

| 函数 | 说明 |
|---|---|
| `big.int(x)` | 构造整数（字符串参数） |
| `big.int_add(a, b)` / `int_sub` / `int_mul` / `int_div` / `int_mod` | 加减乘除 / 取模（除零抛异常） |
| `big.int_pow(a, exp)` | 整数次幂（不支持负指数） |
| `big.int_neg(a)` / `int_abs(a)` | 取负 / 绝对值 |
| `big.int_compare(a, b)` | 比较，返回 -1 / 0 / 1 |
| `big.int_sign(a)` | 符号，返回 -1 / 0 / 1 |

### 高精度十进制（`big.decimal`）

| 函数 | 说明 |
|---|---|
| `big.decimal(x)` | 构造十进制数（默认保留 32 位小数，可用 `big.decimal("2", 60)` 指定精度） |
| `big.decimal_add(a, b, precision?)` / `decimal_sub` / `decimal_mul` | 加减乘 |
| `big.decimal_div(a, b, precision?)` | 除法（除零抛异常） |
| `big.decimal_pow(a, exp, precision?)` | 整数次幂 |
| `big.decimal_sqrt(a, precision?)` | 开方 |
| `big.decimal_floor(a)` / `decimal_ceil(a)` / `decimal_round(a)` | 取整 |
| `big.decimal_neg(a)` / `decimal_abs(a)` / `decimal_sign(a)` | 取负 / 绝对值 / 符号 |
| `big.decimal_compare(a, b)` | 比较，返回 -1 / 0 / 1 |

示例：

```js
// 0.1 + 0.2 == 0.3，不存在 double 误差
big.decimal_add("0.1", "0.2");        // "0.3"

// Fibonacci(100) 精确值
let x = big.int("0"), y = big.int("1");
for (let i = 0; i < 100; i++) { const t = big.int_add(x, y); x = y; y = t; }
// x == "354224848179261915075"
```

## 6. 注册与初始化

全局绑定在 `ScriptSystem` 首次加载脚本时自动注册
（`src/ecs/systems/script_system.cpp` 的 `ScriptSystem::vm()`）：

```cpp
GryceEngineUtils::script::register_engine_bindings(vm.context());
GryceEngineUtils::script::register_math_bindings(vm.context());
GryceEngineUtils::script::register_big_bindings(vm.context());
```

UI 事件绑定（`engine.showDialog` / `bind` 等）由 `src/ui/engine_bridge.cpp` 提供，
纯脚本上下文（无 UI）调用时记录 warning。

## 7. 字节码编译与加密加载（发布模式）

- 打包器：`grycegc --pak --assets ./assets --output game.pak`（release 加密）。
  `.js` 先经 `ScriptVM::compile_script` 编译为平台无关字节码，再用 AES-256-GCM 加密；
  `.uif` DSL 文本直接加密；其它资源原样打包。
- 运行时：`ResourceLoader`（`src/resources/resource_loader.cpp`）挂载 `.pak`，
  `decrypt_js_bytecode(id)` 解密后用 `ScriptVM::eval_bytecode` 执行，
  `decrypt_ui_text(id)` 解密后送入 DSL 解析流程。
- 开发模式（`--dev`）：不加密，`.js` / `.uif` 走磁盘明文，支持热重载。

相关文档：[ECS 脚本指南](./ECS_SCRIPT_GUIDE.md)、[.uif DSL 规范](./UI_DSL_SPEC.md)、
[迁移指南](./MIGRATION_GUIDE.md)。
