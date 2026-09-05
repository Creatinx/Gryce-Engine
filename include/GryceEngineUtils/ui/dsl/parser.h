#pragma once

// GryceEngineUtils::ui::dsl::parser.h — .uif (DSL) 递归下降语法分析器
//
// 语法（EBNF）：
//   program    = { control };
//   control    = IDENTIFIER "(" [ attributes ] ")" [ "{" { control } "}" ];
//   attributes = attribute { "," attribute };
//   attribute  = IDENTIFIER "=" value;
//   value      = STRING | NUMBER | BOOLEAN | IDENTIFIER | array;
//   array      = "[" [ value { "," value } ] "]";
//
// 错误恢复：解析失败时记录带行列号的 ParseError，并跳到同步点
// （IDENTIFIER / RBRACE / EOF）继续解析，保证单处错误不导致整个文件失败。

#include <string>
#include <vector>

#include "GryceEngineUtils/ui/dsl/ast.h"
#include "GryceEngineUtils/ui/dsl/lexer.h"

namespace GryceEngineUtils::ui::dsl {

// ---------------------------------------------------------------------------
// 语法错误
// ---------------------------------------------------------------------------
struct ParseError {
    int line = 0;            // 1-based
    int column = 0;          // 1-based
    std::string message;
    std::string contextLine;
    std::string hint;

    std::string toString() const;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
public:
    explicit Parser(Lexer& lexer);

    // 解析整个 program，返回控件列表（顶层控件节点）
    std::vector<ASTNode> parse();

    bool hasErrors() const { return !m_errors.empty(); }
    const std::vector<ParseError>& getErrors() const { return m_errors; }

private:
    Lexer& m_lexer;
    std::vector<ParseError> m_errors;
    Token m_current;

    void advance();

    // 语法单元
    ASTNode parseControl();
    std::unordered_map<std::string, ASTValue> parseAttributes();
    ASTValue parseValue();
    ASTValue parseArray();
    std::vector<ASTNode> parseChildBlock();

    bool expect(TokenType type, const std::string& errMsg);

    // 报错
    void error(const std::string& msg);
    void errorAt(const Token& tok, const std::string& msg);

    // 错误恢复：跳到同步点
    void synchronize();
};

} // namespace GryceEngineUtils::ui::dsl