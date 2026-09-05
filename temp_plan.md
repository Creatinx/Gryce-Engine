# GryceEngine UI 系统重构实施计划（自定义 DSL 方向）

> 本文档是按**真实代码现状**修订后的重构计划。真实工程并非从零开始——UI 系统、QuickJS 脚本层、QuickJS 替代 Lua、GPAK 打包均已落地。因此本计划以"现状基线"为起点，把剩余工作组织为一个**将 UI 标记从 XML（pugixml）重构为自定义 DSL（自写 Lexer/Parser + AST + 语义分析）** 的完整实施路径，并对各阶段标注相对现状的具体增量、接口与迁移成本。

---

## 零、现状基线（重构起点，已实现）

以下子系统已存在，重构必须以复用、演进而非推倒重来为原则：

| 子系统 | 位置 | 关键实现 |
|--------|------|----------|
| UI 解析（XML） | `src/ui/uif_parser.cpp` | pugixml；含 15 控件名表 + 属性白名单 + `validate()` 语义校验雏形 |
| 控件树构建 | `src/ui/uif_builder.cpp` | `UIFactory::create(type)` + `setProperty()` 遍历属性 |
| 控件库 | `src/ui/` | Window/Panel/Text/Button/Image/List/Slider/TextInput/CheckBox/ProgressBar/ScrollView/Divider/ComboBox/RadioButton/SpinBox |
| 布局引擎 | `src/ui/panel.cpp` | 内置 Flexbox：align-items / justify-content / flex-grow |
| UI 渲染 | `src/ui/ui_renderer.cpp`、`ui.cpp` | UIRenderer（solid/font/roundrect 着色器）、UIManager |
| 脚本运行时 | `src/script/runtime/script_vm.h/.cpp` | QuickJS 封装：`eval / eval_module / call_module_function / register_function / call_function`，`ScriptResult`，`JSValueWrapper`，64MB 内存 / 256KB 栈 |
| 脚本绑定 | `src/script/bindings/engine_bridge.cpp` 等 | `engine.*`、`math.*`、`big.*` 已全部注册 |
| ECS 脚本 | `src/ecs/systems/script_system.cpp`、`src/components/script_component.h` | 已用 QuickJS `eval_module/call_module_function`；ScriptComponent 已绑定 `.js`，含 `props/script_loaded/start_called/last_error` |
| 热重载 | `src/ui/file_watcher.cpp`、`ui.cpp`、`script_vm.cpp` | UI 与 JS 热重载 |
| 打包 | `src/resources/pak_bundle.cpp` | **GPAK v3**（magic `GPAK`，crc32，base64url 随机资源名），打包入口 `tools/grycegc/main.cpp --pak` |
| 测试 | `tests/` | 450 个测试 / 57 套件，全部通过 |

**已确认的状态结论：**
- **无遗留 `.lua` 文件**，`third_party/` 无 Lua，`CMakeLists.txt` 无 `lua` 引用——Lua 已完全移除。
- `engine.*` 并非计划原稿所列 30 个函数，而是**更充分**的实现，含 UI 顶层 API：`loadScene / playSound / showDialog / closeDialog / getString / bind`。
- `.uif` 当前为 **XML** 语法（`<Window id="..." layout="...">` + 子元素），计划核心任务即将其迁移为自定义 DSL。

---

## 总览

