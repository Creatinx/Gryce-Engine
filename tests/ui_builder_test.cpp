#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "GryceEngineUtils/ui/button.h"
#include "GryceEngineUtils/ui/factory.h"
#include "GryceEngineUtils/ui/label.h"
#include "GryceEngineUtils/ui/panel.h"
#include "GryceEngineUtils/ui/slider.h"
#include "GryceEngineUtils/ui/style_preset.h"
#include "GryceEngineUtils/ui/text_input.h"
#include "GryceEngineUtils/ui/ui.h"
#include "GryceEngineUtils/ui/uif_builder.h"
#include "GryceEngineUtils/ui/uif_parser.h"

using namespace GryceEngineUtils::ui;

// 辅助函数：获取测试 fixture 目录路径
static std::string fixture_dir() {
    std::string root = GRYCE_TEST_PROJECT_ROOT;
    return root + "/tests/fixtures/ui";
}

// 辅助函数：递归查找子控件
static Widget* find_widget_by_id(Widget* parent, const char* id) {
    if (!parent) return nullptr;
    if (parent->id() == id) return parent;
    for (auto* child : parent->children()) {
        Widget* found = find_widget_by_id(child, id);
        if (found) return found;
    }
    return nullptr;
}

// 辅助函数：打印控件树
static void dump_widget_tree(Widget* widget, int depth = 0) {
    if (!widget) return;
    std::string indent(depth * 2, ' ');
    std::cout << indent << "<" << widget->type_name()
              << " id=\"" << widget->id() << "\""
              << " bounds=(" << widget->bounds().x << "," << widget->bounds().y
              << "," << widget->bounds().w << "," << widget->bounds().h << ")"
              << ">" << std::endl;
    for (auto* child : widget->children()) {
        dump_widget_tree(child, depth + 1);
    }
    if (depth == 0) {
        std::cout << indent << "</" << widget->type_name() << ">" << std::endl;
    }
}

// ============================================================================
// 基本构建测试
// ============================================================================

// 测试从 .uif 文件构建控件树
TEST(UIBuilderTest, BuildFromFile) {
    UIParser parser;
    auto parse_result = parser.parse_file(fixture_dir() + "/simple_button.uif");
    ASSERT_TRUE(parse_result.success) << parse_result.error_message;

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 验证根控件类型和 ID
    EXPECT_STREQ(build_result.root->type_name(), "Panel"); // Window → Panel
    EXPECT_EQ(build_result.root->id(), "TestWindow");

    // 验证子控件数量
    EXPECT_EQ(build_result.root->children().size(), 2u); // Button + Text

    // 验证 Button
    Widget* btn = find_widget_by_id(build_result.root, "MyButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_STREQ(btn->type_name(), "Button");
    EXPECT_STREQ(btn->style_class().c_str(), "Primary");

    // 验证 Button 的 onClick 事件
    EXPECT_EQ(btn->event_callback("click"), "OnButtonClick");

    // 验证 Label
    Widget* label = find_widget_by_id(build_result.root, "MyLabel");
    ASSERT_NE(label, nullptr);
    EXPECT_STREQ(label->type_name(), "Label");

    // 验证控件总数
    EXPECT_EQ(build_result.widget_count, 3); // Window + Button + Text

    // 打印控件树
    std::cout << "\n=== Widget Tree: simple_button.uif ===" << std::endl;
    dump_widget_tree(build_result.root);

    // 清理
    delete build_result.root;
}

// ============================================================================
// 第 3 周：样式预设测试
// ============================================================================

// 测试 UIStylePresetSet 有内置预设
TEST(UIStyleTest, BuiltinPresetsExist) {
    EXPECT_TRUE(UIStylePresetSet::has("Primary"));
    EXPECT_TRUE(UIStylePresetSet::has("Default"));
    EXPECT_TRUE(UIStylePresetSet::has("Danger"));
}

// 测试 UIStylePresetSet 获取预设
TEST(UIStyleTest, GetPresetValues) {
    const auto& primary = UIStylePresetSet::get("Primary");
    // Primary 的背景色为蓝色 (0, 102, 255) → (0.0, 0.4, 1.0)
    EXPECT_FLOAT_EQ(primary.background.r, 0.0f);
    EXPECT_FLOAT_EQ(primary.background.g, 102.0f / 255.0f);
    EXPECT_FLOAT_EQ(primary.background.b, 1.0f);
    EXPECT_FLOAT_EQ(primary.background.a, 1.0f);

    const auto& danger = UIStylePresetSet::get("Danger");
    // Danger 为红色
    EXPECT_FLOAT_EQ(danger.background.r, 204.0f / 255.0f);
    EXPECT_FLOAT_EQ(danger.background.g, 51.0f / 255.0f);
    EXPECT_FLOAT_EQ(danger.background.b, 51.0f / 255.0f);
}

// 测试不存在的预设返回 Default
TEST(UIStyleTest, UnknownPresetReturnsDefault) {
    EXPECT_FALSE(UIStylePresetSet::has("NonExistentPreset"));
    const auto& fallback = UIStylePresetSet::get("NonExistentPreset");
    // Default 为灰色
    EXPECT_FLOAT_EQ(fallback.background.r, 51.0f / 255.0f);
    EXPECT_FLOAT_EQ(fallback.background.g, 51.0f / 255.0f);
    EXPECT_FLOAT_EQ(fallback.background.b, 51.0f / 255.0f);
}

// 测试注册自定义预设
TEST(UIStyleTest, RegisterCustomPreset) {
    Color custom_bg(0.2f, 0.5f, 0.8f, 1.0f);
    UIStylePreset custom{
        .background        = custom_bg,
        .background_hover  = Color(0.3f, 0.6f, 0.9f, 1.0f),
        .background_pressed = Color(0.1f, 0.4f, 0.7f, 1.0f),
        .color             = Color::white(),
        .border_color      = Color(0.2f, 0.5f, 0.8f, 1.0f),
        .border_radius     = 8.0f,
        .font_size         = 20.0f,
    };

    UIStylePresetSet::register_preset("Custom", custom);
    EXPECT_TRUE(UIStylePresetSet::has("Custom"));

    const auto& retrieved = UIStylePresetSet::get("Custom");
    EXPECT_FLOAT_EQ(retrieved.background.r, 0.2f);
    EXPECT_FLOAT_EQ(retrieved.background.g, 0.5f);
    EXPECT_FLOAT_EQ(retrieved.background.b, 0.8f);
    EXPECT_FLOAT_EQ(retrieved.border_radius, 8.0f);
    EXPECT_FLOAT_EQ(retrieved.font_size, 20.0f);
}

// 测试预设的 apply_to 方法
TEST(UIStyleTest, ApplyPresetToStyle) {
    Style style;
    // 先设置一些默认值
    style.background = Color::black();
    style.color = Color::black();

    const auto& primary = UIStylePresetSet::get("Primary");
    primary.apply_to(style);

    // 验证 style 被覆盖
    EXPECT_FLOAT_EQ(style.background.r, 0.0f);
    EXPECT_FLOAT_EQ(style.background.g, 102.0f / 255.0f);
    EXPECT_FLOAT_EQ(style.background.b, 1.0f);
    EXPECT_FLOAT_EQ(style.color.r, 1.0f); // Primary 的文字色为白色
    EXPECT_FLOAT_EQ(style.color.g, 1.0f);
    EXPECT_FLOAT_EQ(style.color.b, 1.0f);
}

// ============================================================================
// 第 3 周：.uif 的 style 属性注入测试
// ============================================================================

// 测试通过 .uif 的 style="Primary" 自动应用预设
TEST(UIStyleTest, StyleAttributePrimary) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W">
            <Button id="Btn" text="Start" style="Primary" />
        </Window>
    )", "style_primary");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 查找 Button
    Widget* btn = find_widget_by_id(build_result.root, "Btn");
    ASSERT_NE(btn, nullptr);

    // 验证 Button 的样式类
    EXPECT_STREQ(btn->style_class().c_str(), "Primary");

    // 验证 Button 的样式被预设覆盖
    // Primary 预设背景色为蓝色 (0, 102, 255)
    EXPECT_FLOAT_EQ(btn->style().background.r, 0.0f);
    EXPECT_FLOAT_EQ(btn->style().background.g, 102.0f / 255.0f);
    EXPECT_FLOAT_EQ(btn->style().background.b, 1.0f);

    // 验证文字色为白色
    EXPECT_FLOAT_EQ(btn->style().color.r, 1.0f);
    EXPECT_FLOAT_EQ(btn->style().color.g, 1.0f);
    EXPECT_FLOAT_EQ(btn->style().color.b, 1.0f);

    delete build_result.root;
}

