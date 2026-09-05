# .uif 界面 DSL 规范（UI_DSL_SPEC）

> GryceEngine UI 标记语言。`.uif` 文件从 XML（pugixml）迁移为自定义 DSL，
> 由自研 `Lexer → Parser → SemanticAnalyzer → ASTOptimizer → UIBuilder` 全流程解析。
> 本文档描述 DSL 语法、控件属性清单、完整示例与常见错误。

## 1. 语法（EBNF）

```
program    = { control };
control    = IDENTIFIER "(" [ attributes ] ")" [ "{" { control } "}" ];
attributes = attribute { "," attribute };
attribute  = IDENTIFIER "=" value;
value      = STRING | NUMBER | BOOLEAN | IDENTIFIER | array;
array      = "[" [ value { "," value } ] "]";
```

要点：

- `IDENTIFIER`：控件类型名或标识符值，以字母/下划线开头，后跟字母/数字/下划线。
- `STRING`：单引号或双引号字符串，支持转义 `\" \' \\ \n \t`。
- `NUMBER`：整数与浮点，支持负号。
- `BOOLEAN`：`true` / `false` 字面量。
- 注释：`//` 行注释、`/* ... */` 块注释，解析时跳过。
- 顶层可以有多个控件；控件通过 `{ }` 嵌套表达父子关系。

对照示例（XML → DSL）：

```xml
<Window id="Main" layout="absolute" bgcolor="#00000000">
  <Text text="Hello" />
</Window>
```

```text
Window(id="Main", layout="absolute", bgcolor="#00000000") {
  Text(text="Hello")
}
```

## 2. 控件属性清单（15 控件）

语义校验基于 `SemanticAnalyzer::builtin_schemas()`，与 `uif_parser.cpp` 的
控件/属性白名单两表共用。

### 通用属性语义

| 属性 | 期望类型 | 说明 |
|---|---|---|
| `id` | string | 控件标识 |
| `width` / `height` | number | 尺寸（允许 `auto` 等标识符） |
| `margin` / `padding` / `spacing` | number | 外边距 / 内边距 / 子控件间距 |
| `align` | string | 对齐方式（继承 Flexbox：`flex-start/center/flex-end/stretch` 等） |
| `layout` | string | 布局模式（`vertical` / `horizontal` / `absolute`） |
| `style` | string | 引用样式预设 |
| `bgcolor` | string | 背景色（如 `#RRGGBBAA`） |
| `visible` / `enabled` | boolean | 可见性 / 交互可用性（允许标识符做数据绑定） |

### 控件与属性

| 控件 | 属性 | 必需 |
|---|---|---|
| `Window` | id, title, layout, padding, spacing, align, width, height, margin, style, bgcolor, visible, enabled | — |
| `Panel` | id, layout, padding, spacing, align, width, height, margin, style, bgcolor, visible, enabled | — |
| `Text` | id, text, color, font-size, bgcolor, align, width, height, margin, style, visible | — |
| `Button` | id, text, style, color, font-size, bgcolor, width, height, margin, padding, onClick, visible, enabled | text |
| `Image` | id, src, width, height, margin, visible | src |
| `List` | id, width, height, margin, padding, spacing, visible, enabled | — |
| `Slider` | id, min, max, value, width, height, margin, onChange, visible, enabled | — |
| `TextInput` | id, text, placeholder, width, height, margin, onChange, visible, enabled | — |
| `CheckBox` | id, text, checked, width, margin, onChange, visible, enabled | — |
| `ProgressBar` | id, value, min, max, width, height, margin, visible | — |
| `ScrollView` | id, width, height, margin, padding, visible | — |
| `Divider` | id, width, height, margin, color, visible | — |
| `ComboBox` | id, width, height, margin, onChange, visible, enabled | — |
| `RadioButton` | id, text, checked, group, width, margin, onChange, visible, enabled | — |
| `SpinBox` | id, value, min, max, step, width, margin, onChange, visible, enabled | — |

