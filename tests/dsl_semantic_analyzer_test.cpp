#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "GryceEngineUtils/ui/dsl/ast.h"
#include "GryceEngineUtils/ui/dsl/ast_optimizer.h"
#include "GryceEngineUtils/ui/dsl/lexer.h"
#include "GryceEngineUtils/ui/dsl/parser.h"
#include "GryceEngineUtils/ui/dsl/semantic_analyzer.h"

using namespace GryceEngineUtils::ui::dsl;

namespace {
// 便捷：从源码解析并返回所有 AST 节点
std::vector<ASTNode> parseAll(const std::string& src) {
    Lexer lex(src);
    Parser parser(lex);
    return parser.parse();
}
} // namespace

// ============================================================================
// 语义分析（SemanticAnalyzer）测试
// ============================================================================

TEST(SemanticAnalyzerTest, ValidWindowPasses) {
    auto nodes = parseAll("Window(title=\"Main\") {\n"
                          "  Button(text=\"OK\")\n"
                          "  Text(text=\"Hello\")\n"
                          "}");
    SemanticAnalyzer sa;
    EXPECT_TRUE(sa.analyze(nodes)) << (sa.errors().empty() ? "" : sa.errors()[0].toString());
    EXPECT_FALSE(sa.hasErrors());
}

TEST(SemanticAnalyzerTest, UnknownControlReportedWithSuggestion) {
    auto nodes = parseAll("Widnow(title=\"Main\")");
    SemanticAnalyzer sa;
    EXPECT_FALSE(sa.analyze(nodes));
    ASSERT_EQ(sa.errors().size(), 1u);
    EXPECT_EQ(sa.errors()[0].message, "Unknown control 'Widnow'.");
    EXPECT_TRUE(sa.errors()[0].hint.find("Window") != std::string::npos);
}

TEST(SemanticAnalyzerTest, UnknownControlNoSuggestion) {
    auto nodes = parseAll("FooBar()");
    SemanticAnalyzer sa;
    EXPECT_FALSE(sa.analyze(nodes));
    ASSERT_EQ(sa.errors().size(), 1u);
    EXPECT_EQ(sa.errors()[0].message, "Unknown control 'FooBar'.");
}