| 阶段 | 周期 | 核心交付物 | 相对现状的性质 |
|------|------|-----------|----------------|
| 阶段一：词法分析器（Lexer） | 第 1 周 | Token 定义、扫描器、行列号错误 | **新增**（DSL 前置） |
| 阶段二：语法分析器（Parser）+ AST | 第 2 周 | 递归下降解析、AST、错误恢复 | **新增** |
| 阶段三：语义检查与 AST 优化 | 第 3-4 周 | SemanticAnalyzer、ASTOptimizer、性能基准 | 语义校验由 `validate()` 演进，其余**新增** |
| 阶段四：DSL 到控件树与布局渲染 | 第 5-6 周 | UIBuilder 接入、Flexbox 布局、UIRenderer | 控件树/布局/渲染已存在，重点为**替换入口** |
| 阶段五：QuickJS ScriptVM 与 SDK 暴露 | 第 7-8 周 | ScriptVM、engine./math. API | **已基本完成**，补充统一接入与 API 测试 |
| 阶段六：ECS ScriptSystem 与迁移 | 第 9-10 周 | QuickJS 驱动生命周期、props 同步、热重载、迁移工具 | 核心已用 QuickJS，补 props 同步/错误恢复/文档 |
| 阶段七：测试、文档与发布打包 | 第 11-14 周 | 全量测试、文档、AES 加密、JS 字节码、GPAK 打包、运行时解密 | 加密/字节码/ResourceLoader 为**主要剩余工作** |

---

## 阶段一：词法分析器（Lexer）（第 1 周）

### 目标
为自定义 DSL 编写词法分析器，将 `.uif` 文本切分为 Token 流，支持带行列号的报错。这是从 pugixml XML 迁移到自写 DSL 的第一步。

### 现状基线
- 现 `uif_parser.cpp` 直接用 pugixml 解析 XML，无独立 Token 层。Lexer 为全新模块。
- 需要**新增源文件** `src/ui/dsl/lexer.h/.cpp`，不触碰现有 pugixml 路径，直到 DSL 解析整体替换验证通过。

### Token 类型定义

| Token 类型 | 示例 | 说明 |
|-----------|------|------|
| `IDENTIFIER` | `Window`, `Panel`, `onStart` | 控件类型名或标识符值 |
| `STRING` | `"主菜单"`, `'Hello'` | 单/双引号字符串，支持转义 |
| `NUMBER` | `800`, `0.5`, `-10` | 整数/浮点，支持负号 |
| `BOOLEAN` | `true`, `false` | 布尔字面量 |
| `EQUALS` | `=` | 赋值符 |
| `COMMA` | `,` | 属性分隔符 |
| `LPAREN` / `RPAREN` | `(` `)` | 属性列表边界 |
| `LBRACE` / `RBRACE` | `{` `}` | 子控件块边界 |
| `LBRACKET` / `RBRACKET` | `[` `]` | 数组边界 |
| `COMMENT_LINE` / `COMMENT_BLOCK` | `//`、`/* */` | 注释（跳过） |
| `EOF` | — | 终止符 |

### 核心接口

```cpp
// GryceEngineUtils::ui 命名空间，GRYCE_API 导出
class Lexer {
public:
    Lexer(const std::string& source, const std::string& filename);
    Token nextToken();
    Token peekToken();
    int getLine() const;
    int getColumn() const;
    std::string getContextLine() const;
private:
    std::string m_source, m_filename;
    int m_pos = 0, m_line = 1, m_column = 1;
    Token m_peeked; bool m_hasPeeked = false;
};

struct Token {
    TokenType type;
    std::string value;
    int line, column;
    bool is(TokenType t) const { return type == t; }
    std::string toString() const;
};
```

### 关键实现细节
- 跳过空白（空格/换行/制表符），实时更新行列计数。
- `//` 跳过到行尾、`/* ... */` 跳过到 `*/`。
- 标识符：字母或下划线开头，后跟字母数字下划线。
- 数字：整数与浮点，负号仅在数字前有效（与 DSL 语法配合）。
- 字符串：单双引号，转义 `\" \' \\ \n \t`。
- 布尔：`true` / `false` 独立 Token。

### 测试用例（第 1 周末交付，GTest）
- 空文件返回 EOF；单标识符正确；单/双引号字符串正确；整数/浮点/负数正确。
- 注释正确跳过；非法字符（如 `@`）报带行列号错误；未闭合字符串报带行列号错误。

