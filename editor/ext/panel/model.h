// ===========================================================================
// editor/ext/panel/model.h — 面板数据模型（结构体）
//
// 为什么不是"直接产出 JSON 就完事"：面板数据有两个消费者 —— 外面的 JS
// （脚本 / 测试 / 其它宿主，要 JSON）和编辑器自己的 UI（要能直接遍历的结构）。
// 只做 JSON 的话，UI 每帧都得解析一遍自己刚生成的字符串，既慢又能凭空多出
// "解析失败 → 列表空白"这种难查的故障。所以这里把"取数据"和"写成 JSON"分开：
//
//   rebuild_*()  读引擎（tick 里调用，可以在任何持锁路径之外）
//   结构体        唯一真相，JSON 与 UI 都从它派生
//   panel.cpp    JSON 序列化
//
// 生命周期：结构体在每次 rebuild 时整体重建，指针/引用只在当帧有效。UI 回调
// 里只读不建 —— 回调内禁止调用引擎 API（渲染器锁已持有），重建必须在 tick。
// ===========================================================================

#ifndef GRYCE_EDITOR_PANEL_MODEL_H
#define GRYCE_EDITOR_PANEL_MODEL_H

#include "api/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gryce::editor::panel {

// 组件属性类型编码，与 engine/api/component_api.cpp 的 field_type_code 对齐。
enum PropertyType : int {
    kInt = 0, kFloat = 1, kDouble = 2, kBool = 3, kString = 4,
    kVec2 = 5, kVec3 = 6, kVec3i = 7, kVec4 = 8, kQuat = 9, kColor = 10,
};

// 一个属性值。用"宽字段 + 有效个数"而不是 union：属性总数在千级以内，
// 多占几十字节换掉一整类错位 bug 是划算的。
struct PropertyValue {
    std::string name;
    int type = -1;
    int size = 0;
    double number = 0.0;      // int / float / double
    bool boolean = false;     // bool
    std::string text;         // string
    float vec[4] = {0, 0, 0, 0};  // vec2 / vec3 / vec3i / vec4 / quat / color
    int count = 0;            // vec 的有效元素个数
};

struct ComponentData {
    std::string name;            // 短名（MeshRenderer）
    std::string qualified_name;  // 引擎注册名（gryce_engine::components::MeshRenderer）
    uint64_t hash = 0;
    // 引擎内部维护的组件（Transform / ParentComponent / ChildrenComponent /
    // PrefabInstance）不能删：它们记的是实体自身的结构，删掉之后层级、
    // 序列化都对不上，而引擎不会拒绝。
    bool removable = true;
    std::vector<PropertyValue> properties;
};

// 可以挂到实体上的组件类型（"Add Component" 菜单的候选）。
// 已经从注册表里剔掉了内部组件，见 model.cpp 的 is_internal_type()。
struct ComponentTypeInfo {
    uint64_t hash = 0;
    std::string name;         // 注册名
    std::string short_name;   // 去掉命名空间后的短名
    std::string category;     // "Node3D" / "Node2D" / "Other"
};

struct HierarchyNode {
    GEntityHandle handle = 0;
    std::string name;
    int depth = 0;
    std::vector<HierarchyNode> children;
};

struct InspectorData {
    bool valid = false;
    GEntityHandle handle = 0;
    GEntityHandle parent = 0;
    std::string name;
    float position[3] = {0, 0, 0};
    float rotation[4] = {0, 0, 0, 1};
    float scale[3] = {1, 1, 1};
    std::vector<ComponentData> components;
    // 还没挂到这个实体上的组件类型（已按分类分组前的扁平列表）。
    std::vector<ComponentTypeInfo> available_components;
};

struct AssetItem {
    std::string path;
    std::string name;
    std::string ext;
    std::string kind;
    std::string guid;
    unsigned long long size = 0;
    bool has_meta = false;
    bool stale = false;
};

struct AssetList {
    std::string dir;              // 当前列举的目录（res:/ 形式）
    std::vector<AssetItem> items;
    bool scanned = false;         // false = 目录不存在或还没扫过
};

struct StatsData {
    int entities = 0;
    int undo = 0;
    int redo = 0;
    GEntityHandle selected = 0;
    int selection_count = 0;
    bool dirty = false;
    bool playing = false;
    bool paused = false;
    float fps = 0.0f;
    float gpu_ms = 0.0f;
};

// ---------------------------------------------------------------------------
// 重建（读引擎，只能在 UI 回调之外调用）
// ---------------------------------------------------------------------------

void rebuild_hierarchy();
void rebuild_inspector(GEntityHandle handle);
// 目录相同且距上次扫描不足 250ms 时直接复用上次结果（60 次/秒扫盘会拖慢帧）。
void rebuild_assets(const char* dir);
void rebuild_stats();

// ---------------------------------------------------------------------------
// 读取（UI 用；每帧同一份，遍历期间不会变）
// ---------------------------------------------------------------------------

const std::vector<HierarchyNode>& hierarchy();
const InspectorData& inspector();
const AssetList& assets();
const StatsData& stats();

// 全部可挂载的组件类型（已剔除内部组件）。注册表在进程内只建一次，
// 这里再缓存一层是因为 rebuild_inspector 每帧都要按它算"还差哪些没挂"。
const std::vector<ComponentTypeInfo>& component_types();
// 插件在运行中注册了新组件时调用。目前没有热注册的路径，留着是为了
// 避免以后有人在 rebuild 里无条件扫注册表（66 个类型 × 每帧 = 白扫）。
void invalidate_component_types();

// 当前资产目录（UI 顶部的路径输入框）。
const char* asset_dir();
// 让下一次 rebuild_assets 强制重扫（手动刷新、导入资产后）。
void invalidate_assets();

} // namespace gryce::editor::panel

#endif // GRYCE_EDITOR_PANEL_MODEL_H
