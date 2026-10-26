// ===========================================================================
// editor/ext/command/commands.cpp — 编辑命令实现
// ===========================================================================

#include "command/commands.h"

#include "session/session.h"

#include "api/component_api.h"
#include "api/core_api.h"
#include "api/edit_api.h"
#include "api/entity_api.h"

#include "common/json_writer.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gryce::editor::commands {

namespace {

// 嵌套分组：引擎侧只有一个 group_open 标志，嵌套 BeginEdit 不会叠加。
// 这里记深度，只有最外层真正下发，否则内层的 EndEdit 会把外层的分组提前关掉。
struct GroupState {
    int depth = 0;
    std::string label;
};
GroupState g_group;

// 待确认的创建。GEdit_CreateEntity 只入队不回句柄，靠"创建即选中"确认。
// expire 计数用于丢弃永远确认不上的请求（连续创建时只有最后一个能被看到），
// 没有它会一直占着队列，后续创建的句柄永远拿不到。
struct PendingCreate {
    int count = 0;
    int waited_frames = 0;
    GEntityHandle last_selected = 0;
};
PendingCreate g_pending;
constexpr int kPendingExpireFrames = 4;

std::vector<std::string> g_events;

bool finite1(float v) { return std::isfinite(v); }
bool finite3(float a, float b, float c) {
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c);
}

void push_event(const std::string& e) {
    // 同一帧内同类事件去重：一个批量操作会触发 N 次，UI 只需要一条提示。
    for (const auto& existing : g_events) {
        if (existing == e) return;
    }
    g_events.push_back(e);
}

bool is_descendant_of(GEntityHandle entity, GEntityHandle maybe_ancestor) {
    // 沿父链上溯。场景实体数量有限（编辑器一般 < 万），不用做深度限制，
    // 但要防环：父链本身在引擎里是无环的，这里只按固定上限兜底。
    GEntityHandle cur = entity;
    for (int depth = 0; depth < 4096 && cur > 0; ++depth) {
        if (cur == maybe_ancestor) return true;
        cur = GEntity_GetParent(cur);
    }
    return false;
}