### 迁移提示
- 文件顶部直接替换 pugixml 报错来源。现 `parse_string` 的 `pugi::xml_parse_result` 分支在 Lexer 完成后不再存在，改走 `Lexer`。
- 保留一个**双语解析开关**（宏 `GRYCE_UIF_DSL`）：新 DSL 解析通过集成测试前，XML 路径作为 fallback 保留，避免功能回退。

### 验收标准
- 能正确分词含 5 种以上控件的示例 `.uif`（新 DSL 语法）。非法字符与未闭合字符串均带行列号。
- Lexer 单测覆盖率 ≥ 90%。

---

## 阶段二：语法分析器（Parser）+ AST（第 2 周）

### 目标
递归下降解析 Token 流，构建 AST；提供带行列号、含错误恢复的报错。

### 现状基线
- 现 `uif_builder.cpp` 直接遍历 pugixml 节点树。本阶段新增 AST 抽象层，作为 DSL 语义分析与控件树构建的中间表示。

### AST 节点定义

```cpp
struct ASTValue {
    enum class Kind { String, Number, Boolean, Identifier, Array };
    Kind kind;
    std::string stringValue;
    double numberValue;
    bool boolValue;
    std::vector<ASTValue> arrayValue;
    int line, column;
};

struct ASTNode {
    std::string type;                     // 控件类型名
    std::unordered_map<std::string, ASTValue> attributes;
    std::vector<ASTNode> children;
    int line, column;
};
```

### Parser 核心接口

```cpp
struct ParseError {
    int line, column;
    std::string message, contextLine, hint;
    std::string toString() const;
};

class Parser {
public:
    explicit Parser(Lexer& lexer);
    std::vector<ASTNode> parse();
    const std::vector<ParseError>& getErrors() const;
private:
    Lexer& m_lexer;
    std::vector<ParseError> m_errors;
    Token m_current;
    ASTNode parseControl();
    std::unordered_map<std::string, ASTValue> parseAttributes();
    ASTValue parseValue();
    ASTValue parseArray();
    std::vector<ASTNode> parseChildBlock();
    void advance();
    bool expect(TokenType type, const std::string& err);
    void error(const std::string& msg);
    void errorAt(const Token& tok, const std::string& msg);
};
```

### DSL 语法（EBNF）

```
program      = { control };
control      = IDENTIFIER "(" [ attributes ] ")" [ "{" { control } "}" ];
attributes   = attribute { "," attribute };
attribute    = IDENTIFIER "=" value;
value        = STRING | NUMBER | BOOLEAN | IDENTIFIER | array;
array        = "[" [ value { "," value } ] "]";
```

> 对照现有 XML 语义转换示例：
> `Window(id="Main", layout="absolute", bgcolor="#00000000") { Text(text="Hello") }`

### 错误恢复策略
- `parseControl` 遇意外 Token：记录错误并跳到同步点（`IDENTIFIER` / `RBRACE` / `EOF`）继续下一个控件。
- 属性列表格式错误：跳到 `COMMA` 或 `RPAREN`，继续解析后续属性。
- 子控件解析失败：跳过该控件继续。
- 同步点集合：`IDENTIFIER`、`RBRACE`、`EOF`。

### 测试用例（第 2 周末交付）
无属性无子控件 / 带属性 / 嵌套 / 空属性列表 / 数组属性均解析正确；缺失右括号、缺失右花括号、属性缺等号分别报错并恢复；多错误同时存在时全部记录。

### 验收标准
- 完整解析示例 UI，生成正确 AST；常见错误输出带行列号报错；单处错误不导致整个文件解析失败；覆盖率 ≥ 90%。

---

## 阶段三：语义检查与 AST 优化（第 3-4 周）

### 目标
对 AST 校验（控件存在性、属性合法性、类型正确性、必需属性）并做优化（去重、样式展开、空节点移除），同时建立性能基准。

### 现状基线
- 现有 `uif_parser.cpp` 的 `validate()` 已有：控件白名单、属性白名单、空 id 检查、未知属性 warning。可**演进**为 `SemanticAnalyzer`，而非从零。`s_known_controls` / `s_control_attributes` 两张表直接迁移。
- 现有 `stylesheet.cpp` / `style_preset.cpp` 提供样式系统，`expandStyles` 可复用其解析逻辑，不重复实现样式语义。