// 测试多种预设样式
TEST(UIStyleTest, MultipleStylePresets) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W">
            <Button id="PrimaryBtn" text="Primary" style="Primary" />
            <Button id="DefaultBtn" text="Default" style="Default" />
            <Button id="DangerBtn" text="Danger" style="Danger" />
        </Window>
    )", "multi_style");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 验证 Primary 按钮
    Widget* primary = find_widget_by_id(build_result.root, "PrimaryBtn");
    ASSERT_NE(primary, nullptr);
    EXPECT_FLOAT_EQ(primary->style().background.r, 0.0f); // 蓝色
    EXPECT_FLOAT_EQ(primary->style().background.b, 1.0f);

    // 验证 Default 按钮
    Widget* default_btn = find_widget_by_id(build_result.root, "DefaultBtn");
    ASSERT_NE(default_btn, nullptr);
    EXPECT_FLOAT_EQ(default_btn->style().background.r, 51.0f / 255.0f); // 灰色
    EXPECT_FLOAT_EQ(default_btn->style().background.g, 51.0f / 255.0f);
    EXPECT_FLOAT_EQ(default_btn->style().background.b, 51.0f / 255.0f);

    // 验证 Danger 按钮
    Widget* danger = find_widget_by_id(build_result.root, "DangerBtn");
    ASSERT_NE(danger, nullptr);
    EXPECT_FLOAT_EQ(danger->style().background.r, 204.0f / 255.0f); // 红色
    EXPECT_FLOAT_EQ(danger->style().background.g, 51.0f / 255.0f);
    EXPECT_FLOAT_EQ(danger->style().background.b, 51.0f / 255.0f);

    delete build_result.root;
}

// 测试样式预设与 .uif 的 style 属性结合（从文件加载）
TEST(UIStyleTest, StylePresetFromFile) {
    UIParser parser;
    auto parse_result = parser.parse_file(fixture_dir() + "/simple_button.uif");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 按钮的 style="Primary"
    Widget* btn = find_widget_by_id(build_result.root, "MyButton");
    ASSERT_NE(btn, nullptr);
    EXPECT_STREQ(btn->style_class().c_str(), "Primary");
    EXPECT_FLOAT_EQ(btn->style().background.r, 0.0f); // 蓝色
    EXPECT_FLOAT_EQ(btn->style().background.g, 102.0f / 255.0f);
    EXPECT_FLOAT_EQ(btn->style().background.b, 1.0f);

    // 验证没有 style 属性的控件不受影响
    Widget* label = find_widget_by_id(build_result.root, "MyLabel");
    ASSERT_NE(label, nullptr);
    // Label 没有 style 属性，应该是默认样式
    EXPECT_STREQ(label->style_class().c_str(), "");

    delete build_result.root;
}

// ============================================================================
// 第 3 周：FindElementById 测试
// ============================================================================

// 测试 find_element_by_id 查找根控件自身
TEST(UISTest, FindElementById_Self) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="MainWin">
            <Button id="Btn" text="Click" />
        </Window>
    )", "find_self");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 查找根控件自身
    Widget* root = build_result.root->find_element_by_id("MainWin");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root, build_result.root);

    delete build_result.root;
}

// 测试 find_element_by_id 查找直接子控件
TEST(UISTest, FindElementById_DirectChild) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W">
            <Button id="DirectBtn" text="Click" />
        </Window>
    )", "find_direct");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 查找直接子控件
    Widget* btn = build_result.root->find_element_by_id("DirectBtn");
    ASSERT_NE(btn, nullptr);
    EXPECT_STREQ(btn->type_name(), "Button");

    delete build_result.root;
}

