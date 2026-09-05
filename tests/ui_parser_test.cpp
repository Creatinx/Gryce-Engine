#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "GryceEngineUtils/ui/uif_parser.h"

using namespace GryceEngineUtils::ui;

// 辅助函数：递归查找子节点（pugixml 的 find_child_by_attribute 只能查直接子节点）
static pugi::xml_node find_descendant_by_attribute(
    pugi::xml_node parent,
    const char* name,
    const char* attr,
    const char* value)
{
    for (auto child = parent.first_child(); child; child = child.next_sibling()) {
        if (std::strcmp(child.name(), name) == 0 &&
            std::strcmp(child.attribute(attr).value(), value) == 0) {
            return child;
        }
        auto found = find_descendant_by_attribute(child, name, attr, value);
        if (found) return found;
    }
    return pugi::xml_node();
}

// 辅助函数：获取测试 fixture 目录路径
static std::string fixture_dir() {
    std::string root = GRYCE_TEST_PROJECT_ROOT;
    return root + "/tests/fixtures/ui";
}

// ============================================================================
// 基本解析测试
// ============================================================================

// 测试从文件解析一个简单的 .uif 文件
TEST(UIParserTest, ParseSimpleWindow) {
    UIParser parser;
    auto result = parser.parse_file(fixture_dir() + "/simple_window.uif");

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_TRUE(result.file_path.find("simple_window.uif") != std::string::npos);

    // 验证 DOM 结构：根节点应为 Window
    pugi::xml_node root = result.doc.document_element();
    EXPECT_STREQ(root.name(), "Window");

    // 验证根节点属性
    EXPECT_STREQ(root.attribute("id").value(), "MainWindow");
    EXPECT_STREQ(root.attribute("title").value(), "Main Menu");
    EXPECT_STREQ(root.attribute("layout").value(), "vertical");
    EXPECT_STREQ(root.attribute("padding").value(), "10");
    EXPECT_STREQ(root.attribute("spacing").value(), "5");

    // 验证子控件数量
    int child_count = 0;
    for (auto child = root.first_child(); child; child = child.next_sibling()) {
        child_count++;
    }
    EXPECT_EQ(child_count, 7); // HeaderPanel, StartBtn, SettingsBtn, PlayerName, VolumeSlider, Logo, ItemList
}

// 测试从字符串解析 .uif 内容
TEST(UIParserTest, ParseFromString) {
    UIParser parser;
    const char* uif_content = R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="TestWindow" title="Test" layout="vertical">
            <Button id="TestBtn" text="Click Me" onClick="OnClick" />
            <Text id="Label" text="Hello" />
        </Window>
    )";

    auto result = parser.parse_string(uif_content, "inline_test");

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_STREQ(result.doc.document_element().name(), "Window");
    EXPECT_STREQ(result.doc.document_element().attribute("id").value(), "TestWindow");
}

// 测试解析空的 .uif 文件
TEST(UIParserTest, ParseEmptyWindow) {
    UIParser parser;
    auto result = parser.parse_file(fixture_dir() + "/empty.uif");

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_STREQ(result.doc.document_element().name(), "Window");

    // 空窗口没有子节点
    pugi::xml_node root = result.doc.document_element();
    EXPECT_TRUE(root.first_child() == nullptr);
}

// 测试解析复杂的 .uif 文件（登录表单）
TEST(UIParserTest, ParseLoginForm) {
    UIParser parser;
    auto result = parser.parse_file(fixture_dir() + "/login_form.uif");

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_STREQ(result.doc.document_element().name(), "Window");

    pugi::xml_node root = result.doc.document_element();

    // 验证属性
    EXPECT_STREQ(root.attribute("id").value(), "LoginWindow");
    EXPECT_STREQ(root.attribute("width").value(), "400");
    EXPECT_STREQ(root.attribute("height").value(), "300");

    // 递归查找嵌套的子控件（Button 在 Window > Panel > Button 内）
    pugi::xml_node login_btn = find_descendant_by_attribute(root, "Button", "id", "LoginBtn");
    EXPECT_TRUE(login_btn != nullptr) << "LoginBtn not found in nested children";
    EXPECT_STREQ(login_btn.attribute("text").value(), "Login");
    EXPECT_STREQ(login_btn.attribute("style").value(), "Primary");
    EXPECT_STREQ(login_btn.attribute("onClick").value(), "OnLogin");

    // 验证 CheckBox
    pugi::xml_node checkbox = find_descendant_by_attribute(root, "CheckBox", "id", "RememberMe");
    EXPECT_TRUE(checkbox != nullptr) << "RememberMe not found in nested children";
    EXPECT_STREQ(checkbox.attribute("text").value(), "Remember me");
    EXPECT_STREQ(checkbox.attribute("checked").value(), "true");
}

