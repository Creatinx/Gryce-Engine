// ===========================================================================
// engine/resources/project_data.cpp
// project.data 的容错读取：剥离尾随逗号/BOM 后再交给 nlohmann 解析。
// 设计口径见 project_data.h。
// ===========================================================================
#include "project_data.h"

#include <cstddef>
#include <fstream>
#include <sstream>
#include <vector>

namespace gryce_engine::resources {
namespace {

// 剥掉字符串字面量之外、紧邻 '}' 或 ']' 的多余逗号，并记录行号。
// nlohmann 是严格 JSON 解析器，不接受尾随逗号；而手写/脚本生成的配置里
// 这是最常见的错误，直接判死会让整份设置静默失效，代价远大于宽容一次。
std::string strip_trailing_commas(const std::string& src,
                                  std::vector<int>& fixed_lines) {
    std::string out;
    out.reserve(src.size());

    bool in_string = false;
    bool escaped = false;
    int line = 1;
    // 已输出但尚未确定"后面是否还有有效字符"的逗号位置，用于回溯删除。
    std::size_t pending_comma = std::string::npos;
    int pending_line = 0;

    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];

        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            if (c == '\n') ++line;
            out.push_back(c);
            pending_comma = std::string::npos;
            continue;
        }

        switch (c) {
            case '"':
                in_string = true;
                out.push_back(c);
                pending_comma = std::string::npos;
                break;
            case ',':
                // 先挂起，等看到下一个非空白字符再决定去留。
                pending_comma = out.size();
                pending_line = line;
                out.push_back(c);
                break;
            case '}':
            case ']':
                if (pending_comma != std::string::npos) {
                    out.erase(pending_comma, 1);
                    fixed_lines.push_back(pending_line);
                    pending_comma = std::string::npos;
                }
                out.push_back(c);
                break;
            case ' ':
            case '\t':
            case '\r':
                out.push_back(c);
                break;
            case '\n':
                ++line;
                out.push_back(c);
                break;
            default:
                // 任何其它有效字符都说明这个逗号后面还有内容，保留。
                pending_comma = std::string::npos;
                out.push_back(c);
                break;
        }
    }
    return out;
}

} // namespace

ProjectDataResult read_project_data(const std::string& root) {
    ProjectDataResult result;

    std::ifstream in(root + "/project.data", std::ios::binary);
    if (!in) return result;
    result.found = true;

    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();

    // UTF-8 BOM：Windows 编辑器常带，严格解析会当作非法字符。
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }

    std::vector<int> fixed_lines;
    const std::string cleaned = strip_trailing_commas(text, fixed_lines);

    try {
        result.data = nlohmann::json::parse(cleaned, nullptr, true, true);
        result.ok = true;
        if (!fixed_lines.empty()) {
            std::ostringstream note;
            note << "project.data 有 " << fixed_lines.size() << " 处尾随逗号（第 ";
            for (std::size_t i = 0; i < fixed_lines.size(); ++i) {
                if (i) note << "、";
                note << fixed_lines[i];
            }
            note << " 行），已容错忽略；JSON 标准不允许尾随逗号，建议修正。";
            result.tolerant_note = note.str();
        }
    } catch (const std::exception& e) {
        result.ok = false;
        result.error = e.what();
        result.error += "\n  提示：project.data 必须是严格 JSON（键要加双引号、"
                        "字符串用双引号、对象内不能有注释）；"
                        "本轮所有运行设置与启动场景都会退回内置默认值。";
    }
    return result;
}

} // namespace gryce_engine::resources
