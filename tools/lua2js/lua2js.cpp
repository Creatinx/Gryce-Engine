#include "lua2js.h"

#include <cctype>
#include <cstring>
#include <sstream>

namespace lua2js {

namespace {

bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool word_at(const std::string& s, size_t pos, const char* word) {
    const size_t len = std::strlen(word);
    if (s.compare(pos, len, word) != 0) return false;
    const bool left_ok = (pos == 0) || !is_word_char(s[pos - 1]);
    const bool right_ok = (pos + len >= s.size()) || !is_word_char(s[pos + len]);
    return left_ok && right_ok;
}

// trim 行是否以 word 开头且 word 后只有空白（独立块关键字，如 end/else/do）
bool line_starts_with_word(const std::string& trimmed, const char* word) {
    const size_t len = std::strlen(word);
    if (trimmed.compare(0, len, word) != 0) return false;
    if (len < trimmed.size() && !std::isspace(static_cast<unsigned char>(trimmed[len]))) {
        return false;
    }
    for (size_t i = len; i < trimmed.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(trimmed[i]))) return false;
    }
    return true;
}

// trim 行是否以 word 开头（word 已含尾部空格，允许后续内容）。
// 用于带条件/参数的语句行（function name(...) / if cond then 等），
// 与 line_starts_with_word（要求整行仅关键字）区分。
bool line_starts_with(const std::string& trimmed, const char* word) {
    return trimmed.compare(0, std::strlen(word), word) == 0;
}

// 提取字符串字面量外的行注释（-- 到行尾），返回去掉注释后的内容
std::string strip_line_comment(const std::string& line) {
    char quote = 0;
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const char c = line[i];
        if (quote) {
            if (c == '\\') { ++i; continue; }
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; continue; }
        if (c == '-' && line[i + 1] == '-') {
            return line.substr(0, i);
        }
    }
    return line;
}

