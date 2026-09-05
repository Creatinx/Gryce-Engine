// GryceEngineUtils::ui::dsl::parser.cpp
#include "GryceEngineUtils/ui/dsl/parser.h"

#include <sstream>

namespace GryceEngineUtils::ui::dsl {

// ============================================================================
// ParseError
// ============================================================================
std::string ParseError::toString() const {
    std::ostringstream oss;
    oss << line << ":" << column << " - " << message;
    if (!contextLine.empty()) {
        oss << "\n  " << contextLine;
    }
    if (!hint.empty()) {
        oss << "\nHint: " << hint;
    }
    return oss.str();
}

// ============================================================================
// Parser
// ============================================================================
Parser::Parser(Lexer& lexer) : m_lexer(lexer) {
    m_current = m_lexer.nextToken();
}

void Parser::advance() {
    m_current = m_lexer.nextToken();
}

bool Parser::expect(TokenType type, const std::string& errMsg) {
    if (m_current.is(type)) {
        advance();
        return true;
    }
    error(errMsg);
    return false;
}

void Parser::error(const std::string& msg) {
    errorAt(m_current, msg);
}

void Parser::errorAt(const Token& tok, const std::string& msg) {
    ParseError e;
    e.line = tok.line;
    e.column = tok.column;
    e.message = msg;
    e.contextLine = m_lexer.getContextLine(tok.line);
    switch (tok.type) {
        case TokenType::RParen: e.hint = "Expected ')' but found '" + tok.value + "'"; break;
        case TokenType::EndOfFile: e.hint = "Unexpected end of file"; break;
        default: e.hint = "Found '" + tok.value + "'"; break;
    }
    m_errors.push_back(std::move(e));
}

// 跳到下一个同步点：IDENTIFIER / RBRACE / EOF
void Parser::synchronize() {
    while (!m_current.is(TokenType::Identifier) &&
           !m_current.is(TokenType::RBrace) &&
           !m_current.is(TokenType::EndOfFile)) {
        advance();
    }
}

// ============================================================================
// program
// ============================================================================
std::vector<ASTNode> Parser::parse() {
    std::vector<ASTNode> nodes;
    while (!m_current.is(TokenType::EndOfFile)) {
        nodes.push_back(parseControl());
        // 当 parseControl 因错误无法推进时（同步点停在相邻 IDENTIFIER），
        // 外层循环自然继续解析下一个控件。
    }
    return nodes;
}

// ============================================================================
// control = IDENTIFIER "(" [ attributes ] ")" [ "{" { control } "}" ]
// ============================================================================
ASTNode Parser::parseControl() {
    ASTNode node;

    if (!m_current.is(TokenType::Identifier)) {
        errorAt(m_current, "Expected control type name (IDENTIFIER)");
        if (m_current.is(TokenType::EndOfFile)) {
            return node; // 到达文件尾，无法恢复
        }
        synchronize();
        return node; // 跳过损坏的控件，交由外层决定是否结束
    }

    node.type = m_current.value;
    node.line = m_current.line;
    node.column = m_current.column;
    advance();

    if (expect(TokenType::LParen, "Expected '(' after control name '" + node.type + "'")) {
        node.attributes = parseAttributes();
        expect(TokenType::RParen,
               "Expected ')' to close attribute list of '" + node.type + "'");
    } else {
        // 缺少 '('：跳过该控件的剩余内容，保持错误恢复
        synchronize();
        return node;
    }

    if (m_current.is(TokenType::LBrace)) {
        node.children = parseChildBlock();
    }
    return node;
}

// ============================================================================
// attributes = attribute { "," attribute };
// attribute  = IDENTIFIER "=" value;
// ============================================================================
std::unordered_map<std::string, ASTValue> Parser::parseAttributes() {
    std::unordered_map<std::string, ASTValue> attrs;

    if (m_current.is(TokenType::RParen)) {
        return attrs; // 空属性列表
    }

    for (;;) {
        if (!m_current.is(TokenType::Identifier)) {
            errorAt(m_current, "Expected attribute name (IDENTIFIER)");
            // 跳过损坏的属性直到分隔符
            while (!m_current.is(TokenType::Comma) && !m_current.is(TokenType::RParen) &&
                   !m_current.is(TokenType::EndOfFile)) {
                advance();
            }
            if (m_current.is(TokenType::Comma)) {
                advance();
                continue;
            }
            break; // ')' 或 EOF
        }

        std::string name = m_current.value;
        advance();

        if (!expect(TokenType::Equals, "Expected '=' after attribute '" + name + "'")) {
            while (!m_current.is(TokenType::Comma) && !m_current.is(TokenType::RParen) &&
                   !m_current.is(TokenType::EndOfFile)) {
                advance();
            }
            if (m_current.is(TokenType::Comma)) {
                advance();
                continue;
            }
            break;
        }

        ASTValue val = parseValue();
        if (val.kind == ASTValue::Kind::String && val.line == 0) {
            // parseValue 报错后返回空值（line 仍为默认 0），跳过该属性
            while (!m_current.is(TokenType::Comma) && !m_current.is(TokenType::RParen) &&
                   !m_current.is(TokenType::EndOfFile)) {
                advance();
            }
            if (m_current.is(TokenType::Comma)) {
                advance();
                continue;
            }
            break;
        }

        // 重复属性：保留最后一个（后续 ASTOptimizer 亦会去重告警）
        attrs[name] = std::move(val);

        if (m_current.is(TokenType::Comma)) {
            advance();
            continue;
        }
        break; // 预期 ')' 或结束
    }
    return attrs;
}

// ============================================================================
// value = STRING | NUMBER | BOOLEAN | IDENTIFIER | array
// ============================================================================
ASTValue Parser::parseValue() {
    ASTValue val;
    val.line = m_current.line;
    val.column = m_current.column;

    switch (m_current.type) {
        case TokenType::String:
            val.kind = ASTValue::Kind::String;
            val.stringValue = m_current.value;
            advance();
            return val;
        case TokenType::Number:
            val.kind = ASTValue::Kind::Number;
            val.numberValue = std::stod(m_current.value);
            advance();
            return val;
        case TokenType::Boolean:
            val.kind = ASTValue::Kind::Boolean;
            val.boolValue = (m_current.value == "true");
            advance();
            return val;
        case TokenType::Identifier:
            val.kind = ASTValue::Kind::Identifier;
            val.stringValue = m_current.value;
            advance();
            return val;
        case TokenType::LBracket:
            return parseArray();
        default:
            errorAt(m_current, "Expected attribute value");
            val.line = 0;       // 标记失败
            val.column = 0;
            return val;
    }
}

// ============================================================================
// array = "[" [ value { "," value } ] "]"
// ============================================================================
ASTValue Parser::parseArray() {
    ASTValue arr;
    arr.kind = ASTValue::Kind::Array;
    arr.line = m_current.line;
    arr.column = m_current.column;
    advance(); // 消费 '['

    while (!m_current.is(TokenType::RBracket)) {
        if (m_current.is(TokenType::EndOfFile)) {
            errorAt(m_current, "Expected ']' to close array");
            break;
        }
        ASTValue item = parseValue();
        arr.arrayValue.push_back(std::move(item));
        if (m_current.is(TokenType::Comma)) {
            advance();
        } else {
            break;
        }
    }
    expect(TokenType::RBracket, "Expected ']' to close array");
    return arr;
}

// ============================================================================
// block = "{" { control } "}"
// ============================================================================
std::vector<ASTNode> Parser::parseChildBlock() {
    std::vector<ASTNode> children;
    advance(); // 消费 '{'

    while (!m_current.is(TokenType::RBrace)) {
        if (m_current.is(TokenType::EndOfFile)) {
            errorAt(m_current, "Expected '}' to close control block");
            break;
        }
        children.push_back(parseControl());
    }
    if (m_current.is(TokenType::RBrace)) {
        advance();
    }
    return children;
}

} // namespace GryceEngineUtils::ui::dsl