### SemanticAnalyzer 核心接口

```cpp
struct ControlSchema {
    std::string typeName;
    std::unordered_map<std::string, ASTValue::Kind> allowedAttributes;
    std::vector<std::string> requiredAttributes;
};

struct SemanticError { int line, column; std::string message, hint; };

class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(const std::vector<ControlSchema>& schemas);
    bool analyze(ASTNode& root);
    const std::vector<SemanticError>& getErrors() const;
};
```

### 错误类型表

| 错误类型 | 触发条件 | 示例报错 |
|---------|---------|---------|
| UnknownControl | 类型未注册 | `Unknown control 'Widnow'. Did you mean 'Window'?` |
| UnknownAttribute | 不在白名单 | `Unknown attribute 'colorr' for Button.` |
| InvalidType | 值类型不匹配 | `Attribute 'width' expects number, got string.` |
| MissingAttribute | 必需属性缺失 | `Button missing required attribute 'text'.` |
| DuplicateAttribute | 重复定义 | `Duplicate attribute 'id'.` |

- 拼写纠错：Levenshtein 编辑距离 ≤ 2 时给出建议。`s_known_controls`（15 控件）作为候选表。

### ASTOptimizer 核心接口

```cpp
class ASTOptimizer {
public:
    void optimize(ASTNode& root);
private:
    void foldConstants(ASTNode& n);
    void removeEmptyNodes(ASTNode& n);
    void deduplicateAttributes(ASTNode& n);
    void expandStyles(ASTNode& n);   // 复用 stylesheet.cpp
    void visitChildren(ASTNode& n);
};
```

### 性能基准

| 场景 | 控件数 | 预期解析耗时（Lexer+Parser+Semantic+Optimize） |
|------|-------|-----------------------------------------------|
| 小型 | 10 | < 0.1 ms |
| 中型 | 100 | < 1 ms |
| 大型 | 1000 | < 10 ms |

优化方向（未达标时）：`std::string_view` 减拷贝；预缓存源码行 `std::vector<std::string> m_lines`，`getContextLine()` 直接索引；`ASTValue` 用 `std::variant` 减内存。

内存管理：AST 仅在 `parse` 调用期存在；控件树由 UIBuilder 创建、UIManager 持有；大文件预 `reserve` 减少扩容。

### 测试用例（第 4 周末交付）
样式展开正确、属性去重保留最后一个、空子控件列表移除、性能基准达标。

### 验收标准
优化前后 AST 语义等价；样式正确展开；重复属性去重并告警；大型 UI 解析 < 10 ms。

---

## 阶段四：DSL 到控件树、布局与渲染（第 5-6 周）

### 目标
将 AST 经由 UIBuilder 转为控件树，接入现有 Flexbox 布局与 UIRenderer，实现完整 UI 显示与热重载。

### 现状基线（本阶段复用度最高）
- 控件树 / 布局 / 渲染**均已完成**：`uif_builder.cpp`、`panel.cpp`（Flexbox align/justify/flex-grow）、`ui_renderer.cpp`、`ui.cpp`。
- 本阶段核心工作是**把数据来源从 pugixml 节点树换成 AST**，控件工厂与 setProperty 注入逻辑保持不变。

### UIBuilder 改造

```cpp
class UIBuilder {
public:
    explicit UIBuilder(UIFactory& factory);
    UIElement* build(const ASTNode& node, UIElement* parent = nullptr);
private:
    std::string astValueToString(const ASTValue& v);      // 类型转字符串
};
```

构建流程：
1. `UIFactory::create(ast.type)` 创建控件（复用现有工厂）。
2. 遍历 `attributes`，`astValueToString` 后 `setProperty(key, strValue)`（复用现有属性注入）。
3. 递归构建子控件，`addChild` 建立父子关系。