// 查属性的反射类型编码（0=int 1=float 2=double 3=bool 4=string
// 5=vec2 6=vec3 7=vec3i 8=vec4 9=quat 10=color）。找不到返回 -1。
int property_type_of(GEntityHandle handle, uint64_t type_hash, const char* prop) {
    const int count = GComponent_GetPropertyCount(handle, type_hash);
    for (int i = 0; i < count; ++i) {
        char name[64] = {};
        int type = -1;
        int size = 0;
        if (GComponent_GetPropertyInfo(handle, type_hash, i, name,
                                       static_cast<int>(sizeof(name)), &type, &size) != 0) {
            continue;
        }
        if (std::strcmp(name, prop) == 0) return type;
    }
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// 每帧
// ---------------------------------------------------------------------------

void reset() {
    g_group = GroupState{};
    g_pending = PendingCreate{};
    g_events.clear();
}

void tick() {
    if (g_pending.count <= 0) return;

    ++g_pending.waited_frames;
    const GEntityHandle sel = GEntity_GetSelected();

    // 新建实体的句柄只能这样拿：命令在上一帧的 GCore_BeginFrame 里执行，
    // cmd_create_entity 执行完会把新实体设为选中，于是这里能读到。
    if (sel != 0 && sel != g_pending.last_selected && session::handle_alive(sel)) {
        // 引擎那边已经选中了新实体，ext 的选择集还停在旧目标上。
        // 不同步的话"建完接着改属性"会改到上一个选中的物体。
        session::select(sel);
        push_event("created:" + std::to_string(sel));
        g_pending.last_selected = sel;
        --g_pending.count;
        g_pending.waited_frames = 0;
        return;
    }

    if (g_pending.waited_frames > kPendingExpireFrames) {
        // 连续创建时前几个拿不到句柄（选中被最后一个覆盖）。不是错误，
        // 但要让调用方知道返回值缺失，否则会一直等一个不会来的事件。
        push_event("create_unconfirmed");
        --g_pending.count;
        g_pending.waited_frames = 0;
    }
}

std::string take_events_json() {
    Json j;
    j.begin_array();
    for (const auto& e : g_events) j.value(e);
    j.end_array();
    g_events.clear();
    return j.str();
}

// ---------------------------------------------------------------------------
// 层级
// ---------------------------------------------------------------------------

int create(const char* name, GEntityHandle parent) {
    const char* n = (name && name[0]) ? name : "Entity";
    if (parent < 0) return 0;
    if (parent > 0 && !session::handle_alive(parent)) return 0;

    g_pending.last_selected = GEntity_GetSelected();
    if (!GEdit_CreateEntity(n, parent)) return 0;
    ++g_pending.count;
    g_pending.waited_frames = 0;
    return 1;
}

int destroy(GEntityHandle handle) {
    if (handle <= 0 || !session::handle_alive(handle)) return 0;
    return GEdit_DestroyEntity(handle);
}

int rename(GEntityHandle handle, const char* name) {
    if (handle <= 0 || !name || !name[0]) return 0;
    if (std::strlen(name) >= 128) return 0;   // payload 的名字字段是 128 字节
    if (!session::handle_alive(handle)) return 0;
    return GEdit_RenameEntity(handle, name);
}

int reparent(GEntityHandle handle, GEntityHandle parent) {
    if (handle <= 0 || !session::handle_alive(handle)) return 0;
    if (parent < 0) return 0;
    if (parent > 0) {
        if (!session::handle_alive(parent)) return 0;
        // 挂到自己的子树下会造出环，场景序列化会无限递归。引擎侧也拦
        // （core_api.cpp 的 cmd_reparent_entity），这里拦是为了**能给出反馈**：
        // 引擎只是静默忽略，界面就变成"拖过去什么都没发生"。
        if (parent == handle || is_descendant_of(parent, handle)) {
            push_event("reparent_rejected");
            return 0;
        }
    }
    return GEdit_ReparentEntity(handle, parent);
}

// ---------------------------------------------------------------------------
// 变换
// ---------------------------------------------------------------------------

int set_position(GEntityHandle handle, float x, float y, float z) {
    if (handle <= 0 || !finite3(x, y, z) || !session::handle_alive(handle)) return 0;
    const GVec3 p{x, y, z};
    return GEdit_SetTransform(handle, &p, nullptr, nullptr);
}

int set_rotation(GEntityHandle handle, float x, float y, float z, float w) {
    if (handle <= 0 || !finite3(x, y, z) || !finite1(w)) return 0;
    if (!session::handle_alive(handle)) return 0;
    const float len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len < 1e-6f) return 0;   // 全零四元数：引擎会当成"保持原旋转"，直接拒
    const GQuat r{x / len, y / len, z / len, w / len};
    return GEdit_SetTransform(handle, nullptr, &r, nullptr);
}

int set_scale(GEntityHandle handle, float x, float y, float z) {
    if (handle <= 0 || !finite3(x, y, z) || !session::handle_alive(handle)) return 0;
    // 负缩放是合法的（镜像），但不允许出现 0 —— 那会把矩阵变成奇异的，
    // 法线矩阵求逆会出 NaN，之后整棵子树都不见了。
    if (std::fabs(x) < 1e-6f || std::fabs(y) < 1e-6f || std::fabs(z) < 1e-6f) return 0;
    const GVec3 s{x, y, z};
    return GEdit_SetTransform(handle, nullptr, nullptr, &s);
}

int translate(GEntityHandle handle, float dx, float dy, float dz) {
    if (handle <= 0 || !finite3(dx, dy, dz) || !session::handle_alive(handle)) return 0;
    // 用引擎的增量命令，不用"读当前位置 + SetTransform"：后者在入队时就取了
    // 当前值，同一帧内若是"先 setPosition 再 translate"，两条命令算出来的
    // 绝对值相同，后一条会把前一条覆盖掉，最终只生效一次。
    const GVec3 delta{dx, dy, dz};
    return GEdit_TranslateEntity(handle, &delta);
}

// ---------------------------------------------------------------------------
// 组件与属性
// ---------------------------------------------------------------------------

uint64_t type_hash_for(const char* type_name) {
    if (!type_name || !type_name[0]) return 0;
    const int count = GComponent_GetRegisteredTypeCount();
    for (int i = 0; i < count; ++i) {
        uint64_t hash = 0;
        char name[64] = {};
        if (GComponent_GetRegisteredTypeInfo(i, &hash, name, static_cast<int>(sizeof(name))) != 0) {
            continue;
        }
        if (std::strcmp(name, type_name) == 0) return hash;
    }
    return 0;
}