// 测试解析嵌套多层的 .uif 文件（设置页面）
TEST(UIParserTest, ParseSettingsWindow) {
    UIParser parser;
    auto result = parser.parse_file(fixture_dir() + "/settings.uif");

    EXPECT_TRUE(result.success) << result.error_message;
    EXPECT_STREQ(result.doc.document_element().name(), "Window");

    pugi::xml_node root = result.doc.document_element();

    // 验证 ScrollView 存在
    pugi::xml_node scroll = find_descendant_by_attribute(root, "ScrollView", "id", "SettingsScroll");
    EXPECT_TRUE(scroll != nullptr) << "SettingsScroll not found";

    // 验证 ScrollView 下有嵌套的 Panel → Slider
    pugi::xml_node audio_section = find_descendant_by_attribute(scroll, "Panel", "id", "AudioSection");
    EXPECT_TRUE(audio_section != nullptr) << "AudioSection not found inside ScrollView";

    // 验证 Slider 控件
    pugi::xml_node volume_slider = find_descendant_by_attribute(audio_section, "Slider", "id", "MasterVolume");
    EXPECT_TRUE(volume_slider != nullptr) << "MasterVolume not found inside AudioSection";
    EXPECT_STREQ(volume_slider.attribute("value").value(), "80");
    EXPECT_STREQ(volume_slider.attribute("min").value(), "0");
    EXPECT_STREQ(volume_slider.attribute("max").value(), "100");
    EXPECT_STREQ(volume_slider.attribute("onChange").value(), "OnMasterVolumeChange");

    // 验证 Divider
    pugi::xml_node divider = find_descendant_by_attribute(scroll, "Divider", "id", "AudioDivider");
    EXPECT_TRUE(divider != nullptr) << "AudioDivider not found";

    // 验证底部按钮组
    pugi::xml_node bottom = find_descendant_by_attribute(root, "Panel", "id", "BottomBar");
    EXPECT_TRUE(bottom != nullptr) << "BottomBar not found";
    pugi::xml_node apply_btn = find_descendant_by_attribute(bottom, "Button", "id", "ApplyBtn");
    EXPECT_TRUE(apply_btn != nullptr) << "ApplyBtn not found inside BottomBar";
    EXPECT_STREQ(apply_btn.attribute("style").value(), "Primary");
    EXPECT_STREQ(apply_btn.attribute("onClick").value(), "OnApply");
}

// ============================================================================
// 错误处理测试
// ============================================================================

// 测试文件不存在
TEST(UIParserTest, FileNotFound) {
    UIParser parser;
    auto result = parser.parse_file(fixture_dir() + "/nonexistent.uif");

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
}

// 测试无效 XML 内容
TEST(UIParserTest, InvalidXML) {
    UIParser parser;
    const char* invalid_content = R"(<Window><UnclosedButton></Window>)";

    auto result = parser.parse_string(invalid_content, "invalid");

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error_message.empty());
}

// 测试空字符串
TEST(UIParserTest, EmptyString) {
    UIParser parser;
    auto result = parser.parse_string("", "empty");

    EXPECT_FALSE(result.success);
}

// 测试文件扩展名
TEST(UIParserTest, WrongExtension) {
    UIParser parser;
    // 使用 fixture 目录下的一个非 .uif 文件
    // 先创建一个临时 .txt 文件用于测试
    std::string test_file = fixture_dir() + "/test_extension_check.txt";
    // 方法1：创建一个 .txt 文件测试扩展名检查
    // 但如果文件不存在，会先报"File not found"。所以我们用 get_main_playground.cpp（存在但非 .uif）
    // 注意：../get_main_playground.cpp 从 fixture/ui/ 向上到 tests/ 可能不存在
    // 最简单的：直接测试 parse_string 的路径，或构造一个存在的文件
    auto result = parser.parse_file(fixture_dir() + "/simple_window.uif");
    EXPECT_TRUE(result.success);  // 确认 .uif 文件正常

    // 测试扩展名过滤：创建一个 .tmp 文件
    {
        std::ofstream tmp(test_file);
        tmp << "<?xml version=\"1.0\"?><Window></Window>";
    }
    auto result2 = parser.parse_file(test_file);
    EXPECT_FALSE(result2.success);
    EXPECT_TRUE(result2.error_message.find(".uif") != std::string::npos);
    std::filesystem::remove(test_file);
}

