// engine/ui/ui_layout_tree_json.cpp — 布局树 JSON 往返的实现
// 约定（id 走字符串、按存储序读写、宽容读回）见 ui_layout_tree_json.h 头部。

#include "ui/ui_layout_tree_json.h"

#include <array>
#include <cstdint>

namespace gryce_ui {
namespace {

constexpr std::array<const char*, 4> kKindNames  = {"leaf", "column", "row", "grid"};
constexpr std::array<const char*, 4> kAlignNames = {"start", "center", "end", "stretch"};

// 十进制字符串。uint64 全精度过 JSON，不给 JS 的 53 位精度丢低位的機會。
std::string id_str(uint64_t v) {
    return std::to_string(v);
}

// 宽容取 id：字符串（十进制）/ 无符号整数 / 正整数都接；别的给 fallback。
// std::stoull 对空串与非数字串会抛，所以必须 try。
uint64_t id_from(const nlohmann::json& j, uint64_t fallback) {
    try {
        if (j.is_string())         return std::stoull(j.get<std::string>());
        if (j.is_number_unsigned()) return j.get<uint64_t>();
        if (j.is_number_integer()) {
            const int64_t v = j.get<int64_t>();
            return v > 0 ? static_cast<uint64_t>(v) : fallback;
        }
    } catch (...) {
        return fallback;
    }
    return fallback;
}

float f_from(const nlohmann::json& j, float fallback) {
    return (j.is_number() && !j.is_boolean()) ? j.get<float>() : fallback;
}

int i_from(const nlohmann::json& j, int fallback) {
    return j.is_number_integer() ? j.get<int>() : fallback;
}

// 四元组（margin / padding，上 右 下 左）。不是数组就整体保留原值——
// 宽容读，不让一个坏字段把整棵节点丢掉。
void read_quad(const nlohmann::json& j, float out[4]) {
    if (!j.is_array()) return;
    const std::size_t n = j.size() < 4 ? j.size() : 4;
    for (std::size_t i = 0; i < n; ++i) out[i] = f_from(j[i], out[i]);
}

nlohmann::json quad_json(const float v[4]) {
    return nlohmann::json::array({v[0], v[1], v[2], v[3]});
}

} // namespace

const char* layout_kind_name(UiLayoutKind k) {
    const int i = static_cast<int>(k);
    return (i >= 0 && i < static_cast<int>(kKindNames.size())) ? kKindNames[static_cast<size_t>(i)] : "";
}

bool layout_kind_from_name(const std::string& s, UiLayoutKind& out) {
    for (std::size_t i = 0; i < kKindNames.size(); ++i) {
        if (s == kKindNames[i]) {
            out = static_cast<UiLayoutKind>(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

const char* layout_align_name(UiLayoutAlign a) {
    const int i = static_cast<int>(a);
    return (i >= 0 && i < static_cast<int>(kAlignNames.size())) ? kAlignNames[static_cast<size_t>(i)] : "";
}

bool layout_align_from_name(const std::string& s, UiLayoutAlign& out) {
    for (std::size_t i = 0; i < kAlignNames.size(); ++i) {
        if (s == kAlignNames[i]) {
            out = static_cast<UiLayoutAlign>(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

nlohmann::json layout_tree_to_json(const UiLayoutTree& t) {
    nlohmann::json nodes = nlohmann::json::array();
    const int n = t.count();
    for (int i = 0; i < n; ++i) {
        const UiLayoutNode* c = t.node_at(i);
        if (!c) continue;
        nlohmann::json e = nlohmann::json::object();
        e["id"]      = id_str(c->id);
        e["parent"]  = id_str(c->parent);
        e["kind"]    = layout_kind_name(c->kind);
        e["rect"]    = nlohmann::json::array({c->rect.x, c->rect.y, c->rect.w, c->rect.h});
        e["margin"]  = quad_json(c->margin);
        e["flex"]    = c->flex;
        e["align"]   = layout_align_name(c->align);
        e["gap"]     = c->gap;
        e["padding"] = quad_json(c->padding);
        e["cols"]    = c->cols;
        nodes.push_back(std::move(e));
    }

    nlohmann::json j = nlohmann::json::object();
    j["version"] = 1;
    j["nodes"]   = std::move(nodes);
    return j;
}

bool layout_tree_from_json(const nlohmann::json& j, UiLayoutTree& out) {
    if (!j.is_object()) return false;
    const auto it = j.find("nodes");
    if (it == j.end() || !it->is_array()) return false;

    out.clear();
    for (const auto& e : *it) {
        if (!e.is_object()) continue;   // 宽容：坏条目跳过，不让它废掉整份文件

        UiLayoutNode c{};
        c.parent = id_from(e.value("parent", nlohmann::json()), 0);

        // kind / align 未知名按 Leaf / Start（宽容读，见头文件）
        UiLayoutKind  k = UiLayoutKind::Leaf;
        UiLayoutAlign a = UiLayoutAlign::Start;
        if (const auto kit = e.find("kind"); kit != e.end() && kit->is_string()) {
            layout_kind_from_name(kit->get<std::string>(), k);
        }
        if (const auto ait = e.find("align"); ait != e.end() && ait->is_string()) {
            layout_align_from_name(ait->get<std::string>(), a);
        }
        c.kind  = k;
        c.align = a;

        if (const auto rit = e.find("rect"); rit != e.end() && rit->is_array() && rit->size() >= 4) {
            c.rect.x = f_from((*rit)[0], c.rect.x);
            c.rect.y = f_from((*rit)[1], c.rect.y);
            c.rect.w = f_from((*rit)[2], c.rect.w);
            c.rect.h = f_from((*rit)[3], c.rect.h);
        }
        read_quad(e.value("margin", nlohmann::json()), c.margin);
        read_quad(e.value("padding", nlohmann::json()), c.padding);
        c.flex = f_from(e.value("flex", nlohmann::json()), c.flex);
        c.gap  = f_from(e.value("gap", nlohmann::json()), c.gap);
        c.cols = i_from(e.value("cols", nlohmann::json()), c.cols);

        // 失败（表满 / parent 不存在 / 会成环）就丢这个节点。读回顺序与输出
        // 顺序一致，所以前面的节点已经建好，后面的 parent 引用仍能解析。
        out.add(c);
    }
    return true;
}

} // namespace gryce_ui
