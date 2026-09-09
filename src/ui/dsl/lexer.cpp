// GryceEngineUtils::ui::dsl::lexer.cpp
#include "GryceEngineUtils/ui/dsl/lexer.h"

#include <cctype>
#include <sstream>

namespace GryceEngineUtils::ui::dsl {

namespace {

bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool isIdentChar(char c) {
    // 允许连字符：`font-size`、`scroll-x` 等 CSS 风格属性名是 DSL 规范（UI_DSL_SPEC）
    // 声明的合法 token（如 Text 的 font-size）。数字的负号由数字扫描分支先处理
    // （'-' 后紧跟数字），因此此处 '-' 始终属于标识符的一部分。
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
}
bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

} // namespace

// ============================================================================
// Token
// ============================================================================
const char* Token::type_name() const {
    switch (type) {
        case TokenType::Identifier: return "Identifier";
        case TokenType::String:     return "String";
        case TokenType::Number:     return "Number";
        case TokenType::Boolean:    return "Boolean";
        case TokenType::Equals:     return "Equals";
        case TokenType::Comma:      return "Comma";
        case TokenType::LParen:     return "LParen";
        case TokenType::RParen:     return "RParen";
        case TokenType::LBrace:     return "LBrace";
        case TokenType::RBrace:     return "RBrace";
        case TokenType::LBracket:   return "LBracket";
        case TokenType::RBracket:   return "RBracket";
        case TokenType::EndOfFile:  return "EndOfFile";
        case TokenType::Invalid:    return "Invalid";
    }
    return "Unknown";
}

std::string Token::toString() const {
    std::ostringstream oss;
    oss << type_name() << "('" << value << "', " << line << ":" << column << ")";
    return oss.str();
}

// ============================================================================
// LexError
// ============================================================================
std::string LexError::toString() const {
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
// Lexer
// ============================================================================
Lexer::Lexer(const std::string& source, const std::string& filename)
    : source_(source), filename_(filename) {
    // 按行拆分源文本，供 getContextLine 直接索引
    std::size_t start = 0;
    for (std::size_t i = 0; i <= source_.size(); ++i) {
        if (i == source_.size() || source_[i] == '\n') {
            lines_.push_back(source_.substr(start, i - start));
            start = i + 1;
        }
    }
    if (lines_.empty()) {
        lines_.push_back("");
    }
}

char Lexer::advance() {
    if (pos_ >= source_.size()) {
        return '\0';
    }
    char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

char Lexer::peek() const {
    return (pos_ < source_.size()) ? source_[pos_] : '\0';
}

char Lexer::peekAt(int offset) const {
    std::size_t idx = pos_ + static_cast<std::size_t>(offset);
    return (idx < source_.size()) ? source_[idx] : '\0';
}

Token Lexer::makeToken(TokenType type, const std::string& value, int line, int col) const {
    Token t;
    t.type = type;
    t.value = value;
    t.line = line;
    t.column = col;
    return t;
}

void Lexer::addError(int line, int col, const std::string& msg, const std::string& hint) {
    LexError e;
    e.line = line;
    e.column = col;
    e.message = msg;
    e.hint = hint;
    e.contextLine = getContextLine(line);
    errors_.push_back(std::move(e));
}

std::string Lexer::getContextLine(int line) const {
    if (line < 1 || static_cast<std::size_t>(line) > lines_.size()) {
        return "";
    }
    return lines_[static_cast<std::size_t>(line - 1)];
}

// 跳过空白与注释；跳过过程中如遇未闭合块注释会登记错误。
void Lexer::skipWhitespaceAndComments() {
    for (;;) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
            continue;
        }
        if (c == '/' && peekAt(1) == '/') {
            while (peek() != '\0' && peek() != '\n') {
                advance();
            }
            continue;
        }
        if (c == '/' && peekAt(1) == '*') {
            int sl = line_, sc = column_;
            advance(); advance(); // 消费 "/*"
            bool closed = false;
            while (peek() != '\0') {
                if (peek() == '*' && peekAt(1) == '/') {
                    advance(); advance();
                    closed = true;
                    break;
                }
                advance();
            }
            if (!closed) {
                addError(sl, sc, "Unterminated block comment", "Expected '*/'");
            }
            continue;
        }
        break;
    }
}

Token Lexer::peekToken() {
    if (!has_peeked_) {
        peeked_ = nextToken();
        has_peeked_ = true;
    }
    return peeked_;
}

Token Lexer::nextToken() {
    if (has_peeked_) {
        Token t = peeked_;
        has_peeked_ = false;
        return t;
    }

    // 识别 token 时的起始行列
    int sl = line_, sc = column_;

    skipWhitespaceAndComments();
    sl = line_;
    sc = column_;

    char c = peek();
    if (c == '\0') {
        return makeToken(TokenType::EndOfFile, "", sl, sc);
    }

    // 标点符号
    switch (c) {
        case '=': advance(); return makeToken(TokenType::Equals, "=", sl, sc);
        case ',': advance(); return makeToken(TokenType::Comma, ",", sl, sc);
        case '(': advance(); return makeToken(TokenType::LParen, "(", sl, sc);
        case ')': advance(); return makeToken(TokenType::RParen, ")", sl, sc);
        case '{': advance(); return makeToken(TokenType::LBrace, "{", sl, sc);
        case '}': advance(); return makeToken(TokenType::RBrace, "}", sl, sc);
        case '[': advance(); return makeToken(TokenType::LBracket, "[", sl, sc);
        case ']': advance(); return makeToken(TokenType::RBracket, "]", sl, sc);
        default: break;
    }

    // 字符串：单引号或双引号
    if (c == '\'' || c == '"') {
        char q = advance();
        std::string out;
        bool closed = false;
        for (;;) {
            char d = peek();
            if (d == '\0') {
                break;
            }
            char e = advance();
            if (e == q) {
                closed = true;
                break;
            }
            if (e == '\\') {
                // 转义序列
                char esc = advance();
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\': out.push_back('\\'); break;
                    case '\'': out.push_back('\''); break;
                    case '"': out.push_back('"'); break;
                    case 'r': out.push_back('\r'); break;
                    case '0': out.push_back('\0'); break;
                    default:
                        out.push_back('\\');
                        if (esc != '\0') out.push_back(esc);
                        break;
                }
            } else {
                out.push_back(e);
            }
        }
        if (!closed) {
            addError(sl, sc, "Unterminated string literal",
                     "Expected closing '" + std::string(1, q) + "'");
        }
        return makeToken(TokenType::String, out, sl, sc);
    }

