// ===========================================================================
// editor/ext/common/json_writer.h — 极简 JSON 拼装
//
// ext 的对外接口一半以上返回 JSON 字符串（面板数据、会话状态、资产列表），
// 但扩展不该为一个"拼字符串"引入 json 库：nlohmann 是 header-only 也要
// 30 万行，把扩展的编译时间从 2 秒拉到 20 秒，而这里只用得上拼装。
//
// 只做输出，不做解析。数字按最短表示输出（整数不带小数点），字符串做完整
// 转义（引号、反斜杠、控制字符、UTF-8 原样透传）。
// ===========================================================================

#ifndef GRYCE_EDITOR_JSON_WRITER_H
#define GRYCE_EDITOR_JSON_WRITER_H

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace gryce::editor {

class Json {
public:
    Json() { out_.reserve(256); }

    Json& begin_object() { open('{'); return *this; }
    Json& end_object() { close('}'); return *this; }
    Json& begin_array() { open('['); return *this; }
    Json& end_array() { close(']'); return *this; }

    // 对象里写键（下一个 value 会跟在它后面）。
    Json& key(const char* k) {
        pre_value();
        append_string(k ? k : "");
        out_ += ':';
        after_key_ = true;
        return *this;
    }

    Json& value(const char* v) {
        pre_value();
        append_string(v ? v : "");
        return *this;
    }
    Json& value(const std::string& v) { return value(v.c_str()); }

    Json& value(double v) {
        pre_value();
        if (!std::isfinite(v)) {
            // NaN / Inf 不是合法 JSON；写 0 而不是让调用方拿到坏数据。
            out_ += '0';
            return *this;
        }
        if (v == static_cast<double>(static_cast<long long>(v)) &&
            std::fabs(v) < 1e15) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
            out_ += buf;
        } else {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.6g", v);
            out_ += buf;
        }
        return *this;
    }
    Json& value(float v) { return value(static_cast<double>(v)); }
    Json& value(int v) { return value(static_cast<double>(v)); }
    Json& value(long long v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", v);
        pre_value();
        out_ += buf;
        return *this;
    }
    Json& value(unsigned long long v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%llu", v);
        pre_value();
        out_ += buf;
        return *this;
    }
    Json& value(bool v) {
        pre_value();
        out_ += v ? "true" : "false";
        return *this;
    }

    // 便捷写法：obj.kv("name", "Cube") / obj.kv("x", 1.0)
    template <typename T>
    Json& kv(const char* k, const T& v) { key(k); return value(v); }

    // 浮点数组（位置 / 四元数 / 颜色）。
    Json& array(const float* v, int n) {
        begin_array();
        for (int i = 0; i < n; ++i) value(v[i]);
        end_array();
        return *this;
    }

    const std::string& str() const { return out_; }

private:
    void open(char c) {
        pre_value();
        out_ += c;
        stack_.push_back(NeedsComma{false});
        after_key_ = false;
    }

    void close(char c) {
        if (!stack_.empty()) stack_.pop_back();
        out_ += c;
        after_key_ = false;
    }

    // 写值之前先决定要不要逗号：数组/对象的第二个元素起要加。
    void pre_value() {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (stack_.empty()) return;
        NeedsComma& top = stack_.back();
        if (top.written) {
            out_ += ',';
        } else {
            top.written = true;
        }
    }

    void append_string(const char* s) {
        out_ += '"';
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
            switch (*p) {
                case '"':  out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n";  break;
                case '\r': out_ += "\\r";  break;
                case '\t': out_ += "\\t";  break;
                default:
                    if (*p < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", *p);
                        out_ += buf;
                    } else {
                        // UTF-8 多字节按原样透传：JSON 允许，且省掉一遍编解码。
                        out_ += static_cast<char>(*p);
                    }
            }
        }
        out_ += '"';
    }

    struct NeedsComma {
        bool written = false;
    };

    std::string out_;
    std::vector<NeedsComma> stack_;
    bool after_key_ = false;
};

} // namespace gryce::editor

#endif // GRYCE_EDITOR_JSON_WRITER_H
