#include "string_util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace gryce_engine::gc {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string to_pascal_case(std::string s) {
    std::string out;
    bool at_word_start = true;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            if (at_word_start) out.push_back(static_cast<char>(std::toupper(c)));
            else out.push_back(static_cast<char>(std::tolower(c)));
            at_word_start = false;
        } else {
            at_word_start = true; // 非字母数字为分词符
        }
    }
    if (out.empty()) out = "MyGame";
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

} // namespace gryce_engine::gc