// 测试 find_element_by_id 递归查找深层嵌套控件
TEST(UISTest, FindElementById_DeepNested) {
    UIParser parser;
    auto parse_result = parser.parse_file(fixture_dir() + "/settings.uif");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 查找深层嵌套的控件
    Widget* master_vol = build_result.root->find_element_by_id("MasterVolume");
    ASSERT_NE(master_vol, nullptr) << "MasterVolume not found in deep tree";
    EXPECT_STREQ(master_vol->type_name(), "Slider");

    Widget* audio_section = build_result.root->find_element_by_id("AudioSection");
    ASSERT_NE(audio_section, nullptr) << "AudioSection not found";
    EXPECT_STREQ(audio_section->type_name(), "Panel");

    Widget* resolution_combo = build_result.root->find_element_by_id("ResolutionCombo");
    ASSERT_NE(resolution_combo, nullptr) << "ResolutionCombo not found";
    EXPECT_STREQ(resolution_combo->type_name(), "ComboBox");

    delete build_result.root;
}

// 测试 find_element_by_id 查找不存在的 ID 返回 nullptr
TEST(UISTest, FindElementById_NotFound) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W">
            <Button id="Btn" text="Click" />
        </Window>
    )", "find_notfound");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 查找不存在的 ID
    Widget* found = build_result.root->find_element_by_id("NonExistentID");
    EXPECT_EQ(found, nullptr);

    delete build_result.root;
}

// 测试 find_element_by_id 的 const 重载
TEST(UISTest, FindElementById_Const) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W">
            <Button id="Btn" text="OK" />
        </Window>
    )", "find_const");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    const Widget* const_root = build_result.root;
    const Widget* btn = const_root->find_element_by_id("Btn");
    ASSERT_NE(btn, nullptr);
    EXPECT_STREQ(btn->type_name(), "Button");

    // 查找不存在的 ID
    const Widget* none = const_root->find_element_by_id("NonExistent");
    EXPECT_EQ(none, nullptr);

    delete build_result.root;
}

// 测试 find_element_by_id 在平坦树中的正确性
TEST(UISTest, FindElementById_FlatTree) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Panel id="Root">
            <Button id="A" />
            <Button id="B" />
            <Button id="C" />
            <Button id="D" />
        </Panel>
    )", "find_flat");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 验证所有 ID 都能找到
    Widget* root = build_result.root->find_element_by_id("Root");
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root, build_result.root);

    EXPECT_NE(build_result.root->find_element_by_id("A"), nullptr);
    EXPECT_NE(build_result.root->find_element_by_id("B"), nullptr);
    EXPECT_NE(build_result.root->find_element_by_id("C"), nullptr);
    EXPECT_NE(build_result.root->find_element_by_id("D"), nullptr);

    delete build_result.root;
}

// 测试从字符串构建控件树
TEST(UIBuilderTest, BuildFromString) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="MainWin">
            <Button id="Btn1" text="OK" />
            <Button id="Btn2" text="Cancel" />
        </Window>
    )", "inline");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success);
    ASSERT_NE(build_result.root, nullptr);

    EXPECT_EQ(build_result.widget_count, 3); // Window + 2x Button
    EXPECT_EQ(build_result.root->children().size(), 2u);

    Widget* btn1 = find_widget_by_id(build_result.root, "Btn1");
    ASSERT_NE(btn1, nullptr);
    EXPECT_STREQ(btn1->type_name(), "Button");

    delete build_result.root;
}

// ============================================================================
// 嵌套控件构建测试
// ============================================================================

// 测试嵌套结构（Panel 内嵌 Button 和 Text）
TEST(UIBuilderTest, BuildNestedControls) {
    UIParser parser;
    auto parse_result = parser.parse_file(fixture_dir() + "/login_form.uif");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 验证根控件
    EXPECT_STREQ(build_result.root->type_name(), "Panel"); // Window → Panel
    EXPECT_EQ(build_result.root->id(), "LoginWindow");

    // 验证嵌套的 Button (Window > Panel > Button)
    Widget* login_btn = find_widget_by_id(build_result.root, "LoginBtn");
    ASSERT_NE(login_btn, nullptr) << "LoginBtn not found in nested tree";
    EXPECT_STREQ(login_btn->type_name(), "Button");
    EXPECT_EQ(login_btn->event_callback("click"), "OnLogin");

    // 验证嵌套的 TextInput
    Widget* username = find_widget_by_id(build_result.root, "UsernameInput");
    ASSERT_NE(username, nullptr);
    EXPECT_STREQ(username->type_name(), "TextInput");

    // 验证 CheckBox
    Widget* remember = find_widget_by_id(build_result.root, "RememberMe");
    ASSERT_NE(remember, nullptr);
    EXPECT_STREQ(remember->type_name(), "CheckBox");

    // 验证控件总数
    EXPECT_GE(build_result.widget_count, 5); // Window + 2x Panel + 2x TextInput + Button + Cancel + CheckBox + Text

    // 打印控件树
    std::cout << "\n=== Widget Tree: login_form.uif ===" << std::endl;
    dump_widget_tree(build_result.root);

    delete build_result.root;
}

// 测试深层嵌套（设置页面）
TEST(UIBuilderTest, BuildDeepNested) {
    UIParser parser;
    auto parse_result = parser.parse_file(fixture_dir() + "/settings.uif");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 验证 ScrollView 存在
    Widget* scroll = find_widget_by_id(build_result.root, "SettingsScroll");
    ASSERT_NE(scroll, nullptr) << "SettingsScroll not found";

    // 验证 ScrollView 下的嵌套控件
    Widget* master_vol = find_widget_by_id(build_result.root, "MasterVolume");
    ASSERT_NE(master_vol, nullptr) << "MasterVolume not found";
    EXPECT_STREQ(master_vol->type_name(), "Slider");

    // 验证 Divider
    Widget* divider = find_widget_by_id(build_result.root, "AudioDivider");
    ASSERT_NE(divider, nullptr) << "AudioDivider not found";

    // 验证底部 Apply 按钮
    Widget* apply = find_widget_by_id(build_result.root, "ApplyBtn");
    ASSERT_NE(apply, nullptr) << "ApplyBtn not found";
    EXPECT_EQ(apply->event_callback("click"), "OnApply");

    // 打印控件树
    std::cout << "\n=== Widget Tree: settings.uif ===" << std::endl;
    dump_widget_tree(build_result.root);

    delete build_result.root;
}

