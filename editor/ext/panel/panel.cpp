// ===========================================================================
// editor/ext/panel/panel.cpp — 面板数据的 JSON 序列化
//
// 取数据在 model.cpp（读引擎、产出结构体），这里只负责把结构体写成 JSON。
// 分开的理由见 model.h 的文件头：UI 与脚本消费同一份数据，但不能让 UI
// 每帧解析自己刚生成的字符串。
// ===========================================================================

#include "panel/panel.h"

#include "asset/asset_index.h"
#include "command/commands.h"
#include "panel/model.h"

#include "api/core_api.h"

#include "common/json_writer.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace gryce::editor::panel {

namespace {

// 帧级缓存：面板在同一个 on_update 里可能被 UI 层问两三次（布局测量 + 绘制），
// 每次重算一遍层级树既浪费又会让两次结果不一致（枚举期间有实体被删）。
bool g_hierarchy_cached = false;
std::string g_hierarchy_json;

// 64 位哈希写成十进制文本。JSON 数字是 IEEE-754 double，超过 2^53 的值
// 经 JS 的 JSON.parse 就变成了另一个数；字符串形式桥接层能精确读回。
std::string hash_text(uint64_t hash) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

void write_node(Json& j, const HierarchyNode& node) {
    j.begin_object();
    j.kv("handle", node.handle);
    j.kv("name", node.name);
    j.kv("depth", node.depth);
    j.key("children");
    j.begin_array();
    // 数组必须无条件开闭：调用方已经写好了 "children": 这个键，提前返回会
    // 留下一个悬空键，产出的 JSON 直接非法（脚本侧表现为 unexpected token）。
    for (const HierarchyNode& child : node.children) write_node(j, child);
    j.end_array();
    j.end_object();
}

void write_property(Json& j, const PropertyValue& p) {
    j.begin_object();
    j.kv("name", p.name);
    j.kv("type", p.type);
    j.kv("size", p.size);
    j.key("value");
    switch (p.type) {
        case kInt:
        case kVec3i:
            if (p.type == kVec3i) {
                j.begin_array();
                for (int i = 0; i < p.count; ++i) {
                    j.value(static_cast<long long>(std::llround(p.vec[i])));
                }
                j.end_array();
            } else {
                j.value(static_cast<long long>(std::llround(p.number)));
            }
            break;
        case kFloat:
        case kDouble:
            j.value(p.number);
            break;
        case kBool:
            j.value(p.boolean);
            break;
        case kString:
            j.value(p.text);
            break;
        case kVec2:
        case kVec3:
        case kVec4:
        case kQuat:
        case kColor:
            j.array(p.vec, p.count);
            break;
        default:
            // 未知类型不猜，给空串让前端显示成只读。
            j.value("");
            break;
    }
    j.end_object();
}

} // namespace

void reset() {
    g_hierarchy_cached = false;
    g_hierarchy_json.clear();
}

void tick() { g_hierarchy_cached = false; }

std::string hierarchy_json() {
    if (g_hierarchy_cached) return g_hierarchy_json;

    rebuild_hierarchy();
    rebuild_stats();
    const std::vector<HierarchyNode>& roots = hierarchy();

    Json j;
    j.begin_object();
    j.kv("ok", 1);
    j.kv("count", static_cast<long long>(stats().entities));
    j.key("roots");
    j.begin_array();
    for (const HierarchyNode& root : roots) write_node(j, root);
    j.end_array();
    j.end_object();

    g_hierarchy_json = j.str();
    g_hierarchy_cached = true;
    return g_hierarchy_json;
}

std::string inspector_json(GEntityHandle handle) {
    // 模型只保留"当前检视的实体"一份。换了目标就在读引擎（本函数运行在
    // tick 路径上，不在 UI 回调里），否则序列化出来的会是上一个实体。
    if (handle != inspector().handle || (handle > 0 && !inspector().valid)) {
        rebuild_inspector(handle);
    }
    const InspectorData& in = inspector();

    Json j;
    j.begin_object();
    if (!in.valid) {
        j.kv("ok", 0);
        j.end_object();
        return j.str();
    }

    j.kv("ok", 1);
    j.kv("handle", in.handle);
    j.kv("name", in.name);
    j.kv("parent", in.parent);

    j.key("transform");
    j.begin_object();
    j.key("position");
    j.array(in.position, 3);
    j.key("rotation");
    j.array(in.rotation, 4);
    j.key("scale");
    j.array(in.scale, 3);
    j.end_object();

    j.key("components");
    j.begin_array();
    for (const ComponentData& comp : in.components) {
        j.begin_object();
        j.kv("name", comp.name);
        j.kv("qualifiedName", comp.qualified_name);
        // 哈希走**十进制字符串**：64 位值经 JSON 数字再被 JS 解析只剩 53 位
        // 精度，读回去是个不同的数，表现为"类型查得到、属性写不进去"。
        // 桥接层（js_args 的 u64）字符串优先，传回去正好。
        j.kv("hash", hash_text(comp.hash));
        j.kv("removable", comp.removable);
        j.key("properties");
        j.begin_array();
        for (const PropertyValue& p : comp.properties) write_property(j, p);
        j.end_array();
        j.end_object();
    }
    j.end_array();

    // 还能挂哪些组件。UI 用它做 Add Component 菜单，测试用它确认
    // "删掉一个组件之后它重新出现在候选里"。
    j.key("availableComponents");
    j.begin_array();
    for (const ComponentTypeInfo& info : in.available_components) {
        j.begin_object();
        j.kv("name", info.name);
        j.kv("shortName", info.short_name);
        j.kv("category", info.category);
        j.kv("hash", hash_text(info.hash));
        j.end_object();
    }
    j.end_array();

    j.end_object();
    return j.str();
}

std::string assets_json(const char* dir) {
    rebuild_assets(dir);
    const AssetList& list = assets();

    // listing 是引擎列举的原始 JSON（脚本侧要"全部字段"时直接用它），
    // items 是同一批条目的结构化副本（UI 与断言都读这个）。
    const char* raw = asset::scan(list.dir.c_str(), 0);

    Json j;
    j.begin_object();
    j.kv("ok", list.scanned ? 1 : 0);
    j.kv("source", list.dir);
    j.kv("count", static_cast<long long>(list.items.size()));
    j.kv("listing", raw);
    j.key("items");
    j.begin_array();
    for (const AssetItem& item : list.items) {
        j.begin_object();
        j.kv("path", item.path);
        j.kv("name", item.name);
        j.kv("ext", item.ext);
        j.kv("kind", item.kind);
        j.kv("guid", item.guid);
        j.kv("size", static_cast<unsigned long long>(item.size));
        j.kv("hasMeta", item.has_meta);
        j.kv("stale", item.stale);
        j.end_object();
    }
    j.end_array();
    j.end_object();
    return j.str();
}

std::string stats_json() {
    rebuild_stats();
    const StatsData& s = stats();

    Json j;
    j.begin_object();
    j.kv("entities", s.entities);
    j.kv("selected", static_cast<long long>(s.selected));
    j.kv("selectionCount", s.selection_count);
    j.kv("undo", s.undo);
    j.kv("redo", s.redo);
    j.kv("dirty", s.dirty);
    j.kv("playing", s.playing);
    j.kv("paused", s.paused);
    j.kv("fps", s.fps);
    j.kv("gpuMs", s.gpu_ms);
    j.kv("group", commands::current_group_label());
    j.end_object();
    return j.str();
}

} // namespace gryce::editor::panel
