#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "GryceEngineUtils/ui/button.h"
#include "GryceEngineUtils/ui/checkbox.h"
#include "GryceEngineUtils/ui/factory.h"
#include "GryceEngineUtils/ui/label.h"
#include "GryceEngineUtils/ui/panel.h"
#include "GryceEngineUtils/ui/slider.h"
#include "GryceEngineUtils/ui/uif_builder.h"
#include "GryceEngineUtils/ui/widget.h"

using namespace GryceEngineUtils::ui;

namespace {
// 便捷：从 DSL 源码构建控件树（全流程）
UIBuildResult buildDsl(const std::string& src,
                       std::vector<dsl::SemanticError>* se = nullptr) {
    return UIWidgetBuilder::build_from_dsl_source(src, se);
}
} // namespace

// ============================================================================
// 基础转换
// ============================================================================

TEST(DslUiBuilderTest, WindowMapsToPanelRoot) {
    auto r = buildDsl("Window(title=\"Main\")");
    ASSERT_TRUE(r.success) << r.error_message;
    ASSERT_NE(r.root, nullptr);
    // Window -> Panel（根容器）
    EXPECT_NE(dynamic_cast<Panel*>(r.root), nullptr);
    delete r.root;
}

TEST(DslUiBuilderTest, TextMapsToLabel) {
    auto r = buildDsl("Text(text=\"Hello\")");
    ASSERT_TRUE(r.success) << r.error_message;
    ASSERT_NE(r.root, nullptr);
    auto* label = dynamic_cast<Label*>(r.root);
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->text(), "Hello");
    delete r.root;
}

TEST(DslUiBuilderTest, ButtonWithProps) {
    auto r = buildDsl("Button(text=\"OK\", width=80)");
    ASSERT_TRUE(r.success) << r.error_message;
    ASSERT_NE(r.root, nullptr);
    auto* btn = dynamic_cast<Button*>(r.root);
    ASSERT_NE(btn, nullptr);
    EXPECT_EQ(btn->text(), "OK");
    delete r.root;
}

TEST(DslUiBuilderTest, SliderNumberConversion) {
    auto r = buildDsl("Slider(min=0, max=100, value=42)");
    ASSERT_TRUE(r.success) << r.error_message;
    ASSERT_NE(r.root, nullptr);
    EXPECT_NE(dynamic_cast<Slider*>(r.root), nullptr);
    delete r.root;
}

TEST(DslUiBuilderTest, CheckBoxBooleanAndFloatNumber) {
    auto r = buildDsl("CheckBox(text=\"on\", checked=true, width=12.5)");
    ASSERT_TRUE(r.success) << r.error_message;
    ASSERT_NE(r.root, nullptr);
    EXPECT_NE(dynamic_cast<CheckBox*>(r.root), nullptr);
    delete r.root;
}

// ============================================================================
// 嵌套父子关系
// ============================================================================

TEST(DslUiBuilderTest, NestedChildrenParentage) {
    auto r = buildDsl("Window(title=\"Main\") {\n"
                      "  Panel() { Button(text=\"A\") Button(text=\"B\") }\n"
                      "}");
    ASSERT_TRUE(r.success) << r.error_message;
    ASSERT_NE(r.root, nullptr);
    auto* winPanel = dynamic_cast<Panel*>(r.root);
    ASSERT_NE(winPanel, nullptr);
    ASSERT_EQ(winPanel->children().size(), 1u);
    auto* inner = dynamic_cast<Panel*>(winPanel->children()[0]);
    ASSERT_NE(inner, nullptr);
    EXPECT_EQ(inner->parent(), winPanel);
    ASSERT_EQ(inner->children().size(), 2u);
    EXPECT_EQ(inner->children()[0]->parent(), inner);
    EXPECT_EQ(inner->children()[1]->parent(), inner);
    delete r.root;
}

// ============================================================================
// 类型转换正确性
// ============================================================================

TEST(DslUiBuilderTest, IdentifierAsString) {
    // identifier 值（如 onClick 回调名）原样注入；成功创建即可
    auto r = buildDsl("Button(text=\"Go\", onClick=onStart)");
    ASSERT_TRUE(r.success) << r.error_message;
    ASSERT_NE(r.root, nullptr);
    delete r.root;
}

TEST(DslUiBuilderTest, ListDefaultVerticalLayout) {
    auto r = buildDsl("List() { Button(text=\"1\") Button(text=\"2\") }");
    ASSERT_TRUE(r.success) << r.error_message;
    ASSERT_NE(r.root, nullptr);
    auto* panel = dynamic_cast<Panel*>(r.root);
    ASSERT_NE(panel, nullptr);
    EXPECT_EQ(panel->layout_type(), LayoutType::Vertical);
    delete r.root;
}

// ============================================================================
// 未知类型 / 防御性检查
// ============================================================================

TEST(DslUiBuilderTest, UnknownTypeReturnsNullPtrRoot) {
    auto r = buildDsl("Widnow(title=\"Main\")");
    // 语义分析拦截未知控件，构建失败
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.root, nullptr);
}

TEST(DslUiBuilderTest, SyntaxErrorReturnsFailure) {
    auto r = buildDsl("Window(title=\"Main\") {");
    EXPECT_FALSE(r.success);
    EXPECT_EQ(r.root, nullptr);
}

TEST(DslUiBuilderTest, SemanticErrorSurfaced) {
    std::vector<dsl::SemanticError> se;
    auto r = buildDsl("Button(width=100)", &se);   // Button 缺必需属性 text
    EXPECT_FALSE(r.success);
    EXPECT_FALSE(se.empty());
    delete r.root; // nullptr，安全
}

TEST(DslUiBuilderTest, WidgetCountReported) {
    auto r = buildDsl("Window() { Button(text=\"A\") Text(text=\"B\") }");
    ASSERT_TRUE(r.success) << r.error_message;
    EXPECT_EQ(r.widget_count, 3);   // Window + Button + Text
    delete r.root;
}

// ============================================================================
// 内存安全：反复加载/卸载不产生悬挂指针（冒烟）
// ============================================================================

TEST(DslUiBuilderTest, RepeatedLoadUnload) {
    for (int i = 0; i < 50; ++i) {
        auto r = buildDsl("Window(title=\"Loop\") { "
                          "  Button(text=\"x\")\n  Slider(min=0, max=10)\n}");
        ASSERT_TRUE(r.success) << r.error_message;
        ASSERT_NE(r.root, nullptr);
        delete r.root;
    }
    SUCCEED();
}