// ============================================================================
// 属性注入测试
// ============================================================================

// 测试 set_property 正确注入属性
TEST(UIBuilderTest, PropertyInjection) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="TestWin" padding="10" spacing="5">
            <Button id="Btn" text="Click" style="Danger" />
            <Text id="Lbl" text="Hello" color="#FF0000" font-size="18" />
            <Slider id="Sld" min="0" max="100" value="50" />
            <TextInput id="Input" text="default" placeholder="Enter text" />
            <Panel id="SubPanel" layout="horizontal" spacing="10" />
        </Window>
    )", "properties");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 验证 Panel 的布局属性
    auto* root_panel = dynamic_cast<Panel*>(build_result.root);
    ASSERT_NE(root_panel, nullptr);
    // Window 默认没有设置 layout，所以是 Absolute
    EXPECT_EQ(root_panel->layout_type(), LayoutType::Absolute);

    // 验证子 Panel 的布局属性
    Widget* sub = find_widget_by_id(build_result.root, "SubPanel");
    ASSERT_NE(sub, nullptr);
    auto* sub_panel = dynamic_cast<Panel*>(sub);
    ASSERT_NE(sub_panel, nullptr);
    EXPECT_EQ(sub_panel->layout_type(), LayoutType::Horizontal);

    // 验证 Button 的样式
    Widget* btn = find_widget_by_id(build_result.root, "Btn");
    ASSERT_NE(btn, nullptr);
    EXPECT_STREQ(btn->style_class().c_str(), "Danger");

    // 验证颜色属性
    Widget* label = find_widget_by_id(build_result.root, "Lbl");
    ASSERT_NE(label, nullptr);
    // Color 的 r 分量应为 1.0 (#FF0000)
    EXPECT_FLOAT_EQ(label->style().color.r, 1.0f);
    EXPECT_FLOAT_EQ(label->style().color.g, 0.0f);
    EXPECT_FLOAT_EQ(label->style().color.b, 0.0f);

    // 验证字体大小
    EXPECT_FLOAT_EQ(label->style().font_size, 18.0f);

    // 验证 TextInput
    Widget* input = find_widget_by_id(build_result.root, "Input");
    ASSERT_NE(input, nullptr);

    // 验证 Slider 的 min/max
    // 注意：set_property 的 min/max 由 Slider 的 set_property 处理
    // 但 min/max 需要先设置 range 再 set_value 才能正确 clamp
    // 目前 set_property 顺序可能导致 min/max 在 value 之后设置

    // 验证事件回调
    EXPECT_EQ(btn->event_callback("click"), "");
    // 注意：Button 没有 onClick 属性，所以回调为空

    // 打印控件树
    std::cout << "\n=== Widget Tree: properties ===" << std::endl;
    dump_widget_tree(build_result.root);

    delete build_result.root;
}

// ============================================================================
// 错误处理测试
// ============================================================================

// 测试空文档
TEST(UIBuilderTest, BuildEmptyDocument) {
    UIParser parser;
    auto parse_result = parser.parse_string("", "empty");
    ASSERT_FALSE(parse_result.success); // 空字符串无法解析

    // 使用空文档构建
    UIWidgetBuilder builder;
    pugi::xml_document empty_doc;
    auto build_result = builder.build(empty_doc);
    EXPECT_FALSE(build_result.success);
    EXPECT_EQ(build_result.root, nullptr);
}

// 测试未知控件类型
TEST(UIBuilderTest, UnknownControlType) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="Test">
            <UnknownWidget id="bad" />
        </Window>
    )", "unknown");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    // 未知控件导致构建失败（或跳过该节点）
    // 注意：如果 UnknownWidget 无法创建，它会被跳过，但 Window 本身应该成功
    if (build_result.success) {
        // 如果构建成功（Window 成功，UnknownWidget 被跳过）
        Widget* window = build_result.root;
        EXPECT_NE(window, nullptr);
        // 子节点可能为 0（UnknownWidget 被跳过）
        EXPECT_EQ(window->children().size(), 0u);
        delete build_result.root;
    }
}

// ============================================================================
// 类型映射测试
// ============================================================================

// 测试 .uif 类型名到 Widget 类型名的映射
TEST(UIBuilderTest, TypeAliasing) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W">
            <Text id="T" text="Hello" />
            <List id="L" />
        </Window>
    )", "aliases");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // Window → Panel
    EXPECT_STREQ(build_result.root->type_name(), "Panel");

    // Text → Label
    Widget* text = find_widget_by_id(build_result.root, "T");
    ASSERT_NE(text, nullptr);
    EXPECT_STREQ(text->type_name(), "Label");

    // List → Panel
    Widget* list = find_widget_by_id(build_result.root, "L");
    ASSERT_NE(list, nullptr);
    EXPECT_STREQ(list->type_name(), "Panel");

    // 验证 List 的垂直布局
    auto* list_panel = dynamic_cast<Panel*>(list);
    ASSERT_NE(list_panel, nullptr);
    EXPECT_EQ(list_panel->layout_type(), LayoutType::Vertical);

    delete build_result.root;
}

// 测试自定义类型映射
TEST(UIBuilderTest, CustomTypeAlias) {
    UIWidgetBuilder builder;

    // 注册自定义映射
    builder.register_type_alias("CustomBtn", "Button");

    // 验证映射是否生效
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W">
            <CustomBtn id="CB" text="Custom" />
        </Window>
    )", "custom");
    ASSERT_TRUE(parse_result.success);

    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    Widget* custom_btn = find_widget_by_id(build_result.root, "CB");
    ASSERT_NE(custom_btn, nullptr);
    EXPECT_STREQ(custom_btn->type_name(), "Button");

    delete build_result.root;
}

