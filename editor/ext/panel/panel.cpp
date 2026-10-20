// ===========================================================================
// editor/ext/panel/panel.cpp — 面板数据模型实现
// ===========================================================================

#include "panel/panel.h"

#include "asset/asset_index.h"
#include "command/commands.h"
#include "session/session.h"

#include "api/component_api.h"
#include "api/core_api.h"
#include "api/entity_api.h"
#include "api/render_api.h"

#include "common/json_writer.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace gryce::editor::panel {

namespace {

// 帧级缓存：面板在同一个 on_update 里可能被 UI 层问两三次（布局测量 + 绘制），
// 每次重算一遍层级树既浪费又会让两次结果不一致（枚举期间有实体被删）。
struct FrameCache {
    bool valid = false;
    std::string hierarchy;
};
FrameCache g_cache;

// 组件属性类型编码，与 engine/api/component_api.cpp 的 field_type_code 对齐。
enum : int {
    kInt = 0, kFloat = 1, kDouble = 2, kBool = 3, kString = 4,
    kVec2 = 5, kVec3 = 6, kVec3i = 7, kVec4 = 8, kQuat = 9, kColor = 10,
};

void write_property_value(Json& j, GEntityHandle handle, uint64_t hash,
                          const char* name, int type, int size) {
    // 缓冲区按最大字段取值：四元数 16 字节、字符串可能到 512（路径）。
    // 超过 512 的字符串字段（基本不会出现）会被截断，读写侧都按这个上限约定。
    char buf[512] = {};
    const int cap = static_cast<int>(sizeof(buf));
    if (GComponent_GetProperty(handle, hash, name, buf, cap) != 0) {
        j.value("");
        return;
    }
    const void* raw = buf;

    switch (type) {
        case kInt: {
            int32_t v = 0;
            std::memcpy(&v, raw, sizeof(v));
            j.value(static_cast<long long>(v));
            break;
        }
        case kFloat: {
            float v = 0.0f;
            std::memcpy(&v, raw, sizeof(v));
            j.value(v);
            break;
        }
        case kDouble: {
            double v = 0.0;
            std::memcpy(&v, raw, sizeof(v));
            j.value(v);
            break;
        }
        case kBool: {
            bool v = false;
            std::memcpy(&v, raw, sizeof(v));
            j.value(v);
            break;
        }
        case kString:
            j.value(buf);
            break;
        case kVec2: {
            float v[2] = {};
            std::memcpy(v, raw, std::min<std::size_t>(size, sizeof(v)));
            j.array(v, 2);
            break;
        }
        case kVec3: {
            float v[3] = {};
            std::memcpy(v, raw, std::min<std::size_t>(size, sizeof(v)));
            j.array(v, 3);
            break;
        }
        case kVec3i: {
            int32_t v[3] = {};
            std::memcpy(v, raw, std::min<std::size_t>(size, sizeof(v)));
            j.begin_array();
            for (int i = 0; i < 3; ++i) j.value(static_cast<long long>(v[i]));
            j.end_array();
            break;
        }
        case kVec4:
        case kQuat:
        case kColor: {
            float v[4] = {};
            std::memcpy(v, raw, std::min<std::size_t>(size, sizeof(v)));
            j.array(v, 4);
            break;
        }
        default:
            // 未知类型（枚举、引用等）不猜，给 null 让 UI 显示成只读。
            j.value("");
            break;
    }
}

// 枚举全部实体。注意 GEntity_GetCount / GetAt 只统计"有父节点"的实体 ——
// 场景里所有实体都挂在隐藏的 root 下，所以这是全量；但为了不依赖这个
// 实现细节，下面用 parent 是否在集合里来判断根，而不是假设 GetParent 返回 0。
std::vector<GEntityHandle> all_handles() {
    std::vector<GEntityHandle> out;
    const int n = GEntity_GetCount();
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const GEntityHandle h = GEntity_GetAt(i);
        if (h > 0) out.push_back(h);
    }
    return out;
}

void write_children(Json& j, GEntityHandle parent,
                    const std::unordered_map<GEntityHandle, std::vector<GEntityHandle>>& tree,
                    int depth) {
    // 数组必须无条件开闭：调用方已经写好了 "children": 这个键，
    // 这里提前 return 会留下一个悬空键，产出的 JSON 直接非法
    // （表现为脚本侧 JSON.parse 报 unexpected token '}'）。
    j.begin_array();
    if (depth <= 64) {   // 深度上限兜底：序列化出环时不至于把栈打爆
        auto it = tree.find(parent);
        if (it != tree.end()) {
            for (GEntityHandle child : it->second) {
                char name[128] = {};
                GEntity_GetName(child, name, static_cast<int>(sizeof(name)));

                j.begin_object();
                j.kv("handle", child);
                j.kv("name", name);
                j.key("children");
                write_children(j, child, tree, depth + 1);
                j.end_object();
            }
        }
    }
    j.end_array();
}

} // namespace

void reset() { g_cache = FrameCache{}; }

void tick() { g_cache.valid = false; }

