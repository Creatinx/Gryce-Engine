#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "GryceEngineUtils/ui/dsl/lexer.h"

using namespace GryceEngineUtils::ui::dsl;

namespace {

// 便捷：从源码扫描全部 token（不含 EOF）
std::vector<Token> scan(const std::string& src) {
    Lexer lex(src);
    std::vector<Token> out;
    for (;;) {
        Token t = lex.nextToken();
        if (t.is(TokenType::EndOfFile)) {
            return out;
        }
        out.push_back(t);
    }
}

} // namespace

// ============================================================================
// 基础分词
// ============================================================================

TEST(DslLexerTest, EmptyFileYieldsEof) {
    Lexer lex("");
    Token t = lex.nextToken();
    EXPECT_TRUE(t.is(TokenType::EndOfFile));
}

TEST(DslLexerTest, SingleIdentifier) {
    auto toks = scan("Window");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_TRUE(toks[0].is(TokenType::Identifier));
    EXPECT_EQ(toks[0].value, "Window");
    EXPECT_EQ(toks[0].line, 1);
    EXPECT_EQ(toks[0].column, 1);
}

TEST(DslLexerTest, IdentifierWithUnderscoreAndDigits) {
    auto toks = scan("onStart_2nd");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_TRUE(toks[0].is(TokenType::Identifier));
    EXPECT_EQ(toks[0].value, "onStart_2nd");
}

TEST(DslLexerTest, StringDoubleQuotes) {
    auto toks = scan("\"main menu\"");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_TRUE(toks[0].is(TokenType::String));
    EXPECT_EQ(toks[0].value, "main menu");
}

TEST(DslLexerTest, StringSingleQuotes) {
    auto toks = scan("'Hello'");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_TRUE(toks[0].is(TokenType::String));
    EXPECT_EQ(toks[0].value, "Hello");
}

TEST(DslLexerTest, StringEscapes) {
    auto toks = scan("\"a\\t\\nb\"");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_EQ(toks[0].value, "a\t\nb");
}

TEST(DslLexerTest, NumberIntegerFloatNegative) {
    {
        auto t = scan("800");
        ASSERT_EQ(t.size(), 1u);
        EXPECT_TRUE(t[0].is(TokenType::Number));
        EXPECT_EQ(t[0].value, "800");
    }
    {
        auto t = scan("0.5");
        ASSERT_EQ(t.size(), 1u);
        EXPECT_EQ(t[0].value, "0.5");
    }
    {
        auto t = scan("-10");
        ASSERT_EQ(t.size(), 1u);
        EXPECT_EQ(t[0].value, "-10");
    }
}

TEST(DslLexerTest, BooleanLiterals) {
    {
        auto t = scan("true");
        ASSERT_EQ(t.size(), 1u);
        EXPECT_TRUE(t[0].is(TokenType::Boolean));
        EXPECT_EQ(t[0].value, "true");
    }
    {
        auto t = scan("false");
        ASSERT_EQ(t.size(), 1u);
        EXPECT_TRUE(t[0].is(TokenType::Boolean));
    }
}

TEST(DslLexerTest, PunctuationOrder) {
    auto toks = scan("Window(id=\"x\", w=100) { Text }");
    ASSERT_GE(toks.size(), 6u);
    EXPECT_TRUE(toks[0].is(TokenType::Identifier)); // Window
    EXPECT_TRUE(toks[1].is(TokenType::LParen));
    EXPECT_TRUE(toks[2].is(TokenType::Identifier)); // id
    EXPECT_TRUE(toks[3].is(TokenType::Equals));
    EXPECT_TRUE(toks[4].is(TokenType::String));     // "x"
    EXPECT_TRUE(toks[5].is(TokenType::Comma));
}

// ============================================================================
// 注释
// ============================================================================

TEST(DslLexerTest, LineCommentSkipped) {
    auto toks = scan("// hello\nWindow");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_TRUE(toks[0].is(TokenType::Identifier));
    EXPECT_EQ(toks[0].value, "Window");
    // 注释结束后，Window 位于第 2 行
    EXPECT_EQ(toks[0].line, 2);
}

TEST(DslLexerTest, BlockCommentSkipped) {
    auto toks = scan("/* a\nb */Panel");
    ASSERT_EQ(toks.size(), 1u);
    EXPECT_TRUE(toks[0].is(TokenType::Identifier));
    EXPECT_EQ(toks[0].value, "Panel");
}

// ============================================================================
// 错误处理
// ============================================================================

TEST(DslLexerTest, IllegalCharacterReportsPosition) {
    Lexer lex("Panel @");
    auto tok = lex.nextToken(); // Panel
    EXPECT_TRUE(tok.is(TokenType::Identifier));
    // 扫描到 '@' 时才记录错误
    lex.nextToken(); // 触发 '@' 报错并继续
    EXPECT_TRUE(lex.hasErrors());
    ASSERT_EQ(lex.errors().size(), 1u);
    EXPECT_EQ(lex.errors()[0].line, 1);
    EXPECT_EQ(lex.errors()[0].column, 7);
    EXPECT_NE(lex.errors()[0].message.find("Illegal character"), std::string::npos);
}

TEST(DslLexerTest, UnterminatedStringReportsPosition) {
    Lexer lex("\"abc");
    auto tok = lex.nextToken();
    EXPECT_TRUE(tok.is(TokenType::String));
    EXPECT_TRUE(lex.hasErrors());
    ASSERT_EQ(lex.errors().size(), 1u);
    EXPECT_EQ(lex.errors()[0].line, 1);
    EXPECT_EQ(lex.errors()[0].column, 1);
    EXPECT_NE(lex.errors()[0].message.find("Unterminated string"), std::string::npos);
}

TEST(DslLexerTest, PeekDoesNotConsume) {
    Lexer lex("Panel(10)");
    Token a = lex.peekToken();
    Token b = lex.peekToken();
    EXPECT_TRUE(a.is(TokenType::Identifier));
    EXPECT_TRUE(b.is(TokenType::Identifier));
    Token c = lex.nextToken();
    EXPECT_TRUE(c.is(TokenType::Identifier));
    Token d = lex.nextToken();
    EXPECT_TRUE(d.is(TokenType::LParen));
}

TEST(DslLexerTest, ContextLineProvidesSourceText) {
    Lexer lex("hello\nworld");
    EXPECT_EQ(lex.getContextLine(1), "hello");
    EXPECT_EQ(lex.getContextLine(2), "world");
    EXPECT_EQ(lex.getContextLine(3), "");   // 越界
    EXPECT_EQ(lex.getContextLine(0), "");   // 0-based 无效
}

TEST(DslLexerTest, ColumnTracksWithinLine) {
    Lexer lex("Panel,\n  Button");
    Token panel = lex.nextToken(); // Panel
    Token comma = lex.nextToken(); // ,
    EXPECT_EQ(comma.line, 1);
    EXPECT_EQ(comma.column, 6);
    Token button = lex.nextToken();
    EXPECT_EQ(button.line, 2);
    EXPECT_EQ(button.column, 3);
}