类型转换规则：String 去引号；Number `std::to_string()`；Boolean `"true"/"false"`；Identifier 原样；Array 转 `["a","b"]` 形式。

防御性检查：控件创建失败（返回 nullptr）记录错误并跳过该控件（上层）；属性注入失败记录 warning 不崩溃。

### DSL 到现有入口的替换点
- 原 `UIParser::parse_file / parse_string` 返回 `pugi::xml_document`，`UIParser::validate(doc)`。
- 改为：`Lexer -> Parser -> AST -> SemanticAnalyzer -> ASTOptimizer -> UIBuilder`，产出控件树。
- `ui.cpp` 中调用解析的路径由 XML 切到 DSL；`GRYCE_UIF_DSL` 宏提供回退（见阶段一）。

### 测试（第 6 周末交付）
- 每个控件类型至少 1 个 AST→控件树转换用例；嵌套父子关系正确；类型转换正确；未知类型返回 nullptr 并记录日志。
- 集成：加载含 Window/Panel/Text/Button 的 DSL 文件，屏幕显示正确；复杂嵌套布局正确；热重载（修改 `.uif` 自动刷新）；内存泄漏测试（反复加载/卸载 100 次无增长）。

### 验收标准
UI 正确显示、布局正确；开发模式热重载正常；无内存泄漏；AST 与控件树一致。

---

## 阶段五：QuickJS ScriptVM 与 SDK 暴露（第 7-8 周）

### 目标
确认 QuickJS 作为唯一脚本运行时，统一暴露完整 `engine.*` / `math.*` / `big.*`。

### 现状基线（本阶段几乎全部完成）
- `src/script/runtime/script_vm.h`：**接口已定型**，与本计划原稿中的临时接口不同，如下：
  - `ScriptResult eval(code, filename)` / `ScriptResult eval_module(code, filename, JSValue* out_module_ns)`
  - `ScriptResult call_module_function(JSValue module_ns, const char* name, const std::vector<JSValueWrapper>& args={})`
  - `call_function(name, args)` / `register_function(name, JSCFunction*)` / `format_exception` / `consume_exception`
  - `context()` / `runtime()`；内存 64MB / 栈 256KB；console.log/warn/error 重定向引擎日志。

- `src/script/bindings/engine_bridge.cpp`：`engine.*` **实际已注册**（非原稿 30 个，更充分）：

| 对象 | 已注册函数 |
|------|-----------|
| engine.log | info, warn, error |
| engine 顶层 | version, self, loadScene, playSound, showDialog, closeDialog, getString, bind |
| engine.entity | get_name, find, find_all, create, destroy, aabb, get_transform, set_transform |
| engine.component | has, get, set |
| engine.state | get, set, has |
| engine.input | key_down, mouse_pos, mouse_down |
| engine.time | delta, elapsed |
| engine.scene | load, current |
| engine.audio | play_on |
| engine.fx | burst |
| engine.json | read |
| engine.physics | set_gravity, get_gravity |

- `math_bridge.cpp`：已含 lerp/inv_lerp/remap/clamp/smoothstep/sign/wrap/pingpong/move_towards/damp/angle_delta/angle_lerp/round/snap 及缓动曲线（quad/cubic/sine/expo/back/elastic/bounce 的 in/out/in_out）等 25 个函数。
- `big_bridge.cpp` + `big_number.cpp`：`big.*` 大数模块。

### 本阶段剩余工作
- 为全部 `engine.* / math.* / big.*` 补齐**逐一 API 测试**（JS 调用与返回值断言），将现有零散测试整合为覆盖清单。
- 审阅 `eval_module`（IIFE + export 转换，见 `script_vm.cpp`）与 `engine_bridge`/`math_bridge` 的注册一致性，确保 UI 层 `engine.showDialog` 等与文档一致。

### 验收标准
`engine.entity.find("player")`、`math.clamp(5,0,10)`、UI 按钮 `onClick=onStart` 均能正确调用 JS。

---