std::string hierarchy_json() {
    if (g_cache.valid) return g_cache.hierarchy;

    const std::vector<GEntityHandle> handles = all_handles();

    std::unordered_map<GEntityHandle, bool> known;
    known.reserve(handles.size() * 2);
    for (GEntityHandle h : handles) known[h] = true;

    std::unordered_map<GEntityHandle, std::vector<GEntityHandle>> tree;
    std::vector<GEntityHandle> roots;
    for (GEntityHandle h : handles) {
        const GEntityHandle parent = GEntity_GetParent(h);
        if (parent > 0 && known.count(parent)) {
            tree[parent].push_back(h);
        } else {
            roots.push_back(h);
        }
    }

    Json j;
    j.begin_object();
    j.kv("ok", 1);
    j.kv("count", static_cast<long long>(handles.size()));
    j.key("roots");
    j.begin_array();
    for (GEntityHandle root : roots) {
        char name[128] = {};
        GEntity_GetName(root, name, static_cast<int>(sizeof(name)));
        j.begin_object();
        j.kv("handle", root);
        j.kv("name", name);
        j.key("children");
        write_children(j, root, tree, 0);
        j.end_object();
    }
    j.end_array();
    j.end_object();

    g_cache.hierarchy = j.str();
    g_cache.valid = true;
    return g_cache.hierarchy;
}

std::string inspector_json(GEntityHandle handle) {
    Json j;
    j.begin_object();
    if (handle <= 0 || !session::handle_alive(handle)) {
        j.kv("ok", 0);
        j.end_object();
        return j.str();
    }

    char name[128] = {};
    GEntity_GetName(handle, name, static_cast<int>(sizeof(name)));

    GVec3 pos{};
    GQuat rot{};
    GVec3 scale{1.0f, 1.0f, 1.0f};
    GEntity_GetLocalPosition(handle, &pos);
    GEntity_GetLocalRotation(handle, &rot);
    GEntity_GetLocalScale(handle, &scale);

    j.kv("ok", 1);
    j.kv("handle", handle);
    j.kv("name", name);
    j.kv("parent", GEntity_GetParent(handle));

    j.key("transform");
    j.begin_object();
    j.key("position");
    j.array(&pos.x, 3);
    j.key("rotation");
    j.array(&rot.x, 4);
    j.key("scale");
    j.array(&scale.x, 3);
    j.end_object();

    j.key("components");
    j.begin_array();
    const int comp_count = GComponent_GetCount(handle);
    for (int i = 0; i < comp_count; ++i) {
        char type_name[64] = {};
        if (GComponent_GetTypeNameAt(handle, i, type_name,
                                     static_cast<int>(sizeof(type_name))) < 0) {
            continue;
        }
        uint64_t hash = 0;
        if (GComponent_GetTypeHashAt(handle, i, &hash) != 0) continue;

        // 引擎给的是带命名空间的全名（gryce_engine::components::MeshRenderer）。
        // 面板上只显示最后一段：全名在列表里会撑满宽度，且命名空间对用户没意义。
        const char* short_name = type_name;
        for (const char* p = type_name; *p; ++p) {
            if (p[0] == ':' && p[1] == ':') short_name = p + 2;
        }

        j.begin_object();
        j.kv("name", short_name);
        j.kv("qualifiedName", type_name);
        j.kv("hash", static_cast<unsigned long long>(hash));
        j.key("properties");
        j.begin_array();

        const int prop_count = GComponent_GetPropertyCount(handle, hash);
        for (int p = 0; p < prop_count; ++p) {
            char prop_name[64] = {};
            int type = -1;
            int size = 0;
            if (GComponent_GetPropertyInfo(handle, hash, p, prop_name,
                                           static_cast<int>(sizeof(prop_name)),
                                           &type, &size) != 0) {
                continue;
            }
            j.begin_object();
            j.kv("name", prop_name);
            j.kv("type", type);
            j.kv("size", size);
            j.key("value");
            write_property_value(j, handle, hash, prop_name, type, size);
            j.end_object();
        }
        j.end_array();
        j.end_object();
    }
    j.end_array();
    j.end_object();
    return j.str();
}

std::string assets_json(const char* dir) {
    const char* raw = asset::scan(dir && dir[0] ? dir : "res:/models", 0);

    // 引擎给的是平铺列表，这里按 kind 分组。分组只是为了界面好排，
    // 每个条目原样带过去，不增删字段。
    Json j;
    j.begin_object();
    j.kv("ok", 1);
    j.kv("source", dir ? dir : "res:/models");
    // 原样转发引擎结果：UI 若想拿全部字段（guid / stale）直接用这个。
    j.kv("listing", raw);
    j.end_object();
    return j.str();
}

std::string stats_json() {
    const auto& sel = session::selection();
    Json j;
    j.begin_object();
    j.kv("entities", GEntity_GetCount());
    j.kv("selected", static_cast<long long>(sel.primary));
    j.kv("selectionCount", static_cast<long long>(sel.items.size()));
    j.kv("undo", GCore_GetUndoCount());
    j.kv("redo", GCore_GetRedoCount());
    j.kv("dirty", session::dirty());
    j.kv("playing", GCore_IsPlaying());
    j.kv("paused", GCore_IsPaused());
    j.kv("fps", GRender_GetFPS());
    j.kv("gpuMs", GRender_GetGPUFrameTimeMs());
    j.kv("group", commands::current_group_label());
    j.end_object();
    return j.str();
}

} // namespace gryce::editor::panel
