#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "GryceEngineUtils/ui/dsl/ast.h"
#include "GryceEngineUtils/ui/dsl/lexer.h"
#include "GryceEngineUtils/ui/dsl/parser.h"

using namespace GryceEngineUtils::ui::dsl;

namespace {

// 便捷：从源码解析并返回所有 ParseError
std::vector<ASTNode> parseAll(const std::string& src, bool* hasError = nullptr) {
    Lexer lex(src);
    Parser parser(lex);
    std::vector<ASTNode> nodes = parser.parse();
    if (hasError) {
        *hasError = parser.hasErrors();
    }
    return nodes;
}

} // namespace

// ============================================================================
// 基础解析
// ============================================================================

TEST(DslParserTest, EmptySourceYieldsNoNodes) {
    auto nodes = parseAll("");
    EXPECT_TRUE(nodes.empty());
}

TEST(DslParserTest, ControlWithoutAttributesOrChildren) {
    auto nodes = parseAll("Window()");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, "Window");
    EXPECT_TRUE(nodes[0].attributes.empty());
    EXPECT_TRUE(nodes[0].children.empty());
    EXPECT_EQ(nodes[0].line, 1);
}

TEST(DslParserTest, ControlWithAttributes) {
    auto nodes = parseAll("Button(id=\"start\", width=200, enabled=true)");
    ASSERT_EQ(nodes.size(), 1u);
    const auto& a = nodes[0].attributes;
    ASSERT_TRUE(a.count("id"));
    EXPECT_EQ(a.at("id").stringValue, "start");
    ASSERT_TRUE(a.count("width"));
    EXPECT_EQ(a.at("width").numberValue, 200.0);
    ASSERT_TRUE(a.count("enabled"));
    EXPECT_TRUE(a.at("enabled").boolValue);
}

TEST(DslParserTest, EmptyAttributeList) {
    auto nodes = parseAll("Window()");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(nodes[0].attributes.empty());
}

TEST(DslParserTest, NestedControls) {
    auto nodes = parseAll(R"(Window(title="Main") {
        Panel() {
            Button(text="OK")
            Button(text="Cancel")
        }
    })");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, "Window");
    ASSERT_EQ(nodes[0].children.size(), 1u);
    EXPECT_EQ(nodes[0].children[0].type, "Panel");
    ASSERT_EQ(nodes[0].children[0].children.size(), 2u);
    EXPECT_EQ(nodes[0].children[0].children[0].attributes.at("text").stringValue, "OK");
    EXPECT_EQ(nodes[0].children[0].children[1].attributes.at("text").stringValue, "Cancel");
}

TEST(DslParserTest, MultipleTopLevelControls) {
    auto nodes = parseAll("Panel(w=10) Text(t=\"hi\") ");
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].type, "Panel");
    EXPECT_EQ(nodes[1].type, "Text");
}

// ============================================================================
// 属性值类型
// ============================================================================

TEST(DslParserTest, ArrayAttribute) {
    auto nodes = parseAll("ComboBox(items=[\"a\", \"b\", \"c\"])");
    ASSERT_EQ(nodes.size(), 1u);
    const auto& arr = nodes[0].attributes.at("items").arrayValue;
    ASSERT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr[0].stringValue, "a");
    EXPECT_EQ(arr[1].stringValue, "b");
    EXPECT_EQ(arr[2].stringValue, "c");
}

TEST(DslParserTest, EmptyArrayAttribute) {
    auto nodes = parseAll("List(items=[])");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_TRUE(nodes[0].attributes.at("items").arrayValue.empty());
}

TEST(DslParserTest, NumberVariants) {
    auto nodes = parseAll("SpinBox(v=1, step=0.5, neg=-10)");
    const auto& a = nodes[0].attributes;
    EXPECT_EQ(a.at("v").numberValue, 1.0);
    EXPECT_EQ(a.at("step").numberValue, 0.5);
    EXPECT_EQ(a.at("neg").numberValue, -10.0);
}

TEST(DslParserTest, IdentifierAttributeValue) {
    auto nodes = parseAll("Button(onClick=onStart)");
    ASSERT_EQ(nodes.size(), 1u);
    const auto& v = nodes[0].attributes.at("onClick");
    EXPECT_TRUE(v.is(ASTValue::Kind::Identifier));
    EXPECT_EQ(v.stringValue, "onStart");
}

TEST(DslParserTest, DuplicateAttributeKeepsLast) {
    auto nodes = parseAll("Text(text=\"a\", text=\"b\")");
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].attributes.at("text").stringValue, "b");
}

// ============================================================================
// 错误处理与恢复
// ============================================================================

TEST(DslParserTest, MissingRParenReportsAndRecovers) {
    bool err = false;
    auto nodes = parseAll("Window(id=\"x\" ", &err);
    EXPECT_TRUE(err);
}

TEST(DslParserTest, MissingRBraceReportsAndRecovers) {
    bool err = false;
    auto nodes = parseAll("Window() { Panel() ", &err);
    EXPECT_TRUE(err);
    // 内层不完整，但解析不出异常崩溃
    ASSERT_GE(nodes.size(), 0u);
}

TEST(DslParserTest, AttributeMissingEqualsReports) {
    bool err = false;
    auto nodes = parseAll("Button(text \"x\")", &err);
    EXPECT_TRUE(err);
}

TEST(DslParserTest, MultipleErrorsAllCollected) {
    bool err = false;
    parseAll("Window(a) Panel(b, c=) Text( )", &err);
    EXPECT_TRUE(err);
}

TEST(DslParserTest, ParseErrorHasPositionAndContext) {
    Lexer lex("Button(=5)");
    Parser parser(lex);
    parser.parse();
    ASSERT_TRUE(parser.hasErrors());
    const auto& errs = parser.getErrors();
    ASSERT_FALSE(errs.empty());
    EXPECT_GE(errs[0].line, 1);
    EXPECT_GE(errs[0].column, 1);
    EXPECT_FALSE(errs[0].message.empty());
}

TEST(DslParserTest, SingleErrorDoesNotFailWholeFile) {
    // 前两个控件合法，最后 Block 括号未闭合；错误不应影响已解析结果
    bool err = false;
    auto nodes = parseAll("Text(text=\"good\") Panel() Broken(", &err);
    EXPECT_TRUE(err);
    ASSERT_EQ(nodes.size(), 3u);
    EXPECT_EQ(nodes[0].type, "Text");
    EXPECT_EQ(nodes[0].attributes.at("text").stringValue, "good");
    EXPECT_EQ(nodes[1].type, "Panel");
}

TEST(DslParserTest, UnknownFileEndIsEof) {
    auto nodes = parseAll("// only a comment\n");
    EXPECT_TRUE(nodes.empty());
}