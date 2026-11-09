#pragma once

// ---------------------------------------------------------------------------
// code_highlight.h — 轻量代码分词器（语法高亮用，header-only）
//
// 设计取舍：
//   * **逐行**分词。跨行块注释 `/* ... */` 只在本行内闭合，不维护跨行状态；
//     代码编辑器的可视区域本就只关心当前屏的行，跨行状态的收益远小于复杂度。
//   * 输出是**连续覆盖整行**的 token 流（含空白），这样按 token 生成的彩色 run
//     首尾相接，`draw_text_colored` 逐 run 累加宽度就能和原文排版完全一致。
//   * 不追求语言学正确，只做"看起来对、够快"的着色：字符串、注释、数字、
//     关键字、类型、函数调用、运算符、预处理指令。
// ---------------------------------------------------------------------------

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace gryce_engine::ui {

enum class CodeTokenRole : uint8_t {
    Plain = 0,
    Keyword,
    Type,
    String,
    Comment,
    Number,
    Function,
    Operator,
    Punctuation,
    Preprocessor,
    Constant,
};

struct CodeToken {
    std::size_t start = 0;
    std::size_t length = 0;
    CodeTokenRole role = CodeTokenRole::Plain;
};

namespace code_detail {

inline bool is_ident_start(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalpha(uc) != 0 || c == '_' || c == '$';
}

inline bool is_ident_char(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_' || c == '$';
}

inline const std::unordered_set<std::string>& keyword_set() {
    static const std::unordered_set<std::string> s = {
        // C / C++
        "if", "else", "for", "while", "do", "switch", "case", "default", "break",
        "continue", "return", "goto", "struct", "class", "enum", "union",
        "typedef", "namespace", "using", "template", "typename", "public",
        "private", "protected", "virtual", "override", "final", "static",
        "const", "constexpr", "consteval", "extern", "inline", "volatile",
        "mutable", "explicit", "new", "delete", "this", "sizeof", "friend",
        "operator", "throw", "try", "catch", "co_await", "co_return", "noexcept",
        // JS / TS
        "function", "var", "let", "of", "in", "typeof", "instanceof", "export",
        "import", "from", "as", "async", "await", "yield", "extends", "implements",
        "interface", "type", "readonly", "declare", "delete", "void", "with",
        // GLSL
        "layout", "uniform", "varying", "attribute", "inout", "precision",
        "highp", "mediump", "lowp", "discard", "flat", "smooth", "centroid",
        // Python 风格（顺带）
        "def", "lambda", "elif", "pass", "raise", "import", "global", "nonlocal",
    };
    return s;
}

inline const std::unordered_set<std::string>& type_set() {
    static const std::unordered_set<std::string> s = {
        "int", "float", "double", "bool", "char", "short", "long", "unsigned",
        "signed", "size_t", "ssize_t", "uint", "uint8_t", "uint16_t", "uint32_t",
        "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "uchar", "byte",
        "string", "auto", "std", "vec2", "vec3", "vec4", "ivec2", "ivec3",
        "ivec4", "bvec2", "bvec3", "bvec4", "uvec2", "uvec3", "uvec4", "mat2",
        "mat3", "mat4", "sampler2D", "sampler3D", "samplerCube", "sampler2DArray",
        "Texture2D", "Texture3D", "any", "unknown", "number", "boolean",
    };
    return s;
}

inline const std::unordered_set<std::string>& constant_set() {
    static const std::unordered_set<std::string> s = {
        "true", "false", "nullptr", "NULL", "null", "undefined", "PI", "NaN",
        "Infinity",
    };
    return s;
}

inline void push_token(std::vector<CodeToken>& out, std::size_t start,
                       std::size_t len, CodeTokenRole role) {
    if (len == 0) return;
    out.push_back(CodeToken{start, len, role});
}

} // namespace code_detail

// 把一行源码切成连续覆盖整行的 token（空白保留为 Plain，保证排版不变）。
inline std::vector<CodeToken> tokenize_code_line(std::string_view line,
                                                 const std::string& language) {
    std::vector<CodeToken> tokens;
    const std::size_t n = line.size();
    if (n == 0) return tokens;

    // 预处理指令（#include / #define ...）：整行单色，最省事也最贴近直觉。
    {
        std::size_t k = 0;
        while (k < n && std::isspace(static_cast<unsigned char>(line[k])) != 0) ++k;
        if (k < n && line[k] == '#') {
            code_detail::push_token(tokens, 0, k, CodeTokenRole::Plain);
            code_detail::push_token(tokens, k, n - k, CodeTokenRole::Preprocessor);
            return tokens;
        }
    }

    // json 语言：只有字符串 / 数字 / 标点，不套用关键字表，避免误色。
    const bool json = (language == "json");
    const auto& keywords = code_detail::keyword_set();
    const auto& types = code_detail::type_set();
    const auto& constants = code_detail::constant_set();
    static const std::string ops = "+-*/%=<>!&|^~?:";

    std::size_t i = 0;
    while (i < n) {
        const char c = line[i];

        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            const std::size_t s = i;
            while (i < n && std::isspace(static_cast<unsigned char>(line[i])) != 0) ++i;
            code_detail::push_token(tokens, s, i - s, CodeTokenRole::Plain);
            continue;
        }

        // 行注释 //
        if (c == '/' && i + 1 < n && line[i + 1] == '/') {
            code_detail::push_token(tokens, i, n - i, CodeTokenRole::Comment);
            i = n;
            continue;
        }
        // 块注释 /* ... */（仅本行内闭合）
        if (c == '/' && i + 1 < n && line[i + 1] == '*') {
            const std::size_t s = i;
            i += 2;
            while (i + 1 < n && !(line[i] == '*' && line[i + 1] == '/')) ++i;
            i = (i + 1 < n) ? i + 2 : n;
            code_detail::push_token(tokens, s, i - s, CodeTokenRole::Comment);
            continue;
        }
        // 字符串 / 模板串
        if (c == '"' || c == '\'' || c == '`') {
            const char quote = c;
            const std::size_t s = i;
            ++i;
            while (i < n) {
                if (line[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (line[i] == quote) { ++i; break; }
                ++i;
            }
            code_detail::push_token(tokens, s, i - s, CodeTokenRole::String);
            continue;
        }
        // 数字（含指数、十六进制、小数）
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(line[i + 1])) != 0)) {
            const std::size_t s = i;
            ++i;
            while (i < n) {
                const char d = line[i];
                if (std::isalnum(static_cast<unsigned char>(d)) != 0 || d == '.' || d == '_') {
                    ++i;
                } else if ((d == '+' || d == '-') &&
                           (line[i - 1] == 'e' || line[i - 1] == 'E')) {
                    ++i;
                } else {
                    break;
                }
            }
            code_detail::push_token(tokens, s, i - s, CodeTokenRole::Number);
            continue;
        }
        // 标识符 / 关键字
        if (code_detail::is_ident_start(c)) {
            const std::size_t s = i;
            ++i;
            while (i < n && code_detail::is_ident_char(line[i])) ++i;
            const std::string word(line.substr(s, i - s));
            CodeTokenRole role = CodeTokenRole::Plain;
            if (!json) {
                std::size_t j = i;
                while (j < n && std::isspace(static_cast<unsigned char>(line[j])) != 0) ++j;
                const bool call = (j < n && line[j] == '(');
                if (keywords.find(word) != keywords.end()) {
                    role = CodeTokenRole::Keyword;
                } else if (types.find(word) != types.end()) {
                    role = CodeTokenRole::Type;
                } else if (constants.find(word) != constants.end()) {
                    role = CodeTokenRole::Constant;
                } else if (call) {
                    role = CodeTokenRole::Function;
                }
            }
            code_detail::push_token(tokens, s, i - s, role);
            continue;
        }
        // 运算符
        if (!json && ops.find(c) != std::string::npos) {
            code_detail::push_token(tokens, i, 1, CodeTokenRole::Operator);
            ++i;
            continue;
        }
        // 其它（标点、括号、逗号、冒号等）
        code_detail::push_token(tokens, i, 1, CodeTokenRole::Punctuation);
        ++i;
    }
    return tokens;
}

} // namespace gryce_engine::ui