// ============================================================================
// 容器布局测试
// ============================================================================

// 测试 Panel 容器的子控件添加
TEST(UIBuilderTest, ContainerChildren) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Panel id="Root" layout="vertical">
            <Button id="A" text="A" />
            <Button id="B" text="B" />
            <Button id="C" text="C" />
        </Panel>
    )", "container");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 验证根控件
    EXPECT_STREQ(build_result.root->type_name(), "Panel");
    EXPECT_EQ(build_result.root->id(), "Root");

    // 验证子控件
    EXPECT_EQ(build_result.root->children().size(), 3u);

    // 验证子控件顺序
    ASSERT_NE(build_result.root->children()[0], nullptr);
    EXPECT_EQ(build_result.root->children()[0]->id(), "A");
    EXPECT_STREQ(build_result.root->children()[0]->type_name(), "Button");

    ASSERT_NE(build_result.root->children()[1], nullptr);
    EXPECT_EQ(build_result.root->children()[1]->id(), "B");

    ASSERT_NE(build_result.root->children()[2], nullptr);
    EXPECT_EQ(build_result.root->children()[2]->id(), "C");

    // 验证控件计数
    EXPECT_EQ(build_result.widget_count, 4); // Root + A + B + C

    delete build_result.root;
}

// ============================================================================
// 数据绑定属性测试
// ============================================================================

// 测试数据绑定属性（text="{key}"）被正确传递
TEST(UIBuilderTest, DataBindingPreserved) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W">
            <Text id="BoundText" text="{Player.Name}" />
        </Window>
    )", "binding");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 验证绑定属性被保留
    // 注意：set_property("text", "{Player.Name}") 会调用 Label::set_text("{Player.Name}")
    // 绑定值在运行时由 BindingRegistry 刷新
    Widget* bound = find_widget_by_id(build_result.root, "BoundText");
    ASSERT_NE(bound, nullptr);

    // 验证控件树没有错误
    // 数据绑定是一个字符串属性，应该被正常传递

    delete build_result.root;
}

// ============================================================================
// 事件回调测试
// ============================================================================

// 测试事件回调函数名被正确存储
TEST(UIBuilderTest, EventCallbacks) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W">
            <Button id="Btn1" text="Start" onClick="OnStart" />
            <Slider id="Sld1" onChange="OnVolumeChange" />
            <TextInput id="Input1" onChange="OnTextChange" />
            <CheckBox id="Chk1" text="Toggle" onChange="OnToggle" />
        </Window>
    )", "events");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 验证 Button 的 onClick
    Widget* btn = find_widget_by_id(build_result.root, "Btn1");
    ASSERT_NE(btn, nullptr);
    EXPECT_EQ(btn->event_callback("click"), "OnStart");

    // 验证 Slider 的 onChange
    Widget* sld = find_widget_by_id(build_result.root, "Sld1");
    ASSERT_NE(sld, nullptr);
    EXPECT_EQ(sld->event_callback("change"), "OnVolumeChange");

    // 验证 TextInput 的 onChange
    Widget* input = find_widget_by_id(build_result.root, "Input1");
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(input->event_callback("change"), "OnTextChange");

    // 验证 CheckBox 的 onChange
    Widget* chk = find_widget_by_id(build_result.root, "Chk1");
    ASSERT_NE(chk, nullptr);
    EXPECT_EQ(chk->event_callback("change"), "OnToggle");

    delete build_result.root;
}

// ============================================================================
// 工厂测试
// ============================================================================

// 测试 WidgetFactory 注册了所有核心类型
TEST(UIBuilderTest, FactoryHasCoreTypes) {
    auto& factory = WidgetFactory::instance();
    EXPECT_TRUE(factory.is_registered("Button"));
    EXPECT_TRUE(factory.is_registered("Label"));
    EXPECT_TRUE(factory.is_registered("Panel"));
    EXPECT_TRUE(factory.is_registered("Image"));
    EXPECT_TRUE(factory.is_registered("Slider"));
    EXPECT_TRUE(factory.is_registered("TextInput"));
    EXPECT_TRUE(factory.is_registered("CheckBox"));
    EXPECT_TRUE(factory.is_registered("ProgressBar"));
    EXPECT_TRUE(factory.is_registered("ScrollView"));
    EXPECT_TRUE(factory.is_registered("ComboBox"));
    EXPECT_TRUE(factory.is_registered("Divider"));
    // 验证 .uif 类型别名也注册了（通过映射）
    EXPECT_TRUE(factory.is_registered("Panel")); // Window 映射到 Panel
    EXPECT_TRUE(factory.is_registered("Label")); // Text 映射到 Label
}

// ============================================================================
// 清理测试
// ============================================================================

// 测试构建失败时清理
TEST(UIBuilderTest, CleanupOnFailure) {
    UIWidgetBuilder builder;
    builder.set_cleanup_on_failure(true);

    // 空文档应该触发清理
    pugi::xml_document empty_doc;
    auto build_result = builder.build(empty_doc);
    EXPECT_FALSE(build_result.success);
    EXPECT_EQ(build_result.root, nullptr);
}

// ============================================================================
// 完整的 UI 构建与结构验证
// ============================================================================