int set_property_number(GEntityHandle handle, uint64_t type_hash,
                        const char* prop, double value) {
    if (handle <= 0 || type_hash == 0 || !prop || !prop[0]) return 0;
    if (!std::isfinite(value) || !session::handle_alive(handle)) return 0;

    switch (property_type_of(handle, type_hash, prop)) {
        case 0: {   // int
            const int32_t v = static_cast<int32_t>(std::llround(value));
            return GEdit_SetProperty(handle, type_hash, prop, &v, sizeof(v));
        }
        case 1: {   // float
            const float v = static_cast<float>(value);
            return GEdit_SetProperty(handle, type_hash, prop, &v, sizeof(v));
        }
        case 2: {   // double
            return GEdit_SetProperty(handle, type_hash, prop, &value, sizeof(value));
        }
        case 3: {   // bool：走同一个入口，统一按 0/非 0 解释
            const bool v = value != 0.0;
            return GEdit_SetProperty(handle, type_hash, prop, &v, sizeof(v));
        }
        default:
            // 向量 / 四元数 / 颜色需要逐分量接口，标量入口写不进去。
            // 拒绝而不是写一半，免得 Inspector 显示"改成功了但没变"。
            return 0;
    }
}

int set_property_bool(GEntityHandle handle, uint64_t type_hash,
                      const char* prop, int value) {
    return set_property_number(handle, type_hash, prop, value != 0 ? 1.0 : 0.0);
}

int set_property_vec(GEntityHandle handle, uint64_t type_hash,
                     const char* prop, const float* values, int count) {
    if (handle <= 0 || type_hash == 0 || !prop || !prop[0]) return 0;
    if (!values || count <= 0 || count > 4) return 0;
    if (!session::handle_alive(handle)) return 0;
    for (int i = 0; i < count; ++i) {
        if (!std::isfinite(values[i])) return 0;
    }

    // 反射字段按原始字节写入（GCmdSetProperty::value 给 128 字节，向量最长的
    // 四元数只占 16），所以这里要做的是"按字段的真实类型摆好内存"：
    // vec3i 是三个 int32，其余都是 float。摆错类型的表现是数值离谱地大
    // （float 位模式当 int 读）或干脆是 0，界面上看不出来源。
    const int type = property_type_of(handle, type_hash, prop);
    int expected = 0;
    switch (type) {
        case 5: expected = 2; break;   // vec2
        case 6: expected = 3; break;   // vec3
        case 7: expected = 3; break;   // vec3i
        case 8: expected = 4; break;   // vec4
        case 9: expected = 4; break;   // quat
        case 10: expected = 4; break;  // color
        default: return 0;             // 标量 / 字符串不走这里
    }
    if (expected != count) return 0;

    if (type == 7) {
        int32_t iv[3] = {};
        for (int i = 0; i < 3; ++i) iv[i] = static_cast<int32_t>(std::llround(values[i]));
        return GEdit_SetProperty(handle, type_hash, prop, iv, sizeof(iv));
    }

    float fv[4] = {};
    for (int i = 0; i < count; ++i) fv[i] = values[i];
    return GEdit_SetProperty(handle, type_hash, prop, fv,
                             static_cast<int>(sizeof(float) * static_cast<std::size_t>(count)));
}

int set_property_string(GEntityHandle handle, uint64_t type_hash,
                        const char* prop, const char* value) {
    if (handle <= 0 || type_hash == 0 || !prop || !prop[0]) return 0;
    if (!value) return 0;
    if (!session::handle_alive(handle)) return 0;
    // 字段不是字符串就拒：写进去会被引擎按字段类型解读，字符串字面量被当成
    // 数值就是一段乱码，而界面上看不出是自己写错了类型。
    if (property_type_of(handle, type_hash, prop) != 4) return 0;
    // 传的是 C 字符串的原始字节（含结尾的 0）。
    return GEdit_SetProperty(handle, type_hash, prop, value,
                             static_cast<int>(std::strlen(value) + 1));
}

int add_component(GEntityHandle handle, const char* type_name) {
    if (handle <= 0 || !type_name || !type_name[0]) return 0;
    if (!session::handle_alive(handle)) return 0;
    if (type_hash_for(type_name) == 0) return 0;   // 未注册的类型，入队也不会成功
    return GEdit_AddComponent(handle, type_name);
}

