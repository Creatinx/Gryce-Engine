// ===========================================================================
// editor/ext/panel/model.cpp — 面板数据模型实现（读引擎，产出结构体）
//
// 本文件是"面板数据的唯一入口"。UI 层直接读结构体，JSON 层（panel.cpp）
// 也从结构体序列化，两边不会出现"UI 显示的和脚本读到的不一致"。
//
// 所有 rebuild_* 都会调用引擎 API（GEntity_* / GComponent_* / GAsset_*），
// 因此只能在 UI 回调**之外**调用 —— 回调运行在渲染器的锁内，从那里回头调
// 加锁的公开 API 会直接死锁。调用点在 ui::tick()，每帧一次。
// ===========================================================================

#include "panel/model.h"

#include "session/session.h"

#include "api/asset_api.h"
#include "api/component_api.h"
#include "api/core_api.h"
#include "api/entity_api.h"
#include "api/render_api.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_map>

namespace gryce::editor::panel {

namespace {

std::vector<HierarchyNode> g_hierarchy;
InspectorData g_inspector;
AssetList g_assets;
StatsData g_stats;

std::string g_asset_dir = "res:/models";
double g_asset_stamp_ms = 0.0;
bool g_asset_dirty = true;

double now_ms() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

// 枚举全部实体。GEntity_GetCount / GetAt 统计的是"场景里所有实体"，但为了
// 不依赖这个实现细节，下面用 parent 是否在集合里来判断根，而不是假设
// GetParent 对根返回 0。
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

std::string short_component_name(const char* qualified) {
    // 引擎给的是带命名空间的全名（gryce_engine::components::MeshRenderer）。
    // 面板只显示最后一段：全名会把列表撑满，命名空间对用户也没意义。
    const std::string full = qualified ? qualified : "";
    const std::size_t sep = full.rfind("::");
    return sep == std::string::npos ? full : full.substr(sep + 2);
}

void build_children(HierarchyNode& node, int depth,
                    const std::unordered_map<GEntityHandle, std::vector<GEntityHandle>>& tree) {
    if (depth > 64) return;   // 深度上限兜底：真出环时也不把栈打爆
    auto it = tree.find(node.handle);
    if (it == tree.end()) return;

    node.children.reserve(it->second.size());
    for (GEntityHandle child : it->second) {
        HierarchyNode cn;
        cn.handle = child;
        cn.depth = node.depth + 1;
        char name[128] = {};
        GEntity_GetName(child, name, static_cast<int>(sizeof(name)));
        cn.name = name;
        build_children(cn, depth + 1, tree);
        node.children.push_back(std::move(cn));
    }
}

void read_property(GEntityHandle handle, uint64_t hash, const char* name,
                   int type, int size, PropertyValue* out) {
    // 缓冲区按最大字段取值：四元数 16 字节、字符串可能到 512（路径）。
    // 超过 512 的字符串字段会被截断，读写侧都按这个上限约定。
    char buf[512] = {};
    const int cap = static_cast<int>(sizeof(buf));
    out->name = name;
    out->type = type;
    out->size = size;
    if (GComponent_GetProperty(handle, hash, name, buf, cap) != 0) return;

    switch (type) {
        case kInt: {
            int32_t v = 0;
            std::memcpy(&v, buf, sizeof(v));
            out->number = static_cast<double>(v);
            break;
        }
        case kFloat: {
            float v = 0.0f;
            std::memcpy(&v, buf, sizeof(v));
            out->number = static_cast<double>(v);
            break;
        }
        case kDouble:
            std::memcpy(&out->number, buf, sizeof(double));
            break;
        case kBool: {
            bool v = false;
            std::memcpy(&v, buf, sizeof(v));
            out->boolean = v;
            break;
        }
        case kString:
            out->text = buf;
            break;
        case kVec2: {
            std::memcpy(out->vec, buf, std::min<std::size_t>(size, sizeof(float) * 2));
            out->count = 2;
            break;
        }
        case kVec3: {
            std::memcpy(out->vec, buf, std::min<std::size_t>(size, sizeof(float) * 3));
            out->count = 3;
            break;
        }
        case kVec3i: {
            // 整数三元组在结构体里也按 float 存（显示用），整数精度到 2^24
            // 足够覆盖 UI 上的任何取值；要精确值走 JSON 侧的整型分支。
            int32_t v[3] = {};
            std::memcpy(v, buf, std::min<std::size_t>(size, sizeof(v)));
            out->vec[0] = static_cast<float>(v[0]);
            out->vec[1] = static_cast<float>(v[1]);
            out->vec[2] = static_cast<float>(v[2]);
            out->count = 3;
            break;
        }
        case kVec4:
        case kQuat:
        case kColor:
            std::memcpy(out->vec, buf, std::min<std::size_t>(size, sizeof(float) * 4));
            out->count = 4;
            break;
        default:
            // 未知类型（枚举、引用等）不猜，留空让 UI 显示成只读。
            break;
    }
}

int collect_asset(void* user, const GAssetEntry* entry) {
    auto* list = static_cast<AssetList*>(user);
    if (!entry) return 0;
    AssetItem item;
    item.path = entry->path ? entry->path : "";
    item.name = entry->name ? entry->name : "";
    item.ext = entry->ext ? entry->ext : "";
    item.kind = entry->kind ? entry->kind : "other";
    item.guid = entry->guid ? entry->guid : "";
    item.size = entry->size;
    item.has_meta = entry->has_meta != 0;
    item.stale = entry->stale != 0;
    list->items.push_back(std::move(item));
    return 1;
}

} // namespace

void rebuild_hierarchy() {
    g_hierarchy.clear();

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

    g_hierarchy.reserve(roots.size());
    for (GEntityHandle root : roots) {
        HierarchyNode node;
        node.handle = root;
        node.depth = 0;
        char name[128] = {};
        GEntity_GetName(root, name, static_cast<int>(sizeof(name)));
        node.name = name;
        build_children(node, 0, tree);
        g_hierarchy.push_back(std::move(node));
    }
}

void rebuild_inspector(GEntityHandle handle) {
    g_inspector = InspectorData{};
    if (handle <= 0 || !session::handle_alive(handle)) return;

    g_inspector.valid = true;
    g_inspector.handle = handle;
    g_inspector.parent = GEntity_GetParent(handle);

    char name[128] = {};
    GEntity_GetName(handle, name, static_cast<int>(sizeof(name)));
    g_inspector.name = name;

    GVec3 pos{};
    GQuat rot{};
    GVec3 scale{1.0f, 1.0f, 1.0f};
    GEntity_GetLocalPosition(handle, &pos);
    GEntity_GetLocalRotation(handle, &rot);
    GEntity_GetLocalScale(handle, &scale);
    g_inspector.position[0] = pos.x;
    g_inspector.position[1] = pos.y;
    g_inspector.position[2] = pos.z;
    g_inspector.rotation[0] = rot.x;
    g_inspector.rotation[1] = rot.y;
    g_inspector.rotation[2] = rot.z;
    g_inspector.rotation[3] = rot.w;
    g_inspector.scale[0] = scale.x;
    g_inspector.scale[1] = scale.y;
    g_inspector.scale[2] = scale.z;

    const int comp_count = GComponent_GetCount(handle);
    g_inspector.components.reserve(static_cast<std::size_t>(comp_count > 0 ? comp_count : 0));
    for (int i = 0; i < comp_count; ++i) {
        char type_name[64] = {};
        if (GComponent_GetTypeNameAt(handle, i, type_name,
                                     static_cast<int>(sizeof(type_name))) < 0) {
            continue;
        }
        uint64_t hash = 0;
        if (GComponent_GetTypeHashAt(handle, i, &hash) != 0) continue;

        ComponentData comp;
        comp.qualified_name = type_name;
        comp.name = short_component_name(type_name);
        comp.hash = hash;

        const int prop_count = GComponent_GetPropertyCount(handle, hash);
        comp.properties.reserve(static_cast<std::size_t>(prop_count > 0 ? prop_count : 0));
        for (int p = 0; p < prop_count; ++p) {
            char prop_name[64] = {};
            int type = -1;
            int size = 0;
            if (GComponent_GetPropertyInfo(handle, hash, p, prop_name,
                                           static_cast<int>(sizeof(prop_name)),
                                           &type, &size) != 0) {
                continue;
            }
            PropertyValue value;
            read_property(handle, hash, prop_name, type, size, &value);
            comp.properties.push_back(std::move(value));
        }
        g_inspector.components.push_back(std::move(comp));
    }
}

void rebuild_assets(const char* dir) {
    std::string next_dir = (dir && dir[0]) ? dir : g_asset_dir;
    const double t = now_ms();

    // 250ms 缓存：面板每帧都问一次，60 次/秒扫盘会明显拖慢帧；但"同一个目录
    // 连问"和"用户真的切了目录"必须区分开 —— 切目录一律立即重扫。
    if (!g_asset_dirty && next_dir == g_asset_dir && (t - g_asset_stamp_ms) < 250.0) {
        return;
    }

    g_asset_dir = next_dir;
    g_asset_stamp_ms = t;
    g_asset_dirty = false;

    g_assets.dir = g_asset_dir;
    g_assets.items.clear();
    g_assets.scanned = false;

    // 直接吃访客回调，不再解析自己刚生成的 JSON：条目上千时每秒解析一遍
    // 会掉帧，而且解析失败的表现是"列表空白"，很难查。
    const int visited = GAsset_ScanDirectoryEach(g_asset_dir.c_str(), 0, &collect_asset, &g_assets);
    g_assets.scanned = visited >= 0;
}

void rebuild_stats() {
    const auto& sel = session::selection();
    g_stats.entities = GEntity_GetCount();
    g_stats.selected = sel.primary;
    g_stats.selection_count = static_cast<int>(sel.items.size());
    g_stats.undo = GCore_GetUndoCount();
    g_stats.redo = GCore_GetRedoCount();
    g_stats.dirty = session::dirty();
    g_stats.playing = GCore_IsPlaying() != 0;
    g_stats.paused = GCore_IsPaused() != 0;
    g_stats.fps = GRender_GetFPS();
    g_stats.gpu_ms = GRender_GetGPUFrameTimeMs();
}

const std::vector<HierarchyNode>& hierarchy() { return g_hierarchy; }
const InspectorData& inspector() { return g_inspector; }
const AssetList& assets() { return g_assets; }
const StatsData& stats() { return g_stats; }

const char* asset_dir() { return g_asset_dir.c_str(); }

void invalidate_assets() { g_asset_dirty = true; }

} // namespace gryce::editor::panel