TEST(SemanticAnalyzerTest, UnknownAttributeReported) {
    auto nodes = parseAll("Button(text=\"OK\", colorr=\"#fff\")");
    SemanticAnalyzer sa;
    EXPECT_FALSE(sa.analyze(nodes));
    bool found = false;
    for (const auto& e : sa.errors()) {
        if (e.message.find("colorr") != std::string::npos &&
            e.message.find("Button") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(SemanticAnalyzerTest, MissingRequiredAttribute) {
    auto nodes = parseAll("Button()");
    SemanticAnalyzer sa;
    EXPECT_FALSE(sa.analyze(nodes));
    bool found = false;
    for (const auto& e : sa.errors()) {
        if (e.message.find("missing required attribute 'text'") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(SemanticAnalyzerTest, InvalidTypeForWidth) {
    // width 期望 number
    auto nodes = parseAll("Button(text=\"OK\", width=\"narrow\")");
    SemanticAnalyzer sa;
    EXPECT_FALSE(sa.analyze(nodes));
    bool found = false;
    for (const auto& e : sa.errors()) {
        if (e.message.find("expects number") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(SemanticAnalyzerTest, InvalidTypeForChecked) {
    auto nodes = parseAll("CheckBox(text=\"on\", checked=42)");
    SemanticAnalyzer sa;
    EXPECT_FALSE(sa.analyze(nodes));
    bool found = false;
    for (const auto& e : sa.errors()) {
        if (e.message.find("expects boolean") != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);
}

TEST(SemanticAnalyzerTest, NestedErrorsAllCollected) {
    // 两个错误控件，应全部收集而非中断
    auto nodes = parseAll("Widnow() { Bar() Button() }");
    SemanticAnalyzer sa;
    EXPECT_FALSE(sa.analyze(nodes));
    int unknown = 0;
    for (const auto& e : sa.errors()) {
        if (e.message.find("Unknown control") != std::string::npos) {
            ++unknown;
        }
    }
    EXPECT_GE(unknown, 2);
}

TEST(SemanticAnalyzerTest, BuiltinSchemasPresent) {
    const auto& schemas = SemanticAnalyzer::builtin_schemas();
    EXPECT_EQ(schemas.size(), 15u);
    EXPECT_TRUE(SemanticAnalyzer::is_known_control("Window"));
    EXPECT_TRUE(SemanticAnalyzer::is_known_control("SpinBox"));
    EXPECT_FALSE(SemanticAnalyzer::is_known_control("Graph3D"));
}

// ============================================================================
// AST 优化（ASTOptimizer）测试
// ============================================================================

TEST(ASTOptimizerTest, UnknownControlNotOptimized) {
    auto nodes = parseAll("Window(title=\"Main\") { Button(text=\"OK\") }");
    ASTOptimizer opt;
    opt.optimize(nodes);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].type, "Window");
    ASSERT_EQ(nodes[0].children.size(), 1u);
    EXPECT_EQ(nodes[0].children[0].type, "Button");
}

TEST(ASTOptimizerTest, EmptyNodesRemoved) {
    auto nodes = parseAll("Window(title=\"Main\") {\n"
                          "  Panel()\n"           // 空控件（无属性无子节点）
                          "  Button(text=\"OK\")\n"
                          "  Divider()\n"
                          "}");
    ASTOptimizer opt;
    opt.optimize(nodes);
    ASSERT_EQ(nodes.size(), 1u);
    auto& children = nodes[0].children;
    ASSERT_EQ(children.size(), 1u);
    EXPECT_EQ(children[0].type, "Button");
}

TEST(ASTOptimizerTest, EmptyTopLevelNodesRemoved) {
    auto nodes = parseAll("Window(title=\"Main\") Panel() Text(text=\"hi\")");
    EXPECT_EQ(nodes.size(), 3u);
    ASTOptimizer opt;
    opt.optimize(nodes);
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0].type, "Window");
    EXPECT_EQ(nodes[1].type, "Text");
}

TEST(ASTOptimizerTest, LastWinsForDuplicateAfterOptimize) {
    // Parser 已收敛重复键为最后一个；优化不改变该语义
    auto nodes = parseAll("Panel(width=10, width=99)");
    ASTOptimizer opt;
    opt.optimize(nodes);
    ASSERT_EQ(nodes.size(), 1u);
    EXPECT_EQ(nodes[0].attributes.at("width").numberValue, 99.0);
}

TEST(ASTOptimizerTest, StyleExpansionMergesInline) {
    ASTOptimizer opt;
    std::unordered_map<std::string, ASTValue> btnStyle;
    btnStyle["color"] = ASTValue::makeString("#ffffff");
    btnStyle["font-size"] = ASTValue::makeNumber(16.0);
    opt.register_style("primary", btnStyle);

    auto nodes = parseAll("Button(text=\"OK\", style=\"primary\", width=60)");
    opt.optimize(nodes);
    ASSERT_EQ(nodes.size(), 1u);
    auto& attrs = nodes[0].attributes;
    // 内联属性已展开
    EXPECT_TRUE(attrs.count("color"));
    EXPECT_TRUE(attrs.count("font-size"));
    // 不覆盖显式属性
    EXPECT_TRUE(attrs.count("width"));
    EXPECT_EQ(attrs["width"].numberValue, 60.0);
    // 样式引用属性被移除
    EXPECT_FALSE(attrs.count("style"));
}

TEST(ASTOptimizerTest, StyleExpansionRespectsExplicitOverride) {
    ASTOptimizer opt;
    std::unordered_map<std::string, ASTValue> s;
    s["width"] = ASTValue::makeNumber(10.0);
    opt.register_style("dim", s);

    auto nodes = parseAll("Button(text=\"OK\", class=\"dim\", width=500)");
    opt.optimize(nodes);
    ASSERT_EQ(nodes.size(), 1u);
    // 显式 width 500 优先于样式块 10
    EXPECT_EQ(nodes[0].attributes.at("width").numberValue, 500.0);
    EXPECT_FALSE(nodes[0].attributes.count("class"));
}

TEST(ASTOptimizerTest, UnknownStyleClassLeavesNode) {
    auto nodes = parseAll("Button(text=\"OK\", style=\"ghost\")");
    ASTOptimizer opt;
    opt.optimize(nodes);
    ASSERT_EQ(nodes.size(), 1u);
    // 未注册样式类：节点保留，样式引用属性保留（语义不受影响）
    EXPECT_EQ(nodes[0].type, "Button");
}

TEST(ASTOptimizerTest, SemanticsEquivalentAfterFullOptimize) {
    const std::string src =
        "Window(title=\"Main\", layout=\"vertical\") {\n"
        "  Panel()                    // empty, removed\n"
        "  Button(text=\"OK\", style=\"primary\")\n"
        "  Text(text=\"hello\")\n"
        "  Panel() { Slider(min=0, max=100, value=50) }\n"
        "}";

    ASTOptimizer opt;
    std::unordered_map<std::string, ASTValue> primary;
    primary["color"] = ASTValue::makeString("#fff");
    opt.register_style("primary", primary);

    auto nodes = parseAll(src);
    opt.optimize(nodes);

    ASSERT_EQ(nodes.size(), 1u);
    auto& kids = nodes[0].children;
    ASSERT_EQ(kids.size(), 3u);
    EXPECT_EQ(kids[0].type, "Button");
    EXPECT_EQ(kids[0].attributes.at("color").stringValue, "#fff");   // 样式展开
    EXPECT_EQ(kids[1].type, "Text");
    EXPECT_EQ(kids[2].type, "Panel");
    ASSERT_EQ(kids[2].children.size(), 1u);
    EXPECT_EQ(kids[2].children[0].type, "Slider");
}

// ============================================================================
// 性能基准
// ============================================================================

// 生成 n 个 Button 的源码
static std::string make_buttons_source(int n) {
    std::string s = "Window(title=\"Bench\") {\n";
    for (int i = 0; i < n; ++i) {
        s += "  Button(text=\"btn" + std::to_string(i) + "\", width=" + std::to_string(i) + ")\n";
    }
    s += "}\n";
    return s;
}

// 端到端：Lexer -> Parser -> Semantic -> Optimize
static double bench_full(const std::string& src) {
    auto t0 = std::chrono::steady_clock::now();
    auto nodes = parseAll(src);
    SemanticAnalyzer sa;
    sa.analyze(nodes);
    ASTOptimizer opt;
    opt.optimize(nodes);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

TEST(AstBenchmarkTest, Small10Controls) {
    double ms = bench_full(make_buttons_source(10));
    EXPECT_LT(ms, 0.1);
}

TEST(AstBenchmarkTest, Medium100Controls) {
    double ms = bench_full(make_buttons_source(100));
    EXPECT_LT(ms, 1.0);
}

TEST(AstBenchmarkTest, Large1000Controls) {
    double ms = bench_full(make_buttons_source(1000));
    EXPECT_LT(ms, 10.0);
}