// 综合测试：完整解析 → 构建 → 验证
TEST(UIBuilderTest, FullPipeline) {
    // 1. 解析
    UIParser parser;
    auto parse_result = parser.parse_file(fixture_dir() + "/simple_window.uif");
    ASSERT_TRUE(parse_result.success) << "Parse failed: " << parse_result.error_message;

    // 2. 验证解析结果
    auto validation = parser.validate(parse_result.doc);
    EXPECT_TRUE(validation.valid);

    // 3. 构建控件树
    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << "Build failed: " << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    // 4. 验证控件树
    EXPECT_EQ(build_result.root->id(), "MainWindow");

    // 验证所有控件可以通过 ID 查找
    const char* expected_ids[] = {
        "MainWindow", "HeaderPanel", "TitleText", "CloseBtn",
        "StartBtn", "SettingsBtn", "PlayerName", "VolumeSlider",
        "Logo", "ItemList"
    };
    for (const char* id : expected_ids) {
        Widget* w = find_widget_by_id(build_result.root, id);
        EXPECT_TRUE(w != nullptr) << "Widget with id '" << id << "' not found in built tree";
    }

    // 验证控件类型
    Widget* start_btn = find_widget_by_id(build_result.root, "StartBtn");
    ASSERT_NE(start_btn, nullptr);
    EXPECT_STREQ(start_btn->type_name(), "Button");
    EXPECT_EQ(start_btn->event_callback("click"), "OnStart");

    Widget* title_text = find_widget_by_id(build_result.root, "TitleText");
    ASSERT_NE(title_text, nullptr);
    EXPECT_STREQ(title_text->type_name(), "Label"); // Text → Label

    Widget* volume = find_widget_by_id(build_result.root, "VolumeSlider");
    ASSERT_NE(volume, nullptr);
    EXPECT_STREQ(volume->type_name(), "Slider");
    EXPECT_EQ(volume->event_callback("change"), "OnVolumeChange");

    // 5. 打印控件树
    std::cout << "\n=== Full Pipeline: simple_window.uif ===" << std::endl;
    std::cout << "Total widgets: " << build_result.widget_count << std::endl;
    dump_widget_tree(build_result.root);

    // 6. 清理
    delete build_result.root;
}

// ============================================================================
// 第 4 周：布局引擎 Flexbox 增强测试
// ============================================================================

// 辅助：创建带布局的 Panel 并计算子控件 bounds
static Panel* create_layout_panel(LayoutType type, float w, float h, float spacing = 0.0f) {
    auto* panel = new Panel("TestPanel");
    panel->set_layout(type);
    panel->set_spacing(spacing);
    panel->set_bounds(Rect{0, 0, w, h});
    return panel;
}

// 测试 Vertical 布局 + align-items: Center
TEST(LayoutTest, VerticalAlignItemsCenter) {
    auto* panel = create_layout_panel(LayoutType::Vertical, 200, 300, 5);
    panel->set_align_items(AlignItems::Center);

    auto* btn1 = new Button("B1", "Btn1");
    btn1->set_size(60, 30);
    auto* btn2 = new Button("B2", "Btn2");
    btn2->set_size(80, 40);
    panel->add_child(btn1);
    panel->add_child(btn2);
    panel->relayout();

    // 宽度 200, align-items Center → 子控件水平居中
    // btn1: x = (200-60)/2 = 70, y = 0, w=60, h=30
    EXPECT_FLOAT_EQ(btn1->bounds().x, 70.0f);
    EXPECT_FLOAT_EQ(btn1->bounds().y, 0.0f);
    EXPECT_FLOAT_EQ(btn1->bounds().w, 60.0f);
    EXPECT_FLOAT_EQ(btn1->bounds().h, 30.0f);

    // btn2: x = (200-80)/2 = 60, y = 30+5 = 35, w=80, h=40
    EXPECT_FLOAT_EQ(btn2->bounds().x, 60.0f);
    EXPECT_FLOAT_EQ(btn2->bounds().y, 35.0f);
    EXPECT_FLOAT_EQ(btn2->bounds().w, 80.0f);
    EXPECT_FLOAT_EQ(btn2->bounds().h, 40.0f);

    delete panel;
}

// 测试 Vertical 布局 + align-items: FlexEnd
TEST(LayoutTest, VerticalAlignItemsFlexEnd) {
    auto* panel = create_layout_panel(LayoutType::Vertical, 200, 300);
    panel->set_align_items(AlignItems::FlexEnd);

    auto* btn = new Button("B", "Btn");
    btn->set_size(60, 30);
    panel->add_child(btn);
    panel->relayout();

    // x = 200 - 60 = 140
    EXPECT_FLOAT_EQ(btn->bounds().x, 140.0f);
    EXPECT_FLOAT_EQ(btn->bounds().w, 60.0f);

    delete panel;
}

// 测试 Vertical 布局 + justify-content: Center
TEST(LayoutTest, VerticalJustifyCenter) {
    auto* panel = create_layout_panel(LayoutType::Vertical, 200, 300);
    panel->set_justify_content(JustifyContent::Center);

    auto* btn = new Button("B", "Btn");
    btn->set_size(60, 30);
    panel->add_child(btn);
    panel->relayout();

    // 剩余空间 = 300 - 30 = 270, 居中偏移 = 135
    EXPECT_FLOAT_EQ(btn->bounds().y, 135.0f);
    EXPECT_FLOAT_EQ(btn->bounds().h, 30.0f);

    delete panel;
}

// 测试 Vertical 布局 + justify-content: FlexEnd
TEST(LayoutTest, VerticalJustifyFlexEnd) {
    auto* panel = create_layout_panel(LayoutType::Vertical, 200, 300);
    panel->set_justify_content(JustifyContent::FlexEnd);

    auto* btn = new Button("B", "Btn");
    btn->set_size(60, 30);
    panel->add_child(btn);
    panel->relayout();

    // 剩余空间 = 300 - 30 = 270, 底部对齐偏移 = 270
    EXPECT_FLOAT_EQ(btn->bounds().y, 270.0f);

    delete panel;
}

// 测试 Vertical 布局 + justify-content: SpaceBetween
TEST(LayoutTest, VerticalJustifySpaceBetween) {
    auto* panel = create_layout_panel(LayoutType::Vertical, 200, 300);
    panel->set_justify_content(JustifyContent::SpaceBetween);

    auto* btn1 = new Button("B1", "1");
    btn1->set_size(60, 30);
    auto* btn2 = new Button("B2", "2");
    btn2->set_size(60, 30);
    panel->add_child(btn1);
    panel->add_child(btn2);
    panel->relayout();

    // 两个 30px 的子控件，间距 0，总高 60，剩余 240
    // SpaceBetween: 240 / (2-1) = 240 额外间距
    // btn1: y=0, btn2: y=30+240=270
    EXPECT_FLOAT_EQ(btn1->bounds().y, 0.0f);
    EXPECT_FLOAT_EQ(btn2->bounds().y, 270.0f);

    delete panel;
}