    // 数字：整数/浮点，可选前导负号
    if (isDigit(c) || (c == '-' && isDigit(peekAt(1)))) {
        std::string out;
        if (c == '-') {
            out.push_back(advance());
        }
        bool isFloat = false;
        while (isDigit(peek())) {
            out.push_back(advance());
        }
        if (peek() == '.' && isDigit(peekAt(1))) {
            isFloat = true;
            out.push_back(advance());
            while (isDigit(peek())) {
                out.push_back(advance());
            }
        }
        // 数字后的字符若为标识符，视为非法（如 12ab）
        if (isIdentStart(peek())) {
            addError(sl, sc, "Malformed number literal '", "Unexpected character after number");
            // 保持 out 返回，多余字符将由后续扫描处理
        }
        (void)isFloat;
        return makeToken(TokenType::Number, out, sl, sc);
    }

    // 标识符 或 布尔
    if (isIdentStart(c)) {
        std::string out;
        while (isIdentChar(peek())) {
            out.push_back(advance());
        }
        if (out == "true" || out == "false") {
            return makeToken(TokenType::Boolean, out, sl, sc);
        }
        return makeToken(TokenType::Identifier, out, sl, sc);
    }

    // 非法字符：报错并跳过当前字符，继续扫描下一个合法 token
    {
        std::string bad(1, c);
        std::string hint;
        switch (c) {
            case '@': case '#': case '$': case '%':
            case '^': case '&': case '*': case '!':
            case '?': case '|': case ';': case ':':
                hint = "Character '" + bad + "' is not valid in a .uif DSL file";
                break;
            default:
                hint = "Unexpected character '" + bad + "'";
                break;
        }
        addError(sl, sc, "Illegal character '" + bad + "'", hint);
        advance(); // 消费并记录错误，继续扫描后续合法 token
        return nextToken();
    }
}

} // namespace GryceEngineUtils::ui::dsl