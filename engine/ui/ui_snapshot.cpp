// ===========================================================================
// engine/ui/ui_snapshot.cpp — 面板视图模型的序列化实现
//
// 只依赖 nlohmann/json（引擎其余部分已在用的头文件库），不碰 ImGui、
// 不碰 GPU——这个文件的可编译性本身就是"UI 模型层与渲染层解耦"的证明。
// ===========================================================================

#include "ui/ui_snapshot.h"

#include <cstring>

namespace gryce_ui {

bool Panel::add(const Row& r) {
    if (row_count >= kMaxRows) return false;
    rows[row_count++] = r;
    return true;
}

const char* row_kind_name(RowKind k) {
    switch (k) {
        case RowKind::Label:     return "label";
        case RowKind::Button:    return "button";
        case RowKind::Checkbox:  return "checkbox";
        case RowKind::Slider:    return "slider";
        case RowKind::Drag:      return "drag";
        case RowKind::Text:      return "text";
        case RowKind::Separator: return "separator";
        case RowKind::Custom:    return "custom";
    }
    return "label";
}

bool row_kind_from_name(const char* s, RowKind& out) {
    if (!s) return false;
    for (uint32_t i = 0; i <= static_cast<uint32_t>(RowKind::Custom); ++i) {
        const auto k = static_cast<RowKind>(i);
        if (std::strcmp(row_kind_name(k), s) == 0) {
            out = k;
            return true;
        }
    }
    return false;
}

namespace {

// 固定字符数组的安全赋值：截断到容量-1 并补 '\0'。
template <size_t N>
void copy_text(char (&dst)[N], const std::string& src) {
    const size_t n = src.size() < N - 1 ? src.size() : N - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

float json_float(const nlohmann::json& j, const char* key, float def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<float>();
}

// 读整数；也接受布尔（to_json 把 checkbox 的 flag 写成 bool，
// 而 nlohmann 的 is_number_integer() 不含 is_boolean()）。
int64_t json_int(const nlohmann::json& j, const char* key, int64_t def) {
    const auto it = j.find(key);
    if (it == j.end()) return def;
    if (it->is_boolean()) return it->get<bool>() ? 1 : 0;
    if (!it->is_number_integer()) return def;
    return it->get<int64_t>();
}

} // namespace

nlohmann::json to_json(const Panel& p) {
    nlohmann::json j = nlohmann::json::object();
    j["title"] = p.title;
    nlohmann::json rows = nlohmann::json::array();
    for (size_t i = 0; i < p.row_count; ++i) {
        const Row& r = p.rows[i];
        nlohmann::json rj = nlohmann::json::object();
        rj["kind"]  = row_kind_name(r.kind);
        // 十进制字符串：JS JSON.parse 只有 53 位整数精度，数字形式会丢低位
        rj["id"]    = std::to_string(r.id);
        rj["label"] = r.label;
        switch (r.kind) {
            case RowKind::Slider:
                rj["value"] = r.value;
                rj["min"]   = r.min_v;
                rj["max"]   = r.max_v;
                break;
            case RowKind::Checkbox:
                rj["flag"] = (r.flag != 0);
                break;
            case RowKind::Text:
            case RowKind::Custom:
                rj["text"] = r.text;
                break;
            default:
                break;
        }
        rows.push_back(std::move(rj));
    }
    j["rows"] = std::move(rows);
    return j;
}

bool from_json(const nlohmann::json& j, Panel& out) {
    if (!j.is_object()) return false;
    out.clear();

    const auto title = j.find("title");
    if (title != j.end() && title->is_string()) {
        copy_text(out.title, title->get<std::string>());
    }

    const auto rows = j.find("rows");
    if (rows == j.end() || !rows->is_array()) return true;   // 无 rows 视为空面板

    for (const nlohmann::json& rj : *rows) {
        if (!rj.is_object()) continue;
        Row r;
        const auto kind = rj.find("kind");
        if (kind != rj.end() && kind->is_string()) {
            std::string name = kind->get<std::string>();
            if (!row_kind_from_name(name.c_str(), r.kind)) continue;   // 坏行跳过
        }
        const auto id = rj.find("id");
        if (id != rj.end()) {
            if (id->is_string()) {
                // 十进制字符串（to_json 的输出）；也容忍数字形式（C++ 侧读回）
                try {
                    r.id = std::stoull(id->get<std::string>());
                } catch (...) {
                    continue;
                }
            } else if (id->is_number_unsigned()) {
                r.id = id->get<uint64_t>();
            }
        }
        const auto label = rj.find("label");
        if (label != rj.end() && label->is_string()) {
            copy_text(r.label, label->get<std::string>());
        }
        r.value = json_float(rj, "value", r.value);
        r.min_v = json_float(rj, "min", r.min_v);
        r.max_v = json_float(rj, "max", r.max_v);
        r.flag  = static_cast<int32_t>(json_int(rj, "flag", r.flag));
        const auto text = rj.find("text");
        if (text != rj.end() && text->is_string()) {
            copy_text(r.text, text->get<std::string>());
        }
        if (!out.add(r)) break;   // 满：截断（调用方可见 row_count）
    }
    return true;
}

} // namespace gryce_ui