## 阶段六：ECS ScriptSystem 与迁移（第 9-10 周）

### 目标
用 QuickJS 驱动 ECS 脚本生命周期，完善 props 双向同步与错误恢复；提供 Lua→JS 迁移辅助。

### 现状基线（本阶段核心已落地）
- `script_system.cpp` 已用 `ScriptVM::eval_module` 加载 `.js`、`call_module_function` 调用 `on_start/on_update/on_destroy`；全局 ScriptVM 注册 engine/math/big 绑定。
- `script_component.h` 已绑定 `.js`：字段 `script_path`、`props`（JSON）、`script_loaded`、`start_called`、`last_error`；仅 script_path + props 序列化。
- **Lua 已完全移除**：无 `.lua` 遗留、`third_party` 无 Lua、CMake 无 lua 引用、无 `lua_` 头引用。因此原稿"移除 Lua 依赖"步骤已不适用。

### 本阶段剩余工作（补齐项）
1. **props 双向同步**
   - `sync_props_from_js(comp)`：读模块导出的 `props` 对象，经反射写入组件字段（复用 reflection）。
   - `write_prop_to_js(comp, key, value)`：写回模块 props 对象（不存在则创建）。
2. **错误处理**
   - `on_update` 异常：调用失败时暂停该实体（`pause_mode = PAUSED_ERROR`），不崩溃，错误写入 `comp.last_error`，Inspector 可查。
3. **热重载保留 props**
   - 重载前调用 `on_destroy` → 释放旧 module → 缓存旧 props → 重载后恢复，避免 Inspector 修改丢失（现有文件监听 `file_watcher.cpp` 复用）。
4. **Lua→JS 迁移辅助（`lua2js`）**
   - 因已无遗留 Lua，定位为**历史/存量脚本导入工具**而非主路径：`function name() end`→`export function name(){}`、`local x={a=1}`→`const x={a:1}`、`--`→`//`、engine./math. 调用保持等。复杂闭包/元表/协程需手动重写。

### 测试用例（第 10 周末交付）
- props 双向映射正确；热重载保留 props；on_update 抛异常进入暂停态；lua2js 至少 3 个存量脚本转换后运行一致（如有）。

### 验收标准
props 双向同步正确；热重载保留 Inspector 修改；脚本异常进暂停态不崩溃；迁移工具对基础语法有效。

---

## 阶段七：测试、文档与发布打包（第 11-14 周）

### 目标
完成全量测试与文档；实现 AES 加密、JS 字节码编译、GPAK 打包与运行时解密加载。

### 现状基线
- 现有 450 测试 / 57 套件已通过。
- 打包已实现 **GPAK v3**（magic `GPAK`、crc32、base64url 随机资源名），入口 `tools/grycegc/main.cpp --pak`。
- **AES 加密、JS 字节码编译、运行时 ResourceLoader 均未实现**（engine_bridge 无 AES/字节码代码）——这是本阶段的主要新增量。

### 第 11 周：测试体系完善（GTest）

| 类别 | 数量 | 覆盖 |
|------|------|------|
| Lexer | 15 | Token、注释、错误字符、未闭合字符串 |
| Parser | 20 | 控件、嵌套、属性、数组、错误恢复 |
| SemanticAnalyzer | 12 | 未知控件/属性、类型、必需属性 |
| ASTOptimizer | 8 | 样式展开、去重、空节点移除 |
| UIBuilder（DSL） | 10 | 控件转换、属性注入、错误跳过 |
| ScriptVM | 10 | eval/eval_module、call、注册、异常格式化 |
| EngineBridge | 30 | 全部 `engine.*` API |
| MathBridge | 25 | 全部 `math.*` 计算 |
| ScriptSystem | 15 | 加载、生命周期、热重载、props、错误 |
| 加密/打包 | 4+ | AES 往返、Tag 校验、GPAK 生成/加载 |

集成场景：UI+脚本联动（按钮→JS→`engine.scene.load`）；ECS+脚本生命周期；JS/UIF 热重载；发布包加载验证。覆盖率目标 ≥ 80%。