> 数值型属性：`width/height/margin/padding/spacing/font-size/min/max/value/step`。
> 布尔型属性：`checked/visible/enabled`。其余按字符串处理（允许标识符作枚举/样式名）。
> 事件属性：`onClick`（Button）、`onChange`（Slider/TextInput/CheckBox/ComboBox/RadioButton/SpinBox）。

## 3. 完整示例

```text
Window(id="MainMenu", title="主菜单", layout="vertical", padding=24, spacing=12, bgcolor="#202020FF") {
  Text(id="Title", text="欢迎回来", font-size=32, color="#FFFFFFFF", align="center")
  Button(id="BtnStart", text="开始游戏", onClick="onStart", width=220, height=48)
  Button(id="BtnExit", text="退出", onClick="onExit", width=220, height=48)
  Panel(id="BottomBar", layout="horizontal", spacing=8, margin=8) {
    Text(id="Version", text="v0.1.0", font-size=12, color="#888888FF")
    CheckBox(id="Fullscreen", text="全屏", checked=true)
  }
}
```

数组属性示例：

```text
List(id="Inventory", width=320, height=400) {
  Text(id="Item0", text="长剑")
  Text(id="Item1", text="盾牌")
}
```

## 4. 常见错误

统一错误格式（带行列号与上下文）：

```text
[ERROR] file.uif:3:5 - Unknown control 'Widnow'
   2 |   Window(title="Main")
   3 |     Widnow(title="Main")
     |     ^
Hint: Did you mean 'Window'?
```

| 错误类型 | 触发条件 | 示例 |
|---|---|---|
| 词法错误 | 非法字符（如 `@`）、未闭合字符串 | `[ERROR] file.uif:1:9 - unexpected character '@'` |
| 语法错误 | 缺失 `)` / `}`、属性缺 `=` | `[ERROR] file.uif:2:20 - expected '=' after attribute name` |
| 未知控件 | 控件类型未注册 | `Unknown control 'Widnow'. Hint: Did you mean 'Window'?` |
| 未知属性 | 不在白名单 | `Unknown attribute 'colorr' for Button.` |
| 类型不匹配 | 属性值类型不符 | `Attribute 'width' expects number, got string.` |
| 缺少必需属性 | 必需属性缺失 | `Button missing required attribute 'text'.` |

错误恢复策略：

- 解析失败记录错误并跳到同步点（`IDENTIFIER` / `RBRACE` / `EOF`）继续，单处错误不会导致整个文件解析失败。
- 未知控件仍继续检查其子节点；属性注入失败记录 warning，不崩溃。
- 语义分析使用 Levenshtein 编辑距离（≤ 2）给出控件名拼写建议。

## 5. 解析流程与入口

```text
.uif 文本
  → Lexer（Token 流）
  → Parser（AST）
  → SemanticAnalyzer（语义校验）
  → ASTOptimizer（样式展开 / 属性去重 / 空节点移除）
  → UIWidgetBuilder::build_from_dsl_source（控件树）
```

C++ 入口（`src/ui/uif_builder.cpp`）：

```cpp
GryceEngineUtils::ui::UIWidgetBuilder builder;
std::vector<dsl::SemanticError> errors;
UIBuildResult result = builder.build_from_dsl_source(source, &errors);
// result.root 为控件树根 Widget*
```

## 6. 性能基准

| 场景 | 控件数 | 预期解析耗时（Lexer+Parser+Semantic+Optimize） |
|---|---|---|
| 小型 | 10 | < 0.1 ms |
| 中型 | 100 | < 1 ms |
| 大型 | 1000 | < 10 ms |

相关模块：`src/ui/dsl/`（lexer/parser/semantic_analyzer/ast_optimizer）、
`src/ui/uif_builder.cpp`、`include/GryceEngineUtils/ui/dsl/`。