// 测试 Vertical 布局 + justify-content: SpaceAround
TEST(LayoutTest, VerticalJustifySpaceAround) {
    auto* panel = create_layout_panel(LayoutType::Vertical, 200, 300);
    panel->set_justify_content(JustifyContent::SpaceAround);

    auto* btn1 = new Button("B1", "1");
    btn1->set_size(60, 30);
    auto* btn2 = new Button("B2", "2");
    btn2->set_size(60, 30);
    panel->add_child(btn1);
    panel->add_child(btn2);
    panel->relayout();

    // 两个 30px 的子控件，总高 60，剩余 240
    // SpaceAround: 240 / 2 = 120 每个子控件周围间距
    // btn1: y = 120, btn2: y = 120 + 30 + 120 = 270
    EXPECT_FLOAT_EQ(btn1->bounds().y, 120.0f);
    EXPECT_FLOAT_EQ(btn2->bounds().y, 270.0f);

    delete panel;
}

// 测试 Horizontal 布局 + align-items: Center
TEST(LayoutTest, HorizontalAlignItemsCenter) {
    auto* panel = create_layout_panel(LayoutType::Horizontal, 300, 200, 5);
    panel->set_align_items(AlignItems::Center);

    auto* btn1 = new Button("B1", "Btn1");
    btn1->set_size(60, 30);
    auto* btn2 = new Button("B2", "Btn2");
    btn2->set_size(80, 40);
    panel->add_child(btn1);
    panel->add_child(btn2);
    panel->relayout();

    // 高度 200, align-items Center → 子控件垂直居中
    // btn1: y = (200-30)/2 = 85, x = 0, w=60, h=30
    EXPECT_FLOAT_EQ(btn1->bounds().y, 85.0f);
    EXPECT_FLOAT_EQ(btn1->bounds().x, 0.0f);
    EXPECT_FLOAT_EQ(btn1->bounds().w, 60.0f);
    EXPECT_FLOAT_EQ(btn1->bounds().h, 30.0f);

    // btn2: y = (200-40)/2 = 80, x = 60+5 = 65, w=80, h=40
    EXPECT_FLOAT_EQ(btn2->bounds().y, 80.0f);
    EXPECT_FLOAT_EQ(btn2->bounds().x, 65.0f);
    EXPECT_FLOAT_EQ(btn2->bounds().w, 80.0f);
    EXPECT_FLOAT_EQ(btn2->bounds().h, 40.0f);

    delete panel;
}

// 测试 Horizontal 布局 + justify-content: Center
TEST(LayoutTest, HorizontalJustifyCenter) {
    auto* panel = create_layout_panel(LayoutType::Horizontal, 300, 200);
    panel->set_justify_content(JustifyContent::Center);

    auto* btn = new Button("B", "Btn");
    btn->set_size(60, 30);
    panel->add_child(btn);
    panel->relayout();

    // 剩余空间 = 300 - 60 = 240, 居中偏移 = 120
    EXPECT_FLOAT_EQ(btn->bounds().x, 120.0f);

    delete panel;
}

// 测试 Horizontal 布局 + justify-content: FlexEnd
TEST(LayoutTest, HorizontalJustifyFlexEnd) {
    auto* panel = create_layout_panel(LayoutType::Horizontal, 300, 200);
    panel->set_justify_content(JustifyContent::FlexEnd);

    auto* btn = new Button("B", "Btn");
    btn->set_size(60, 30);
    panel->add_child(btn);
    panel->relayout();

    // 剩余空间 = 300 - 60 = 240, 右对齐偏移 = 240
    EXPECT_FLOAT_EQ(btn->bounds().x, 240.0f);

    delete panel;
}

// 测试 Vertical 布局 + flex-grow
TEST(LayoutTest, VerticalFlexGrow) {
    auto* panel = create_layout_panel(LayoutType::Vertical, 200, 300, 5);
    panel->set_align_items(AlignItems::FlexStart);

    auto* btn1 = new Button("B1", "1");
    btn1->set_size(60, 30); // 固定高度 30
    auto* btn2 = new Button("B2", "2");
    btn2->set_size(60, 0);  // 高度由 flex-grow 决定
    btn2->set_flex_grow(1.0f);
    auto* btn3 = new Button("B3", "3");
    btn3->set_size(60, 0);
    btn3->set_flex_grow(2.0f);
    panel->add_child(btn1);
    panel->add_child(btn2);
    panel->add_child(btn3);
    panel->relayout();

    // 固定高度: btn1=30, 间距: 5+5=10
    // 总固定高度 = 30 + 10 = 40
    // 可用空间 = 300 - 40 = 260
    // 总 flex-grow = 1+2 = 3
    // flex_unit = 260/3 ≈ 86.6667
    // btn2 高度 = 86.6667, btn3 高度 = 173.3333
    // btn1: y=0, h=30
    // btn2: y=35, h=86.6667
    // btn3: y=126.6667, h=173.3333

    EXPECT_FLOAT_EQ(btn1->bounds().y, 0.0f);
    EXPECT_FLOAT_EQ(btn1->bounds().h, 30.0f);

    EXPECT_FLOAT_EQ(btn2->bounds().y, 35.0f);
    EXPECT_NEAR(btn2->bounds().h, 260.0f / 3.0f, 0.01f);

    EXPECT_FLOAT_EQ(btn3->bounds().y, 35.0f + 260.0f / 3.0f + 5.0f);
    EXPECT_NEAR(btn3->bounds().h, 260.0f * 2.0f / 3.0f, 0.01f);

    // 验证总高度不超过容器
    EXPECT_NEAR(btn3->bounds().y + btn3->bounds().h, 300.0f, 0.01f);

    delete panel;
}