### 第 12 周：文档交付（`docs/`）
1. `UI_DSL_SPEC.md`：DSL EBNF、控件属性清单（15 控件）、完整示例、常见错误。
2. `SCRIPT_API_REFERENCE.md`：`engine.* / math.* / big.*` 全部签名、参数、返回、示例、C++↔JS 类型映射。
3. `ECS_SCRIPT_GUIDE.md`：ES Module `export` 结构、生命周期、props 同步、热重载。
4. `MIGRATION_GUIDE.md`：XML→DSL 转换对照、`lua2js` 用法（存量脚本导入）。

### 第 13 周：JS 编译与加密

**JS 字节码编译（新增文件，如 `tools/packager/` 或扩展 `grycegc/main.cpp`）：**
```cpp
std::vector<uint8_t> CompileJS(const std::string& source) {
    JSRuntime* rt = JS_NewRuntime();
    JSContext* ctx = JS_NewContext(rt);
    JSValue bc = JS_CompileScript(ctx, source.c_str(), source.size(),
                                  "script.js", JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(bc)) { /* 取异常、格式化、抛出 */ }
    std::vector<uint8_t> buf;
    JS_WriteObject(ctx, [](void* op, const uint8_t* d, size_t len)->int {
        static_cast<std::vector<uint8_t>*>(op)->insert(static_cast<std::vector<uint8_t>*>(op)->end(), d, d+len);
        return 0;
    }, &buf, bc, JS_READ_OBJ_BYTECODE);
    JS_FreeValue(ctx, bc); JS_FreeContext(ctx); JS_FreeRuntime(rt);
    return buf; // 平台无关字节码，打包器与引擎须用同一 QuickJS 版本
}
```
- AES-256-GCM 加密（Windows CNG `bcrypt.lib`，Linux 用 OpenSSL 或等效）加密字节码与 DSL 文本。
- 密钥硬编码拆多个常量 + 运行时异或组合。

**GPAK 打包（复用现有 `pak_bundle.cpp`）：**
- JS 字节码加密块、DSL 标记加密块写入 GPAK（沿用 GPAK v3 header/随机 base64url 名），不引入原稿虚构的 `GRPK` 格式。
- CLI：`grycegc --pak --assets ./assets --output game.pak`（release 加密）与 dev 模式（不加密，仅打包）。

### 第 14 周：运行时解密加载与端到端

**新增 `ResourceLoader`（如 `src/resources/resource_loader.h/.cpp`）：**
```cpp
class ResourceLoader {
public:
    bool mount(const std::string& pakPath);          // 复用 PakReader
    std::vector<uint8_t> decryptChunk(uint32_t type, const std::string& id);
    std::string decryptUIText(const std::string& id);
    std::vector<uint8_t> decryptJSBytecode(const std::string& id);
};
```
- **JS 加载**：读加密块 → 解密 → `JS_ReadObject` 恢复字节码 → `JS_EvalFunction` 执行。
- **DSL 加载**：读加密块 → 解密 → 文本送入 `Parser` 流程。

**开发/发布差异：**

| 模式 | DSL 数据源 | JS 数据源 | 热重载 |
|------|-----------|----------|--------|
| 开发 | 磁盘明文 `.uif` | 磁盘明文 `.js` | 支持 |
| 发布 | `.pak` 加密文本 | `.pak` 加密字节码 | 不支持 |

通过宏 `DEVELOPMENT` 控制加载分支：
```cpp
#ifdef DEVELOPMENT
    UIManager::loadFromFile(path);      // DSL 明文
    ScriptVM::eval_file(path);           // JS 明文
#else
    UIManager::loadFromString(ResourceLoader::decryptUIText(id));   // 解密 DSL
    ScriptVM::eval_bytecode(ResourceLoader::decryptJSBytecode(id)); // 解密字节码
#endif
```

**端到端测试：**