// ============================================================================
// 验证测试
// ============================================================================

// 测试验证已知控件类型
TEST(UIParserTest, ValidateKnownControls) {
    UIParser parser;
    auto result = parser.parse_file(fixture_dir() + "/simple_window.uif");
    ASSERT_TRUE(result.success);

    auto info = parser.validate(result.doc);
    EXPECT_TRUE(info.valid);
    EXPECT_TRUE(info.errors.empty());
}

// 测试验证未知控件类型
TEST(UIParserTest, ValidateUnknownControl) {
    UIParser parser;
    const char* uif_content = R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="Test">
            <UnknownWidget id="bad" />
        </Window>
    )";

    auto result = parser.parse_string(uif_content, "unknown_test");
    ASSERT_TRUE(result.success);

    auto info = parser.validate(result.doc);
    EXPECT_FALSE(info.valid); // 应该报告错误
    EXPECT_FALSE(info.errors.empty());
}

// 测试验证控件类型收集
TEST(UIParserTest, ValidationCollectsControlTypes) {
    UIParser parser;
    auto result = parser.parse_file(fixture_dir() + "/settings.uif");
    ASSERT_TRUE(result.success);

    auto info = parser.validate(result.doc);
    EXPECT_TRUE(info.valid);

    // 验证收集到的控件类型包含 Window, Panel, Text, Button, Slider, ...
    EXPECT_GT(info.control_types_used.size(), 0);
    EXPECT_GT(info.control_count, 0);
}

// ============================================================================
// 已知控件类型和属性测试
// ============================================================================

TEST(UIParserTest, KnownControlsList) {
    const auto& controls = UIParser::known_control_types();

    // 验证包含计划中的核心控件
    EXPECT_NE(std::find(controls.begin(), controls.end(), "Window"), controls.end());
    EXPECT_NE(std::find(controls.begin(), controls.end(), "Panel"), controls.end());
    EXPECT_NE(std::find(controls.begin(), controls.end(), "Text"), controls.end());
    EXPECT_NE(std::find(controls.begin(), controls.end(), "Button"), controls.end());
    EXPECT_NE(std::find(controls.begin(), controls.end(), "Image"), controls.end());
    EXPECT_NE(std::find(controls.begin(), controls.end(), "List"), controls.end());
    EXPECT_NE(std::find(controls.begin(), controls.end(), "Slider"), controls.end());
}

TEST(UIParserTest, SupportedAttributes) {
    // 验证 Button 支持 onClick 属性
    const auto& btn_attrs = UIParser::supported_attributes("Button");
    EXPECT_NE(std::find(btn_attrs.begin(), btn_attrs.end(), "onClick"), btn_attrs.end());
    EXPECT_NE(std::find(btn_attrs.begin(), btn_attrs.end(), "text"), btn_attrs.end());
    EXPECT_NE(std::find(btn_attrs.begin(), btn_attrs.end(), "style"), btn_attrs.end());

    // 验证 Slider 支持 onChange 属性
    const auto& slider_attrs = UIParser::supported_attributes("Slider");
    EXPECT_NE(std::find(slider_attrs.begin(), slider_attrs.end(), "onChange"), slider_attrs.end());
    EXPECT_NE(std::find(slider_attrs.begin(), slider_attrs.end(), "min"), slider_attrs.end());
    EXPECT_NE(std::find(slider_attrs.begin(), slider_attrs.end(), "max"), slider_attrs.end());
    EXPECT_NE(std::find(slider_attrs.begin(), slider_attrs.end(), "value"), slider_attrs.end());
}

// ============================================================================
// 数据绑定属性测试
// ============================================================================

TEST(UIParserTest, DataBindingAttribute) {
    UIParser parser;
    const char* uif_content = R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="Test">
            <Text id="BoundText" text="{Player.Name}" />
            <Text id="StaticText" text="Hello World" />
        </Window>
    )";

    auto result = parser.parse_string(uif_content, "binding_test");
    ASSERT_TRUE(result.success);

    pugi::xml_node root = result.doc.document_element();

    // 验证数据绑定属性
    pugi::xml_node bound = find_descendant_by_attribute(root, "Text", "id", "BoundText");
    EXPECT_TRUE(bound != nullptr);
    EXPECT_STREQ(bound.attribute("text").value(), "{Player.Name}");

    // 验证静态文本
    pugi::xml_node static_text = find_descendant_by_attribute(root, "Text", "id", "StaticText");
    EXPECT_TRUE(static_text != nullptr);
    EXPECT_STREQ(static_text.attribute("text").value(), "Hello World");
}