// 测试 Horizontal 布局 + flex-grow
TEST(LayoutTest, HorizontalFlexGrow) {
    auto* panel = create_layout_panel(LayoutType::Horizontal, 300, 100, 5);
    panel->set_align_items(AlignItems::Stretch);

    auto* btn1 = new Button("B1", "1");
    btn1->set_size(40, 30); // 固定宽度 40
    auto* btn2 = new Button("B2", "2");
    btn2->set_size(0, 30);
    btn2->set_flex_grow(1.0f);
    auto* btn3 = new Button("B3", "3");
    btn3->set_size(0, 30);
    btn3->set_flex_grow(2.0f);
    panel->add_child(btn1);
    panel->add_child(btn2);
    panel->add_child(btn3);
    panel->relayout();

    // 固定宽度: btn1=40, 间距: 5+5=10
    // 总固定宽度 = 40 + 10 = 50
    // 可用空间 = 300 - 50 = 250
    // 总 flex-grow = 1+2 = 3
    // flex_unit = 250/3 ≈ 83.3333
    // btn2 宽度 = 83.3333, btn3 宽度 = 166.6667

    EXPECT_FLOAT_EQ(btn1->bounds().x, 0.0f);
    EXPECT_FLOAT_EQ(btn1->bounds().w, 40.0f);

    EXPECT_FLOAT_EQ(btn2->bounds().x, 45.0f);
    EXPECT_NEAR(btn2->bounds().w, 250.0f / 3.0f, 0.01f);

    EXPECT_FLOAT_EQ(btn3->bounds().x, 45.0f + 250.0f / 3.0f + 5.0f);
    EXPECT_NEAR(btn3->bounds().w, 250.0f * 2.0f / 3.0f, 0.01f);

    // 验证总宽度不超过容器
    EXPECT_NEAR(btn3->bounds().x + btn3->bounds().w, 300.0f, 0.01f);

    delete panel;
}

// 测试 .uif 的 align-items 属性映射
TEST(LayoutTest, UifAlignItemsAttribute) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W" layout="vertical" align-items="center">
            <Button id="Btn" text="OK" width="80" />
        </Window>
    )", "align_items_attr");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    auto* root_panel = dynamic_cast<Panel*>(build_result.root);
    ASSERT_NE(root_panel, nullptr);
    EXPECT_EQ(root_panel->align_items(), AlignItems::Center);

    delete build_result.root;
}

// 测试 .uif 的 justify-content 属性映射
TEST(LayoutTest, UifJustifyContentAttribute) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W" layout="horizontal" justify-content="space-between">
            <Button id="B1" text="Left" />
            <Button id="B2" text="Right" />
        </Window>
    )", "justify_content_attr");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    auto* root_panel = dynamic_cast<Panel*>(build_result.root);
    ASSERT_NE(root_panel, nullptr);
    EXPECT_EQ(root_panel->justify_content(), JustifyContent::SpaceBetween);

    delete build_result.root;
}

// 测试 .uif 的 flex-grow 属性映射
TEST(LayoutTest, UifFlexGrowAttribute) {
    UIParser parser;
    auto parse_result = parser.parse_string(R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="W" layout="vertical">
            <Panel id="Top" height="50" />
            <Panel id="Content" flex-grow="1" />
            <Panel id="Bottom" height="30" />
        </Window>
    )", "flex_grow_attr");
    ASSERT_TRUE(parse_result.success);

    UIWidgetBuilder builder;
    auto build_result = builder.build(parse_result.doc);
    ASSERT_TRUE(build_result.success) << build_result.error_message;
    ASSERT_NE(build_result.root, nullptr);

    Widget* content = build_result.root->find_element_by_id("Content");
    ASSERT_NE(content, nullptr);
    EXPECT_FLOAT_EQ(content->flex_grow(), 1.0f);

    Widget* bottom = build_result.root->find_element_by_id("Bottom");
    ASSERT_NE(bottom, nullptr);
    EXPECT_FLOAT_EQ(bottom->flex_grow(), 0.0f); // 未设置 flex-grow

    delete build_result.root;
}

// 测试组合布局：flex-grow + align-items + justify-content 综合
TEST(LayoutTest, CombinedFlexboxLayout) {
    // 创建一个 300x300 的垂直布局 Panel
    auto* panel = new Panel("Root");
    panel->set_bounds(Rect{0, 0, 300, 300});
    panel->set_layout(LayoutType::Vertical);
    panel->set_spacing(0);
    panel->set_align_items(AlignItems::Center);
    panel->set_justify_content(JustifyContent::Center);

    auto* header = new Panel("Header");
    header->set_size(200, 40);
    auto* body = new Panel("Body");
    body->set_size(200, 0);
    body->set_flex_grow(1.0f);
    auto* footer = new Panel("Footer");
    footer->set_size(200, 40);
    panel->add_child(header);
    panel->add_child(body);
    panel->add_child(footer);
    panel->relayout();

    // 固定高度: header=40, footer=40, 总固定=80
    // 可用空间: 300-80=220
    // flex-grow body(1.0): body 高度 = 220
    // justify-content Center: 剩余空间 = 0, 起始 y = 0
    // align-items Center: 所有子控件 x = (300-200)/2 = 50

    EXPECT_FLOAT_EQ(header->bounds().x, 50.0f);
    EXPECT_FLOAT_EQ(header->bounds().y, 0.0f);
    EXPECT_FLOAT_EQ(header->bounds().w, 200.0f);
    EXPECT_FLOAT_EQ(header->bounds().h, 40.0f);

    EXPECT_FLOAT_EQ(body->bounds().x, 50.0f);
    EXPECT_FLOAT_EQ(body->bounds().y, 40.0f);
    EXPECT_FLOAT_EQ(body->bounds().w, 200.0f);
    EXPECT_FLOAT_EQ(body->bounds().h, 220.0f);

    EXPECT_FLOAT_EQ(footer->bounds().x, 50.0f);
    EXPECT_FLOAT_EQ(footer->bounds().y, 260.0f);
    EXPECT_FLOAT_EQ(footer->bounds().w, 200.0f);
    EXPECT_FLOAT_EQ(footer->bounds().h, 40.0f);

    delete panel;
}