| 场景 | 描述 | 预期 |
|------|------|------|
| 发布包加载 | 生成 GPAK，发布模式加载 | UI 正确、脚本正常 |
| 加密校验失败 | 篡改 `.pak` 密文 | 解密失败，回退默认 UI，不崩溃 |
| JS 字节码执行 | 加密字节码正确加载执行 | 全部 API 正常 |
| 无 Lua 依赖 | 发布模式完全不加载 Lua | 编译/运行正常 |
| 内存泄漏 | 反复加载/卸载 UI 100 次 | 无增长 |

### 验收标准
发布模式正确加载 GPAK；篡改密文后回退默认 UI 不崩溃；端到端通过；CMake 无任何 Lua 链接与 `lua_` 引用。

---

## 统一错误处理与报错信息规范

| 错误等级 | 说明 | 开发模式 | 发布模式 |
|---------|------|---------|---------|
| 词法 | 非法字符、未闭合字符串 | 报错并停止该文件解析 | 同左，回退默认 UI |
| 语法 | 缺失括号、属性格式错误 | 报错并尝试恢复 | 同左，回退默认 UI |
| 语义 | 未知控件、属性类型不匹配 | 报错并跳过该控件 | 同左，跳过该控件 |
| 运行时 | JS 异常、控件创建失败 | 报错并暂停实体 | 报错并暂停实体 |
| 解密 | GCM Tag 校验失败 | — | 报错并回退默认 UI |

统一格式：
```
[ERROR] file.uif:12:8 - Unknown control type 'Widnow'
   11 |   Widnow(title="Main")
   12 |     Text(text="Hello")
      |     ^
Hint: Did you mean 'Window'?
```

复用现有 `GLOG`（GLOG_INFO/WARN/ERROR）。`ErrorCollector` 支持 addError/addWarning/hasErrors/formatAll/printToLog。

---

## 风险与应对汇总

| 风险 | 概率 | 影响 | 应对 |
|------|------|------|------|
| DSL 解析性能不达标（>10ms/1000控件） | 中 | 中 | string_view、预缓存源码行、AST vector reserve |
| 语义错误未捕获导致运行时崩溃 | 低 | 高 | UIBuilder 防御性检查、创建失败返回 nullptr |
| 热重载旧模块悬空指针 | 中 | 高 | 先 on_destroy 再释放，再建新模块（现有流程保持一致） |
| XML→DSL 替换导致功能回退 | 中 | 中 | `GRYCE_UIF_DSL` 双轨开关，集成测试通过前保留 XML fallback |
| DSL 语法与现有 XML 控件属性语义漂移 | 中 | 中 | 迁移对照表维护于 `MIGRATION_GUIDE.md`，控件白名单两表共用 |
| 测试覆盖率不足 | 中 | 中 | 80% 目标，gcov/CI 拦截 |
| AES 密钥硬编码被提取 | 低 | 低 | 密钥拆分+异或混淆；仅防普通用户 |
| 解密失败导致 UI 不可用 | 低 | 高 | 内置最简 UI 明文备份，失败回退 |
| QuickJS 字节码跨平台不兼容 | 低 | 中 | 同一 QuickJS 版本（third_party/quickjs），`JS_WRITE_OBJ_BYTECODE` 平台无关格式 |
| 复用现有控件/渲染/脚本时引入耦合 | 中 | 中 | 本计划各阶段明确"复用已存在模块"，仅替换数据入口 |

---

## 进度跟踪说明

- 阶段四之前是 **DSL 重构主战场**（新增 Lexer/Parser/AST/语义/优化）。
- 阶段五、六是**已有能力的验证、整合、补齐**（ScriptSystem/绑定/ScriptComponent 已就位）。
- 阶段七是**真正的增量交付**（AES 加密、JS 字节码、ResourceLoader、运行时分发）。
- 本计划以 `src/ui/`、`src/script/`、`src/resources/`、`src/ecs/systems/script_system.cpp` 为锚点；任何新增文件先确认与此处已有实现的关系，避免重复建设。