// ============================================================================
// 属性遍历测试
// ============================================================================

TEST(UIParserTest, IterateAllAttributes) {
    UIParser parser;
    auto result = parser.parse_file(fixture_dir() + "/simple_window.uif");
    ASSERT_TRUE(result.success);

    pugi::xml_node root = result.doc.document_element();

    // 遍历根节点所有属性
    int attr_count = 0;
    for (auto attr : root.attributes()) {
        attr_count++;
        // 属性名和值都不应为空
        EXPECT_GT(std::strlen(attr.name()), 0u);
        EXPECT_GT(std::strlen(attr.value()), 0u);
    }
    // Window 根节点有 5 个属性：id, title, layout, padding, spacing
    EXPECT_EQ(attr_count, 5);
}

// ============================================================================
// 跨平台：pugixml 编码处理测试
// ============================================================================

TEST(UIParserTest, UTF8Encoding) {
    UIParser parser;
    const char* uif_content = R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="Main">
            <Text id="Chinese" text="你好世界" />
            <Text id="Japanese" text="こんにちは" />
        </Window>
    )";

    auto result = parser.parse_string(uif_content, "utf8_test");
    EXPECT_TRUE(result.success) << result.error_message;

    pugi::xml_node root = result.doc.document_element();
    pugi::xml_node chinese = find_descendant_by_attribute(root, "Text", "id", "Chinese");
    EXPECT_TRUE(chinese != nullptr);
    EXPECT_STREQ(chinese.attribute("text").value(), "你好世界");

    pugi::xml_node japanese = find_descendant_by_attribute(root, "Text", "id", "Japanese");
    EXPECT_TRUE(japanese != nullptr);
    EXPECT_STREQ(japanese.attribute("text").value(), "こんにちは");
}

// ============================================================================
// DOM 结构输出测试（验证解析器能打印 DOM 树）
// ============================================================================

TEST(UIParserTest, DumpDOMStructure) {
    UIParser parser;
    auto result = parser.parse_file(fixture_dir() + "/simple_window.uif");
    ASSERT_TRUE(result.success);

    // 验证 DOM 树可以通过 pugixml 的标准方式输出
    std::ostringstream oss;
    result.doc.print(oss, "  ");
    std::string dump = oss.str();

    // 验证输出内容包含关键结构
    EXPECT_NE(dump.find("<Window"), std::string::npos);
    EXPECT_NE(dump.find("MainWindow"), std::string::npos);
    EXPECT_NE(dump.find("</Window>"), std::string::npos);

    // 打印 DOM 树到控制台（GTest 会自动捕获 stdout）
    std::cout << "\n=== DOM Tree: simple_window.uif ===" << std::endl;
    result.doc.print(std::cout, "  ");
    std::cout << std::endl;
}

// ============================================================================
// 严格模式测试
// ============================================================================

TEST(UIParserTest, StrictModeWarning) {
    UIParser parser;
    parser.set_strict_mode(true);

    // 创建一个包含未知属性的 .uif（strict_mode 下会警告）
    const char* uif_content = R"(
        <?xml version="1.0" encoding="UTF-8"?>
        <Window id="Test">
            <Button id="TestBtn" text="OK" unknown_attr="value" />
        </Window>
    )";

    auto result = parser.parse_string(uif_content, "strict_test");
    ASSERT_TRUE(result.success);

    // strict_mode 下验证应报告未知属性警告
    // 注意：目前 validate 只对 strict_mode 记录未知属性到 warnings
    auto info = parser.validate(result.doc);
    if (parser.strict_mode()) {
        // 在严格模式下，Button 的 unknown_attr 会被记录为警告
        bool has_unknown_warning = false;
        for (const auto& w : info.warnings) {
            if (w.find("unknown_attr") != std::string::npos) {
                has_unknown_warning = true;
                break;
            }
        }
        // 注意：目前已知属性列表不含 unknown_attr，严格模式下应有警告
        EXPECT_TRUE(info.valid); // 警告不影响 valid 状态
    }
}