int remove_component(GEntityHandle handle, uint64_t type_hash) {
    if (handle <= 0 || type_hash == 0) return 0;
    if (!session::handle_alive(handle)) return 0;
    return GEdit_RemoveComponent(handle, type_hash);
}

int set_material(GEntityHandle handle, const char* mesh_path,
                 float r, float g, float b, float roughness, float metallic) {
    if (handle <= 0 || !session::handle_alive(handle)) return 0;
    if (!finite3(r, g, b) || !finite1(roughness) || !finite1(metallic)) return 0;
    // 归一化到 [0,1]：Gizmo/Inspector 传进来的可能是 0~255 的整数色，
    // 越界值写进材质会让整个物体过曝，看不出是数值问题。
    auto sat = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    return GEdit_SetMaterial(handle, mesh_path ? mesh_path : "",
                             sat(r), sat(g), sat(b), sat(roughness), sat(metallic));
}

int import_asset(const char* path) {
    if (!path || !path[0]) return 0;
    return GEdit_ImportAsset(path);
}

// ---------------------------------------------------------------------------
// 复合操作
// ---------------------------------------------------------------------------

int delete_selection() {
    const auto& sel = session::selection();
    if (sel.items.empty()) return 0;

    // 一次删除多个实体必须合成一步撤销：否则用户按 Ctrl+Z 只回来一个，
    // 要按 N 次才回到删除前，很难理解。
    begin_edit("delete");
    for (GEntityHandle h : sel.items) {
        if (session::handle_alive(h)) GEdit_DestroyEntity(h);
    }
    end_edit();

    // 选择集不能留着一堆即将失效的句柄：tick 会剔除，但这一帧 Inspector
    // 还会读到它们，先主动清掉。
    session::clear_selection();
    push_event("deleted");
    return 1;
}

int save(const char* path) {
    return session::save_doc(path);
}

int set_play(int play) {
    return GEdit_SetPlayMode(play != 0 ? 1 : 0);
}

int toggle_play() {
    if (GCore_IsPlaying()) {
        return GEdit_SetPlayMode(0);
    }
    return GEdit_SetPlayMode(1);
}

int step_frame() {
    if (!GCore_IsPlaying()) return 0;
    return GEdit_StepFrame(1);
}

std::string focus_json() {
    const auto& sel = session::selection();
    const GEntityHandle target = sel.primary;
    if (target <= 0 || !session::handle_alive(target)) return "{\"ok\":0}";

    // 用"世界位置 + 子节点位置的平均"代替真正的几何包围盒：引擎没有暴露
    // 包围盒查询，而位置中心对"聚焦到物体"这个用途足够了。等引擎补上
    // GEntity_GetWorldBounds 再换（按 README 规则一，那是通用能力）。
    GVec3 center{};
    if (GEntity_GetWorldPosition(target, &center) != 0) return "{\"ok\":0}";

    int n = 1;
    auto accumulate = [&](GEntityHandle h) {
        GVec3 p{};
        if (GEntity_GetWorldPosition(h, &p) == 0) {
            center.x += p.x;
            center.y += p.y;
            center.z += p.z;
            ++n;
        }
    };
    const int children = GEntity_GetChildCount(target);
    for (int i = 0; i < children; ++i) {
        accumulate(GEntity_GetChildAt(target, i));
    }
    center.x /= static_cast<float>(n);
    center.y /= static_cast<float>(n);
    center.z /= static_cast<float>(n);

    Json j;
    j.begin_object();
    j.kv("ok", 1);
    j.kv("x", center.x);
    j.kv("y", center.y);
    j.kv("z", center.z);
    j.kv("count", n);
    j.end_object();
    return j.str();
}

// ---------------------------------------------------------------------------
// 分组
// ---------------------------------------------------------------------------

void begin_edit(const char* label) {
    if (g_group.depth == 0) {
        g_group.label = (label && label[0]) ? label : "edit";
        GCore_BeginEdit(g_group.label.c_str());
    }
    ++g_group.depth;
}

void end_edit() {
    if (g_group.depth == 0) return;
    --g_group.depth;
    if (g_group.depth == 0) {
        GCore_EndEdit();
        g_group.label.clear();
    }
}

void abort_edit() {
    if (g_group.depth == 0) return;
    g_group.depth = 0;
    GCore_EndEdit();
    g_group.label.clear();
}

const char* current_group_label() {
    return g_group.label.c_str();
}

} // namespace gryce::editor::commands
