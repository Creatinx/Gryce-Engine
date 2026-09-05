# UI 控件库重构计划

## 背景

现有控件库存在以下问题（经完整代码审查发现）：

- **缺少** **`disabled`** **状态** — 控件无法禁用，所有交互分支都无禁用检查

- **API 命名不一致** — `is_checked()` vs `checked()`、`set_label()` vs `set_text()`、`min()`/`max()` vs `set_range()`

- **焦点无视觉反馈** — 仅 TextInput 有光标指示焦点，其他控件点击后无焦点环

- **样式解析每帧重跑** — `UIManager::draw_widget()` 每帧调用 `resolve()`，无缓存

- **绘制代码重复** — 多个控件重复相同 hover/pressed 背景色切换和标签文本绘制逻辑

- **绘制辅助函数太少** — `ui_draw.cpp` 只有 3 个函数，缺少文本对齐、阴影、焦点环等

- **部分控件功能缺失** — ScrollView 滚动条不能自动隐藏、ProgressBar 无不确定模式、Slider 不支持垂直方向

## 重构方案（7 阶段，按依赖顺序执行）

### 阶段一：`enabled`/`disabled` 状态支持

**影响 14 个文件 | 优先级：最高**

- `widget.h` — 添加 `enabled_ = true`、`set_enabled()`、`enabled()`；修改 `hit_test()` 检查 `enabled_`

- `stylesheet.h/.cpp` — `Pseudo` 枚举添加 `Disabled`，`matches()` 中匹配 `:disabled`

- 各控件：视觉降级（disabled 时降低不透明度）、交互入口添加 `if (!enabled_) return;`

- `ui.cpp` — `on_keyboard()`、`on_text_input()`、`set_focus()` 添加 enabled 检查

### 阶段二：API 命名一致性

**影响 6 个文件 | 优先级：高 | 与阶段一并行**

- `CheckBox`/`RadioButton` — 添加 `checked()` 别名，保留 `is_checked()`

- `CheckBox`/`RadioButton` — 添加 `set_text()`/`text()` 别名，保留 `set_label()`/`label()`

- `Slider` — 添加 `min_value()`/`max_value()` getter

- `ProgressBar` — 添加 `show_text_label()` 和 `set_show_text_label()`

### 阶段三：焦点视觉反馈

**影响 6 个文件 | 优先级：高 | 与阶段一并行**

- `ui_draw.h/.cpp` — 添加 `draw_focus_ring()` 辅助函数

- `widget.cpp` — 在 `Widget::draw()` 末尾添加焦点环绘制

- `CheckBox`/`RadioButton`/`Slider` — 手动绘制焦点环（它们不调用 Widget::draw）

- `ui.cpp` — `on_mouse_button()` 中为非 TextInput 控件（Button/CheckBox/RadioButton/Slider）设置焦点

### 阶段四：样式解析缓存优化

**影响 3 个文件 | 优先级：中 | 依赖阶段一**

- `widget.h` — 添加 `style_dirty_`、`cached_style_`、`mark_style_dirty()`

- `widget.cpp` — 状态变化时（hover/pressed/focused/enabled/style\_class/style\_id）设置 `style_dirty_ = true`

- `ui.cpp` — `draw_widget()` 中仅当 `style_dirty_` 时调用 `resolve()`，然后缓存结果

### 阶段五：绘制辅助函数扩展

**影响 2 个文件 | 优先级：中 | 独立**

- `ui_draw.h/.cpp` — 添加 `text_align_x()`、`text_center_y()`、`draw_rounded_rect_with_shadow()`、`is_light_color()`

### 阶段六：消除绘制代码重复

**影响 8 个文件 | 优先级：中 | 依赖阶段五**

- `ui_draw.h/.cpp` — 添加 `widget_background()`（根据 hover/pressed 返回正确背景色）、`draw_label_text()`（整合文本对齐、截断、居中）

- 各控件（Button/CheckBox/RadioButton/Slider/ComboBox）— 替换内联代码为辅助函数调用

### 阶段七：缺失控件功能增强

**影响 8 个文件 | 优先级：低 | 独立**

- `ScrollView` — 滚动条自动隐藏

- `ProgressBar` — 不确定模式（动画条纹）

- `Slider` — 垂直方向支持

- `ComboBox` — 键盘导航（上下箭头/Enter/Escape）

## 关键原则

- 所有修改保持向后兼容，旧 API 保留（仅添加新别名/新方法）

- 每个阶段完成后可独立编译通过

- 无 breaking changes

## 验证方式

1. 每个阶段后 `cmake --build build` 确认编译通过
2. 运行 `bin\Debug\gryce_tests.exe` 检查测试
3. 运行 `bin\Debug\uitest.exe` 和 `bin\Debug\ui_demo.exe` 确认控件行为正常

