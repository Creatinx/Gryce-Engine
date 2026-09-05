#pragma once

// GryceEngineUtils::ui::dsl::lexer.h — .uif (DSL) 词法分析器
//
// .uif 的 DSL 语法（自定义标记语言，区别于旧的 pugixml XML 形式）：
//   program    = { control };
//   control    = IDENTIFIER "(" [ attributes ] ")" [ "{" { control } "}" ];
//   attributes = attribute { "," attribute };
//   attribute  = IDENTIFIER "=" value;
//   value      = STRING | NUMBER | BOOLEAN | IDENTIFIER | array;
//   array      = "[" [ value { "," value } ] "]";
//
// Lexer 负责任何文本片段切分为 Token 流，并对非法字符、未闭合字符串等
// 词法错误输出带行列号（1-based）的报错信息。
//
// 该模块是 DSL 重构（替换 pugixml XML 解析）的第一阶段，独立于
// UIParser（pugixml）。后续 Parser / SemanticAnalyzer / ASTOptimizer
// 均以本模块产生的 Token 流为基础。

#include <cstdint>
#include <string>
#include <vector>

#include "export.h"

namespace GryceEngineUtils::ui::dsl {

// ---------------------------------------------------------------------------
// Token 类型
// ---------------------------------------------------------------------------
enum class TokenType {
    Identifier,     // Window, Panel, onStart
    String,         // "主菜单", 'Hello'
    Number,         // 800, 0.5, -10
    Boolean,        // true, false
    Equals,         // =
    Comma,          // ,
    LParen,         // (
    RParen,         // )
    LBrace,         // {
    RBrace,         // }
    LBracket,       // [
    RBracket,       // ]
    EndOfFile,      // EOF
    Invalid         // 非法字符
};

// ---------------------------------------------------------------------------
// Token
// ---------------------------------------------------------------------------
struct GRYCE_API Token {
    TokenType type = TokenType::Invalid;
    std::string value;       // 字面量文本（字符串已去引号、数字为原文）
    int line = 0;            // 1-based 行号
    int column = 0;          // 1-based 列号

    bool is(TokenType t) const { return type == t; }
    const char* type_name() const;
    std::string toString() const;   // 用于调试与报错的完整描述
};

// ---------------------------------------------------------------------------
// 词法错误
// ---------------------------------------------------------------------------
struct GRYCE_API LexError {
    int line = 0;            // 1-based
    int column = 0;          // 1-based
    std::string message;
    std::string contextLine; // 出错处的源行文本
    std::string hint;        // 可选修复建议

    std::string toString() const;
};

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------
class Lexer {
public:
    Lexer(const std::string& source, const std::string& filename = "<uif>");

    // 扫描并返回下一个 Token
    Token nextToken();

    // 预取当前 Token 而不消费（重复调用返回同一个 Token）
    Token peekToken();

    // 是否有未消费的 Token 流错误
    bool hasErrors() const { return !errors_.empty(); }
    const std::vector<LexError>& errors() const { return errors_; }

    // 当前（1-based）行列
    int getLine() const { return line_; }
    int getColumn() const { return column_; }

    // 返回指定 1-based 行的源文本；行号越界返回空串
    std::string getContextLine(int line) const;

    const std::string& filename() const { return filename_; }
    bool atEnd() const { return has_peeked_ ? peeked_.is(TokenType::EndOfFile)
                                            : peeked_.is(TokenType::Invalid) && pos_ >= source_.size(); }

private:
    // 底层扫描当前字符并推进位置
    char advance();
    char peek() const;
    char peekAt(int offset) const;

    void skipWhitespaceAndComments();
    void addError(int line, int col, const std::string& msg, const std::string& hint = "");

    Token makeToken(TokenType type, const std::string& value, int line, int col) const;

    std::string source_;
    std::string filename_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;

    std::vector<std::string> lines_;    // 按行拆分，第 i 项为第 (i+1) 行文本

    Token peeked_;
    bool has_peeked_ = false;
    std::vector<LexError> errors_;
};

} // namespace GryceEngineUtils::ui::dsl