# 迁移指南（MIGRATION_GUIDE）

> 本文档包含两套迁移对照：
> 1. `.uif` 从 XML（pugixml）迁移到自定义 DSL。
> 2. 存量 Lua 脚本导入到 QuickJS（`lua2js` 工具 + 手工重写清单）。
>
> 背景：Lua 运行时已完全移除（无 `.lua` 遗留、`third_party` 无 Lua、CMake 无
> `lua` 引用），QuickJS 是唯一脚本运行时。

## 1. XML → DSL 转换对照

### 1.1 基础结构

| XML | DSL |
|---|---|
| `<Window id="Main" layout="vertical">` | `Window(id="Main", layout="vertical") {` |
| `</Window>` | `}` |
| 空元素 `<Text text="Hi" />` | `Text(text="Hi")`（无子块） |
| 属性 `id="Main"` | `id="Main"`（值可省引号：`id=Main`） |
| 子元素嵌套 | `{ }` 块嵌套 |
| XML 注释 `<!-- -->` | `//` 行注释 / `/* */` 块注释 |

### 1.2 示例对照

XML：

```xml
<Window id="Main" layout="vertical" padding="16" spacing="8" bgcolor="#202020FF">
    <Button id="Start" text="开始" onClick="onStart" width="200" height="48" />
    <Panel id="Bottom" layout="horizontal" spacing="4">
        <Text id="Ver" text="v1.0" />
        <CheckBox id="Fs" text="全屏" checked="true" />
    </Panel>
</Window>
```

DSL：

```text
Window(id="Main", layout="vertical", padding=16, spacing=8, bgcolor="#202020FF") {
    Button(id="Start", text="开始", onClick="onStart", width=200, height=48)
    Panel(id="Bottom", layout="horizontal", spacing=4) {
        Text(id="Ver", text="v1.0")
        CheckBox(id="Fs", text="全屏", checked=true)
    }
}
```

### 1.3 转换要点

- 数字属性（`width/height/padding/spacing/min/max/value/step`）可省引号写为数字。
- 布尔属性（`checked/visible/enabled`）可写为 `true` / `false`（不带引号）。
- 事件属性 `onClick` / `onChange` 的值是 **JS 函数名**（模块导出函数），不再是 Lua 函数。
- 控件类型名与属性白名单两表共用，非法名称会被语义分析报错并给出拼写建议。
- 解析入口 `UIWidgetBuilder::build_from_dsl_source`；旧 XML 路径保留于
  `uif_parser.cpp`（`GRYCE_UIF_DSL` 双轨开关，集成测试通过前可回退）。

### 1.4 常见迁移错误

```text
// 错误：Button 缺 text（必需属性）
Button(id="Start")

// 错误：宽度类型不匹配
Text(text="Hi", width="大")

// 错误：未知控件（拼写）
Widnow(id="Main")    // Hint: Did you mean 'Window'?

// 错误：缺失右括号
Window(id="Main" {
```

## 2. Lua → JS 迁移（`lua2js`）

### 2.1 工具用法

`lua2js`（`tools/lua2js/`）是历史/存量 Lua 脚本的导入辅助，非主路径：

```cpp
#include "lua2js.h"

lua2js::ConvertResult r = lua2js::convert(luaSource);
// r.ok       是否成功
// r.output   ES Module 风格 JS 源码
// r.warnings 需要人工确认的位置（含 TODO 标记）
```

测试位于 `tests/lua2js_test.cpp`（转换并在 QuickJS 模块中执行验证语义一致）。

### 2.2 语法对照表

| Lua | JS（转换输出） |
|---|---|
| `-- 注释` / `--[[ 块注释 ]]` | `// 注释` / `/* 块注释 */` |
| `function name(a, b) ... end` | `export function name(a, b) { ... }` |
| `local function name(x) ... end` | `function name(x) { ... }` |
| `function Player.new(name) ... end` | `Player.new = function(name) { ... }` |
| `local x = 100` | `let x = 100` |
| `local t = {a = 1, [k] = v}` | `let t = {a: 1, [k]: v}` |
| `a and b or c` / `not a` | `a && b \|\| c` / `!a` |
| `a ~= b` | `a !== b` |
| `a .. b` | `a + b`（字符串拼接见下方说明） |
| `nil` | `null` / `undefined` |
| `#t` | `__len(t)`（注入 `__len` helper） |
| `if/elseif/else/end` | `if/else if/else` |
| 数值 `for i = 1, 10 do ... end` | `for (let i = 1; i <= 10; i++) { ... }` |

### 2.3 需手工重写的内容（转换输出带 TODO 标记）

- 复杂闭包与多返回值。
- 元表（metatable）与继承。
- 协程（`coroutine`）。
- Lua 运算符重载（`big` 数值的 `+` 等）：JS 侧改用 `big.int_add` / `big.decimal_add` 函数式 API。
- `require("common")`：改用 ES Module `import` / 直接 `export`。
- `pcall` 错误捕获：改用 `try/catch`（QuickJS 异常以 `error_msg` 形式返回）。
- 字符串拼接 `..`：数值用 `+`，字符串用 `+` 或模板字符串。

### 2.4 engine.* / math.* 调用

`engine.*`、`math.*` 调用保持同名迁移（Lua 时代 API 与 QuickJS 版基本一致），
注意：

| Lua | JS |
|---|---|
| `engine.component.get(h, type, prop)` | `engine.component.get(h, type)`（返回对象后取字段） |
| `big.decimal("0.1") + big.decimal("0.2")` | `big.decimal_add("0.1", "0.2")` |
| `print(...)` | `console.log(...)`（重定向到引擎日志） |

### 2.5 完整转换示例

Lua：

```lua
props = { speed = 1.0 }

function on_update(dt)
    local t = engine.entity.get_transform(engine.self())
    local p = t.position
    p.x = p.x + props.speed * dt
    engine.entity.set_transform(engine.self(), { position = p })
end
```

转换输出（JS）：

```js
export const props = { speed: 1.0 };

export function on_update(dt) {
    let t = engine.entity.get_transform(engine.self());
    let p = t.position;
    p.x = p.x + props.speed * dt;
    engine.entity.set_transform(engine.self(), { position: p });
}
```

## 3. 打包格式迁移说明

- 发布包统一为 **GPAK v3**（`.pak`），资源名随机 base64url，无逻辑命名。
- `.js` 在打包时编译为 QuickJS 字节码并用 AES-256-GCM 加密；
  `.uif` DSL 文本直接加密；其余资源原样打包。
- 开发/发布加载分支由 `DEVELOPMENT` 宏控制（见
  [SCRIPT_API_REFERENCE](./SCRIPT_API_REFERENCE.md) 第 7 节）。

相关文档：[.uif DSL 规范](./UI_DSL_SPEC.md)、[脚本 API 参考](./SCRIPT_API_REFERENCE.md)、
[ECS 脚本指南](./ECS_SCRIPT_GUIDE.md)。