// --[[ ... ]] 块注释 -> /* ... */（跨行），-- 行注释保留给 strip_line_comment
std::string strip_block_comments(std::string src) {
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    bool in_string = false;
    char quote = 0;
    while (i < src.size()) {
        const char c = src[i];
        if (in_string) {
            out += c;
            if (c == '\\' && i + 1 < src.size()) { out += src[i + 1]; i += 2; continue; }
            if (c == quote) in_string = false;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') { in_string = true; quote = c; out += c; ++i; continue; }
        if (c == '-' && i + 1 < src.size() && src[i + 1] == '-') {
            size_t p = i + 2;
            if (p < src.size() && src[p] == '[') {
                size_t q = p + 1;
                while (q < src.size() && src[q] == '=') ++q;
                if (q < src.size() && src[q] == '[') {
                    const std::string closer = "]" + std::string(q - p - 1, '=') + "]";
                    const size_t end = src.find(closer, q + 1);
                    if (end != std::string::npos) {
                        out += "/*";
                        out += src.substr(p, end + closer.size() - p);
                        out += "*/";
                        i = end + closer.size();
                        continue;
                    }
                }
            }
            // 行注释：-- 转为 // 保留（便于人工审阅），丢弃到行尾
            out += "//";
            i += 2;
            while (i < src.size() && src[i] != '\n') { out += src[i]; ++i; }
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

bool is_expr_delim(char c) {
    switch (c) {
        case ' ': case '\t': case ',': case ')': case ']':
        case '+': case '-': case '*': case '/': case '%':
        case '<': case '>': case '=': case '~': case '.':
            return true;
        default:
            return false;
    }
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string leading_ws(const std::string& line) {
    size_t b = 0;
    while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
    return line.substr(0, b);
}

struct ConvertState {
    int indent = 0;
    std::vector<std::string> stack;   // 块类型: function/if/for/while/do/repeat
    bool need_len_helper = false;
    bool warned_concat = false;
    std::vector<std::string> warnings;
};

void warn(ConvertState& st, const std::string& msg) {
    st.warnings.push_back(msg);
}

// 运算符与字面量替换（跳过字符串；处理行注释截断）
std::string replace_operators(const std::string& line, ConvertState& st) {
    std::string out;
    out.reserve(line.size() * 2);
    size_t i = 0;
    char quote = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (quote) {
            out += c;
            if (c == '\\' && i + 1 < line.size()) { out += line[i + 1]; i += 2; continue; }
            if (c == quote) quote = 0;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; out += c; ++i; continue; }
        if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') break; // 行注释
        if (line.compare(i, 2, "~=") == 0) { out += "!=="; i += 2; continue; }
        if (line.compare(i, 2, "==") == 0) { out += "==="; i += 2; continue; }
        if (line.compare(i, 2, "..") == 0) {
            out += "+";
            if (!st.warned_concat) {
                st.warned_concat = true;
                warn(st, "字符串连接 .. 已转为 +，请确认两侧均为字符串/数字");
            }
            i += 2;
            continue;
        }
        if (word_at(line, i, "and")) { out += "&&"; i += 3; continue; }
        if (word_at(line, i, "or")) { out += "||"; i += 2; continue; }
        if (word_at(line, i, "not")) { out += "!"; i += 3; continue; }
        if (word_at(line, i, "nil")) { out += "null"; i += 3; continue; }
        if (word_at(line, i, "setmetatable") || word_at(line, i, "getmetatable") ||
            word_at(line, i, "pcall") || word_at(line, i, "xpcall") ||
            word_at(line, i, "coroutine")) {
            out += c;
            warn(st, "Lua 特有 API '" + std::string(line.substr(i, line.find_first_of(" \t(,", i) - i)) + "' 需手动重写");
            ++i;
            continue;
        }
        if (word_at(line, i, "require")) {
            out += c;
            warn(st, "require() 需手动改为 import/加载器");
            ++i;
            continue;
        }
        if (c == '#') {
            // #expr -> __len(expr)
            size_t j = i + 1;
            while (j < line.size() && !is_expr_delim(line[j])) ++j;
            std::string expr = trim(line.substr(i + 1, j - i - 1));
            if (!expr.empty()) {
                out += "__len(" + expr + ")";
                st.need_len_helper = true;
                i = j;
                continue;
            }
        }
        out += c;
        ++i;
    }
    // 去掉行尾空白
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
    return out;
}

// 表构造 {a=1, [k]=v} -> {a:1, [k]:v}（键后允许空白，如 {a = 1}）
std::string convert_table_constructs(std::string line) {
    size_t i = 0;
    char quote = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (quote) {
            if (c == '\\') { i += 2; continue; }
            if (c == quote) quote = 0;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; ++i; continue; }
        if (c == '{' || c == ',') {
            size_t j = i + 1;
            while (j < line.size() && std::isspace(static_cast<unsigned char>(line[j]))) ++j;
            if (j < line.size() && line[j] == '[') {
                const size_t close = line.find(']', j);
                if (close != std::string::npos) {
                    size_t m = close + 1;
                    while (m < line.size() && std::isspace(static_cast<unsigned char>(line[m]))) ++m;
                    if (m < line.size() && line[m] == '=') {
                        line.erase(close + 1, m - (close + 1)); // 删除 ] 与 = 之间空白
                        line[close + 1] = ':';
                    }
                }
            } else if (is_word_char(line[j])) {
                size_t k = j;
                while (k < line.size() && is_word_char(line[k])) ++k;
                size_t m = k;
                while (m < line.size() && std::isspace(static_cast<unsigned char>(line[m]))) ++m;
                if (m < line.size() && line[m] == '=') {
                    line.erase(k, m - k); // 删除键名与 = 之间空白
                    line[k] = ':';
                }
            }
        }
        ++i;
    }
    return line;
}

// 多返回值 return a, b -> return [a, b]（容忍前导缩进）
std::string convert_multi_return(const std::string& line, ConvertState& st) {
    const std::string trimmed = trim(line);
    if (trimmed.compare(0, 7, "return ") != 0) return line;
    std::string rest = trimmed.substr(7);
    size_t i = 0;
    char quote = 0;
    while (i < rest.size()) {
        const char c = rest[i];
        if (quote) {
            if (c == '\\') { i += 2; continue; }
            if (c == quote) quote = 0;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') { quote = c; ++i; continue; }
        if (c == ',') {
            warn(st, "多返回值 return 已包为数组，需手动确认");
            // 保留原行前导空白
            return line.substr(0, line.size() - trimmed.size()) + "return [" + rest + "]";
        }
        ++i;
    }
    return line;
}

// 数值 for 头：for i=a,b[,c] do
std::string convert_numeric_for(const std::string& inner, ConvertState& st) {
    // inner 形如 "i=a,b" 或 "i=a,b,c"
    const size_t eq = inner.find('=');
    if (eq == std::string::npos) return inner;
    const std::string var = trim(inner.substr(0, eq));
    const std::string range = inner.substr(eq + 1);
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= range.size(); ++i) {
        if (i == range.size() || range[i] == ',') {
            parts.push_back(trim(range.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (parts.size() < 2) return inner;
    const std::string a = parts[0], b = parts[1], step = (parts.size() >= 3) ? parts[2] : "1";
    std::string cmp = "<=";
    std::string sstep = trim(step);
    if (!sstep.empty() && sstep[0] == '-') cmp = ">=";
    warn(st, "数值 for 已转为 for(let ...)，步长/边界需人工确认");
    return "for (let " + var + " = " + a + "; " + var + " " + cmp + " " + b + "; " + var + " += " + sstep + ") {";
}

} // namespace

ConvertResult convert(const std::string& luaSource) {
    ConvertResult result;
    ConvertState st;

    const std::string no_block_comments = strip_block_comments(luaSource);
    std::istringstream in(no_block_comments);
    std::string line;
    std::ostringstream out;

    while (std::getline(in, line)) {
        const std::string ws = leading_ws(line);
        std::string content = strip_line_comment(line);
        const std::string t = trim(content);

        if (t.empty()) {
            out << "\n";
            continue;
        }

        // ---- 块结束 ----
        if (line_starts_with_word(t, "end")) {
            if (st.stack.empty()) {
                warn(st, "多余的 end，已忽略");
                out << ws << "}\n";
            } else {
                st.stack.pop_back();
                --st.indent;
                if (st.indent < 0) st.indent = 0;
                out << ws << "}\n";
            }
            continue;
        }
        if (line_starts_with(t, "elseif ")) {
            if (st.stack.empty() || st.stack.back() != "if") {
                warn(st, "elseif 前无 if 块，转换可能不准确");
            }
            std::string cond = trim(t.substr(7));
            if (!cond.empty() && cond.compare(cond.size() - 5, 5, " then") == 0) {
                cond = trim(cond.substr(0, cond.size() - 5));
            }
            out << ws << "} else if (" << cond << ") {\n";
            continue;
        }
        if (line_starts_with_word(t, "else")) {
            if (st.stack.empty() || st.stack.back() != "if") {
                warn(st, "else 前无 if 块，转换可能不准确");
            }
            out << ws << "} else {\n";
            continue;
        }

        // ---- local function ----
        if (t.compare(0, 15, "local function ") == 0) {
            const std::string rest = t.substr(15);
            const size_t lp = rest.find('(');
            if (lp == std::string::npos) {
                warn(st, "无法解析 local function 签名");
                out << ws << replace_operators(content, st) << "\n";
                continue;
            }
            const std::string name = trim(rest.substr(0, lp));
            const size_t rp = rest.find(')', lp);
            const std::string params = (rp != std::string::npos) ? rest.substr(lp + 1, rp - lp - 1) : "";
            st.stack.push_back("function");
            ++st.indent;
            out << ws << "function " + name + "(" << params << ") {\n";
            continue;
        }

        // ---- 顶层 function ----
        if (line_starts_with(t, "function ")) {
            const std::string rest = t.substr(9);
            const size_t lp = rest.find('(');
            if (lp == std::string::npos) {
                warn(st, "无法解析 function 签名");
                out << ws << replace_operators(content, st) << "\n";
                continue;
            }
            const std::string name = trim(rest.substr(0, lp));
            std::string head;
            if (name.find('.') != std::string::npos || name.find(':') != std::string::npos) {
                // 表方法 function a.b.c() / function a:b() -> a.b.c = function(...)
                std::string fn_name = name;
                bool colon = false;
                const size_t colon_pos = fn_name.find(':');
                if (colon_pos != std::string::npos) {
                    colon = true;
                    fn_name.replace(colon_pos, 1, ".");
                }
                head = fn_name + " = function(";
                if (colon) head += "self, ";
                warn(st, "表方法 '" + name + "' 已转为赋值形式，self 语义需确认");
            } else {
                head = "export function " + name + "(";
            }
            st.stack.push_back("function");
            ++st.indent;
            // 补全括号与参数、{：从原 rest 中提取括号内容
            const size_t rp = rest.find(')', lp);
            const std::string params = (rp != std::string::npos) ? rest.substr(lp + 1, rp - lp - 1) : "";
            out << ws << head << params << ") {\n";
            continue;
        }

        // ---- local 变量 ----
        if (line_starts_with(t, "local ")) {
            std::string rest = t.substr(6);
            // Lua 的 local 可重新赋值，转 let 而非 const（如 s = s + 1）
            std::string out_line = "let " + replace_operators(rest, st);
            out_line = convert_table_constructs(out_line);
            if (out_line.find("function(") != std::string::npos ||
                out_line.find("function (") != std::string::npos) {
                st.stack.push_back("function");
                ++st.indent;
                warn(st, "local 匿名函数已转为 let，闭包引用需人工确认");
            }
            out << ws << out_line << "\n";
            continue;
        }

        // ---- if ----
        if (line_starts_with(t, "if ")) {
            std::string cond = trim(t.substr(3));
            if (cond.size() >= 5 && cond.compare(cond.size() - 5, 5, " then") == 0) {
                cond = trim(cond.substr(0, cond.size() - 5));
            } else {
                warn(st, "if 缺少 then，请检查语法");
            }
            st.stack.push_back("if");
            ++st.indent;
            out << ws << "if (" << cond << ") {\n";
            continue;
        }

        // ---- while ----
        if (line_starts_with(t, "while ")) {
            std::string cond = trim(t.substr(6));
            if (cond.size() >= 3 && cond.compare(cond.size() - 3, 3, " do") == 0) {
                cond = trim(cond.substr(0, cond.size() - 3));
            } else {
                warn(st, "while 缺少 do，请检查语法");
            }
            st.stack.push_back("while");
            ++st.indent;
            out << ws << "while (" << cond << ") {\n";
            continue;
        }

        // ---- for ----
        if (line_starts_with(t, "for ")) {
            std::string rest = trim(t.substr(4));
            const size_t do_pos = rest.rfind(" do");
            std::string inner = (do_pos != std::string::npos) ? trim(rest.substr(0, do_pos)) : rest;
            if (do_pos == std::string::npos) warn(st, "for 缺少 do，请检查语法");

            const size_t in_pos = inner.find(" in ");
            st.stack.push_back("for");
            ++st.indent;
            if (in_pos != std::string::npos) {
                // 泛型 for：for k,v in pairs(t) do
                const std::string vars = trim(inner.substr(0, in_pos));
                std::string iter = inner.substr(in_pos + 4);
                std::string js_iter = "Object.entries(" + iter + ")";
                if (iter.compare(0, 5, "pairs") == 0) {
                    js_iter = "Object.entries(" + iter.substr(6, iter.size() - 7) + ")";
                } else if (iter.compare(0, 6, "ipairs") == 0) {
                    js_iter = "Object.entries(" + iter.substr(7, iter.size() - 8) + ")";
                    warn(st, "ipairs 已按 Object.entries 转换，索引语义需人工确认");
                } else {
                    warn(st, "自定义迭代器 '" + iter + "' 需手动转换");
                }
                out << ws << "for (const [" + vars + "] of " << js_iter << ") {\n";
            } else {
                out << ws << convert_numeric_for(inner, st) << "\n";
            }
            continue;
        }

        // ---- do 块 ----
        if (line_starts_with_word(t, "do")) {
            st.stack.push_back("do");
            ++st.indent;
            out << ws << "{\n";
            continue;
        }

        // ---- repeat / until ----
        if (line_starts_with_word(t, "repeat")) {
            warn(st, "repeat/until 已按 do/while 转换，边界需人工确认");
            st.stack.push_back("repeat");
            ++st.indent;
            out << ws << "do {\n";
            continue;
        }
        if (line_starts_with(t, "until ")) {
            if (!st.stack.empty() && st.stack.back() == "repeat") st.stack.pop_back();
            --st.indent;
            if (st.indent < 0) st.indent = 0;
            const std::string cond = trim(t.substr(6));
            out << ws << "} while (!(" << cond << "));\n";
            continue;
        }

        // ---- 普通行 ----
        std::string body = replace_operators(content, st);
        body = convert_table_constructs(body);
        body = convert_multi_return(body, st);
        out << ws << body << "\n";
    }

    // 未闭合块
    while (!st.stack.empty()) {
        st.stack.pop_back();
        warn(st, "文件结束时存在未闭合块，已补 }");
        if (st.indent > 0) --st.indent;
        out << "}\n";
    }

    // 注入 __len helper（# 长度运算）
    if (st.need_len_helper) {
        const std::string helper =
            "function __len(v) { if (v === null || v === undefined) return 0; "
            "return typeof v.length === 'number' ? v.length : Object.keys(v).length; }\n";
        result.output = helper + out.str();
    } else {
        result.output = out.str();
    }
    result.warnings = st.warnings;
    return result;
}

} // namespace lua2js
