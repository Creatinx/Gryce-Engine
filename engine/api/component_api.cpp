// ===========================================================================
// engine/api/component_api.cpp
// 组件 C API：组件属性、组件树、类型继承的对外适配层。
//
// 类型 hash 口径（统一后）
// ------------------------
// 引擎内所有组件类型 hash 一律是「可读全名的 std::hash<std::string>>」，
// 唯一定义在 reflection/type_name.h：
//   GComponent_GetTypeHashAt          组件自身类型的全名 hash
//   GComponent_GetRegisteredTypeInfo  注册类型表里每种类型的全名 hash
//   GComponent_GetTypeHashByName      按名字（全名 / 短名）取全名 hash
//
// 组件解析一律按「全名 hash」进行：遍历反射表比对，命中后拿该类型的
// type_index，再用 Entity::find_component_view 取指针 —— 这一步天然支持
// 基类查询（派生实例向所有祖先类型各登记了一份正确子对象指针）。
//
// 组件下标口径
// ------------
// 所有带 index 入参的接口共用同一个下标空间：Entity::user_components()
// （过滤 Transform / ParentComponent / ChildrenComponent 后的可见序列）。
// 编辑器按下标取组件、脚本按下标取组件，必须与本口径一致。
// ===========================================================================

#include "api/component_api.h"
#include "api/api_guard.h"
#include "api/core_api.h"

#include "runtime/engine_context.h"

#include "ecs/world.h"

#include "scene/scene.h"

#include "scene/entity.h"

#include "components/component_factory.h"
#include "components/2d/tilemap.h"
#include "components/mesh_renderer.h"
#include "render/material.h"
#include "math/math.h"

#include "reflection/reflection.h"
#include "reflection/type_name.h"

#include <cstring>

#include <typeinfo>
#include <unordered_map>
#include <vector>

using gryce_engine::scene::Entity;

using gryce_engine::reflection::FieldType;
using gryce_engine::reflection::Registry;
using gryce_engine::reflection::TypeInfo;

using gryce_engine::components::Component;
using gryce_engine::components::ComponentFactory;

namespace {

// 有界 strlen（不依赖平台是否提供 std::strnlen）
static size_t bounded_strlen(const char* s, size_t max_len) {
    size_t n = 0;
    while (n < max_len && s[n] != '\0') ++n;
    return n;
}

// ---------------------------------------------------------------------------
// 类型 hash 与类型名
// ---------------------------------------------------------------------------

// 组件的 RTTI 全名（解修饰后的可读名，如
// "gryce_engine::components::d2::basic_rect::ColorRect"）。
static std::string component_type_name_of(Component* comp) {
    if (!comp) return "";
    return gryce_engine::reflection::demangle_type_name(typeid(*comp).name());
}

// 统一口径的类型 hash：可读全名 → hash。
static uint64_t hash_type_name(const std::string& readable_name) {
    return gryce_engine::reflection::type_hash_of_name(readable_name);
}

// ---------------------------------------------------------------------------
// 反射表查询
// ---------------------------------------------------------------------------

// 全名 hash → 反射表条目。
//
// 反射表是「组件继承关系」的真源：每个条目带 type_index（对应 C++ 类的
// typeid），据此可以在实体上按类型视角取到组件（含基类查询）。
// 表条目数量变化时重建缓存（运行期允许注册新的组件类型）。
static const TypeInfo* resolve_type_by_hash(uint64_t full_hash) {
    if (full_hash == 0) return nullptr;
    const Registry& reg = Registry::instance();

    static std::unordered_map<uint64_t, std::string> cache;  // 全名 hash → 反射表短名
    static size_t cached_type_count = 0;
    const size_t type_count = reg.type_count();
    if (type_count != cached_type_count) {
        cache.clear();
        for (const auto& entry : reg.all_types()) {
            const uint64_t h = reg.full_hash_of(entry.first);
            if (h != 0) cache.emplace(h, entry.first);
        }
        cached_type_count = type_count;
    }

    auto it = cache.find(full_hash);
    if (it == cache.end()) return nullptr;
    return reg.find(it->second);
}

// 全名 hash → 实体上的组件（支持基类查询）。
//
// 命中测试走 Entity::find_component_view(type_index)：类型视角索引为每个组件
// 实例的「自身 + 所有祖先类型」各登记了一份正确子对象指针，因此这一步天然
// 支持基类查询（传基类的 hash 能命中派生实例）。
//
// 视角指针是「该类型视角」的子对象地址，多继承时与 owning 指针不同址；
// dynamic_cast 需要 owning 指针（Component*）才能按正确偏移调整，因此命中
// 之后再回查该视角属于哪个组件实例。
static Component* resolve_component_by_hash(Entity* e, uint64_t full_hash) {
    if (!e || full_hash == 0) return nullptr;
    const TypeInfo* info = resolve_type_by_hash(full_hash);
    if (!info) return nullptr;
    if (!e->find_component_view(info->type_index)) return nullptr;

    for (const auto& comp : e->components()) {
        std::vector<gryce_engine::components::ComponentTypeView> views;
        comp->collect_type_views(views);
        for (const auto& v : views) {
            if (v.type == info->type_index) return comp.get();
        }
    }
    return nullptr;
}

// 组件解析结果：命中的组件 + 其反射表条目 + 展开后的字段表。
struct ComponentLookup {
    Component* comp = nullptr;
    const TypeInfo* type = nullptr;
    std::vector<const gryce_engine::reflection::FieldInfo*> fields;
};

static ComponentLookup lookup_component(Entity* e, uint64_t full_hash) {
    ComponentLookup out;
    if (!e || full_hash == 0) return out;
    const TypeInfo* info = resolve_type_by_hash(full_hash);
    if (!info) return out;
    Component* comp = resolve_component_by_hash(e, full_hash);
    if (!comp) return out;
    out.comp = comp;
    out.type = info;
    // 字段表按继承链展开（基类字段在前），与 Inspector 的展平顺序一致。
    out.fields = Registry::instance().all_fields(info->name);
    return out;
}

// ---------------------------------------------------------------------------
// 工厂注册表 ↔ 反射表 的名字映射
// ---------------------------------------------------------------------------

// 工厂注册名 → 反射表短名。
//
// 两张类型表绝大多数同名，唯一的例外是脚本组件：对外注册名是 "Script"
// （反序列化用的 type() 串），反射表里的类名是 "ScriptComponent"。
static const std::string& reflection_short_name_of(const std::string& factory_name) {
    static const std::string kScriptComponent = "ScriptComponent";
    if (factory_name == "Script") return kScriptComponent;
    return factory_name;
}

// 工厂注册名 → 全名 hash。
//
// 有反射条目的走反射全名 hash（与 GComponent_GetTypeHashAt 同口径）；
// 反射表没有的类型（ParentComponent / ChildrenComponent / SubViewport）
// 回退为短名 hash —— 这三个类型不进入对外的可见组件序列，编辑器用不到，
// 但哈希必须稳定可复算，因此用同一把尺子量短名。
static uint64_t registered_type_full_hash(const std::string& factory_name) {
    const uint64_t full = Registry::instance().full_hash_of(reflection_short_name_of(factory_name));
    if (full != 0) return full;
    return hash_type_name(factory_name);
}

// ---------------------------------------------------------------------------
// 组件树
// ---------------------------------------------------------------------------

// 组件指针 → 可见组件序列里的下标（找不到返回 -1）。
static int index_of_component(const std::vector<Component*>& comps, Component* target) {
    for (size_t i = 0; i < comps.size(); ++i) {
        if (comps[i] == target) return static_cast<int>(i);
    }
    return -1;
}

// 组件的类型祖先链（根 → 派生），每项是「自身 + 所有祖先类型」的全名 hash。
// 祖先链本身由 components::component_type_chain_names 统一产出（与脚本层同源）。
static std::vector<uint64_t> component_type_chain(const Component* comp) {
    std::vector<uint64_t> chain;
    for (const std::string& name : gryce_engine::components::component_type_chain_names(comp)) {
        chain.push_back(hash_type_name(name));
    }
    return chain;
}

// ---------------------------------------------------------------------------
// 实体与可见组件
// ---------------------------------------------------------------------------

static Entity* resolve_entity(GEntityHandle h) {
    return gryce_core::EntityResolver::resolve(h);
}

// 可见组件序列（过滤 Transform / ParentComponent / ChildrenComponent）。
// 所有带 index 入参的接口共用这一个下标空间。
static std::vector<Component*> get_components(Entity* e) {
    if (!e) return {};
    return e->user_components();
}

// Map FieldType to byte size
static int field_type_size(FieldType ft) {
    switch (ft) {
        case FieldType::Int:        return 4;
        case FieldType::Float:      return 4;
        case FieldType::Double:     return 8;
        case FieldType::Bool:       return 1;
        case FieldType::String:     return 256; // max string buffer
        case FieldType::Vector2f:   return 8;
        case FieldType::Vector3f:   return 12;
        case FieldType::Vector3i:   return 12;
        case FieldType::Vector4f:   return 16;
        case FieldType::Quaternionf:return 16;
        case FieldType::Color:      return 16;
        case FieldType::Enum:       return 4;
    }
    return 0;
}

// Map FieldType to int code for C API
static int field_type_code(FieldType ft) {
    switch (ft) {
        case FieldType::Int:        return 0;
        case FieldType::Float:      return 1;
        case FieldType::Double:     return 2;
        case FieldType::Bool:       return 3;
        case FieldType::String:     return 4;
        case FieldType::Vector2f:   return 5;
        case FieldType::Vector3f:   return 6;
        case FieldType::Vector3i:   return 7;
        case FieldType::Vector4f:   return 8;
        case FieldType::Quaternionf:return 9;
        case FieldType::Color:      return 10;
        case FieldType::Enum:       return 11;
    }
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// Tilemap 瓦片数据
// ---------------------------------------------------------------------------
namespace {

// Tilemap 解析：按全名 hash 命中反射表里的 Tilemap 条目，再取实体上的
// Tilemap 组件指针（dynamic_cast 做最终的 C++ 类型确认）。
gryce_engine::components::d2::tilemap::Tilemap* resolve_tilemap(
    GEntityHandle entity, uint64_t comp_type_hash) {
    Entity* e = resolve_entity(entity);
    if (!e) return nullptr;
    Component* comp = resolve_component_by_hash(e, comp_type_hash);
    if (!comp) return nullptr;
    return dynamic_cast<gryce_engine::components::d2::tilemap::Tilemap*>(comp);
}

} // namespace

int GComponent_TilemapGetTiles(GEntityHandle entity, uint64_t comp_type_hash,
                               int* out_tiles, int max_count) {
    GRYCE_API_GUARD();
    auto* tm = resolve_tilemap(entity, comp_type_hash);
    if (!tm) return -1;
    const int n = static_cast<int>(tm->tiles.size());
    // out_tiles 为空时返回所需数量（供调用方分配缓冲）
    if (!out_tiles) return n;
    if (max_count <= 0) return 0;
    const int copy = n < max_count ? n : max_count;
    for (int i = 0; i < copy; ++i) out_tiles[i] = tm->tiles[static_cast<size_t>(i)];
    return copy;
}

int GComponent_TilemapSetTiles(GEntityHandle entity, uint64_t comp_type_hash,
                               const int* tiles, int count) {
    GRYCE_API_GUARD();
    auto* tm = resolve_tilemap(entity, comp_type_hash);
    if (!tm) return -1;
    if (count <= 0) {
        tm->tiles.clear();
    } else if (tiles) {
        tm->tiles.assign(tiles, tiles + count);
    } else {
        return -1;
    }
    return 0;
}

namespace gryce_core {
std::string get_component_type_name(gryce_engine::components::Component* comp) {
    GRYCE_API_GUARD();
    return component_type_name_of(comp);
}
} // namespace gryce_core

extern "C" {

int GComponent_GetCount(GEntityHandle entity) {
    GRYCE_API_GUARD();

    Entity* e = resolve_entity(entity);

    if (!e) return 0;

    auto comps = get_components(e);

    return static_cast<int>(comps.size());
}

int GComponent_GetTypeHashAt(GEntityHandle entity, int index, uint64_t* out_hash) {
    GRYCE_API_GUARD();

    if (!out_hash) return -1;

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return -1;

    // 全名 hash（与 GComponent_GetRegisteredTypeInfo / GComponent_GetTypeHashByName 同口径）
    *out_hash = hash_type_name(component_type_name_of(comps[index]));

    return 0;
}

int GComponent_GetTypeNameAt(GEntityHandle entity, int index, char* out_buf, int buf_size) {
    GRYCE_API_GUARD();

    if (!out_buf || buf_size <= 0) return -1;

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return -1;

    std::string type_name = component_type_name_of(comps[index]);

    std::strncpy(out_buf, type_name.c_str(), static_cast<size_t>(buf_size) - 1);

    out_buf[buf_size - 1] = '\0';

    return static_cast<int>(std::strlen(out_buf));
}

int GComponent_GetPropertyCount(GEntityHandle entity, uint64_t comp_type_hash) {
    GRYCE_API_GUARD();

    Entity* e = resolve_entity(entity);

    if (!e) return 0;

    auto lookup = lookup_component(e, comp_type_hash);

    return static_cast<int>(lookup.fields.size());
}

int GComponent_GetPropertyInfo(GEntityHandle entity, uint64_t comp_type_hash, int prop_index,
                               char* out_name, int name_buf_size,
                               int* out_type, int* out_size) {
    GRYCE_API_GUARD();

    if (!out_name || name_buf_size <= 0 || !out_type || !out_size) return -1;

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto lookup = lookup_component(e, comp_type_hash);

    if (!lookup.comp) return -1;

    if (prop_index < 0 || prop_index >= static_cast<int>(lookup.fields.size())) return -1;

    const auto* f = lookup.fields[static_cast<size_t>(prop_index)];

    std::strncpy(out_name, f->name.c_str(), static_cast<size_t>(name_buf_size) - 1);

    out_name[name_buf_size - 1] = '\0';

    *out_type = field_type_code(f->type);

    *out_size = field_type_size(f->type);

    return 0;
}

int GComponent_GetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                           void* out_value, int value_size) {
    GRYCE_API_GUARD();

    if (!prop_name || !out_value || value_size <= 0) return -1;

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto lookup = lookup_component(e, comp_type_hash);

    if (!lookup.comp) return -1;

    for (const auto* f : lookup.fields) {

        if (f->name != prop_name) continue;

        if (!f->read) return -1;

        const int expected_size = field_type_size(f->type);

        if (value_size < expected_size) return -1;

        if (f->type == FieldType::String) {
            // 字符串字段：反射 read 目标类型是 std::string，
            // 桥接层先读入真实 std::string 再按 C 字符串写出。
            std::string tmp;
            f->read(lookup.comp, &tmp);
            std::strncpy(static_cast<char*>(out_value), tmp.c_str(),
                         static_cast<size_t>(value_size) - 1);
            static_cast<char*>(out_value)[value_size - 1] = '\0';
        } else {
            f->read(lookup.comp, out_value);
        }

        return 0;
    }

    return -1; // prop not found
}

int GComponent_SetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,
                           const void* value, int value_size) {
    GRYCE_API_GUARD();

    if (!prop_name || !value || value_size <= 0) return -1;

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto lookup = lookup_component(e, comp_type_hash);

    if (!lookup.comp) return -1;

    for (const auto* f : lookup.fields) {

        if (f->name != prop_name) continue;

        if (!f->write || f->read_only) return -1;

        const int expected_size = field_type_size(f->type);

        if (value_size < expected_size) return -1;

        bool ok = false;
        if (f->type == FieldType::String) {
            // 字符串字段：从 C 字符串构造 std::string 再走反射写
            const char* cstr = static_cast<const char*>(value);
            const size_t len = bounded_strlen(cstr, static_cast<size_t>(value_size));
            std::string tmp(cstr, len);
            ok = f->write(lookup.comp, &tmp);
        } else {
            ok = f->write(lookup.comp, value);
        }

        if (ok) {

            e->mark_dirty();

            // Fire component changed callback

            if (gryce_core::g_core_state.callbacks.on_component_changed) {

                gryce_core::g_core_state.callbacks.on_component_changed(

                    entity, comp_type_hash, gryce_core::g_core_state.callback_user_data);

            }

        }

        return ok ? 0 : -1;
    }

    return -1;
}

int GComponent_GetRegisteredTypeCount(void) {
    GRYCE_API_GUARD();

    return static_cast<int>(ComponentFactory::instance().all_types().size());
}

int GComponent_GetRegisteredTypeInfo(int index, uint64_t* out_hash, char* out_name, int name_buf_size) {
    GRYCE_API_GUARD();

    if (!out_hash || !out_name || name_buf_size <= 0) return -1;

    auto types = ComponentFactory::instance().all_types();

    if (index < 0 || index >= static_cast<int>(types.size())) return -1;

    const std::string& name = types[index];

    // 全名 hash（与 GComponent_GetTypeHashAt 同口径）；
    // 反射表没有的类型回退为短名 hash（见 registered_type_full_hash 注释）。
    *out_hash = registered_type_full_hash(name);

    std::strncpy(out_name, name.c_str(), static_cast<size_t>(name_buf_size) - 1);

    out_name[name_buf_size - 1] = '\0';

    return 0;
}

int GComponent_GetRegisteredTypeCategory(int index, char* out_category, int category_buf_size) {
    GRYCE_API_GUARD();

    if (!out_category || category_buf_size <= 0) return -1;

    auto types = ComponentFactory::instance().all_types();

    if (index < 0 || index >= static_cast<int>(types.size())) return -1;

    const char* category = ComponentFactory::instance().category(types[index]);

    std::strncpy(out_category, category, static_cast<size_t>(category_buf_size) - 1);

    out_category[category_buf_size - 1] = '\0';

    return 0;
}

int GComponent_AddComponent(GEntityHandle entity, uint64_t comp_type_hash) {
    GRYCE_API_GUARD();

    // 按全名 hash 找注册名（工厂短名口径 → 全名 hash 反查）
    auto types = ComponentFactory::instance().all_types();

    std::string type_name;

    for (const auto& t : types) {

        if (registered_type_full_hash(t) == comp_type_hash) {

            type_name = t;

            break;

        }

    }

    if (type_name.empty()) return -1;

    // Push AddComponent command via command buffer

    struct Payload { GEntityHandle h; char type_name[128]; };

    Payload payload;

    payload.h = entity;

    std::strncpy(payload.type_name, type_name.c_str(), sizeof(payload.type_name) - 1);

    payload.type_name[sizeof(payload.type_name) - 1] = '\0';

    GCommand cmd;

    cmd.type = ECMD_ADD_COMPONENT;

    cmd.seq = 0;

    std::memcpy(cmd.payload, &payload, sizeof(payload));

    return GCore_PushCommand(&cmd);

}

int GComponent_RemoveComponent(GEntityHandle entity, uint64_t comp_type_hash) {
    GRYCE_API_GUARD();

    // Push RemoveComponent command via command buffer

    struct Payload { GEntityHandle h; uint64_t type_hash; };

    Payload payload;

    payload.h = entity;

    payload.type_hash = comp_type_hash;

    GCommand cmd;

    cmd.type = ECMD_REMOVE_COMPONENT;

    cmd.seq = 0;

    std::memcpy(cmd.payload, &payload, sizeof(payload));

    return GCore_PushCommand(&cmd);

}

// ---------------------------------------------------------------------------
// 组件树（实例级父子挂载）与类型继承（基类 / 子类）
//
// 两轴正交：基类/子类说的是类型继承，父/子组件说的是实例挂载。
// 所有接口的 index 都落在同一个可见组件下标空间（见文件头说明）。
// ---------------------------------------------------------------------------

int GComponent_GetParentAt(GEntityHandle entity, int index) {
    GRYCE_API_GUARD();

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return -1;

    Component* parent = comps[index]->component_parent();

    // 无父组件 = 组件树根
    if (!parent) return -1;

    // 父组件若不在可见序列里（内部组件），对外无法表达，同样返回 -1
    return index_of_component(comps, parent);
}

int GComponent_SetParentAt(GEntityHandle entity, int index, int parent_index) {
    GRYCE_API_GUARD();

    Entity* e = resolve_entity(entity);

    if (!e) return 0;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return 0;

    Component* child = comps[index];

    Component* parent = nullptr;

    if (parent_index >= 0) {

        if (parent_index >= static_cast<int>(comps.size())) return 0;

        parent = comps[parent_index];

        // 不能挂到自己下面
        if (parent == child) return 0;

    }

    // 防环（挂到自己的后代之下）由 Component::set_component_parent 兜底
    if (!e->set_component_parent(child, parent)) return 0;

    e->mark_dirty();

    return 1;
}

int GComponent_GetChildCountAt(GEntityHandle entity, int index) {
    GRYCE_API_GUARD();

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return -1;

    int count = 0;

    for (Component* child : comps[index]->component_children()) {

        if (index_of_component(comps, child) < 0) continue;  // 内部组件不对外可见

        ++count;

    }

    return count;
}

int GComponent_GetChildAt(GEntityHandle entity, int index, int child_index) {
    GRYCE_API_GUARD();

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return -1;

    if (child_index < 0) return -1;

    int seen = 0;

    for (Component* child : comps[index]->component_children()) {

        const int child_idx = index_of_component(comps, child);

        if (child_idx < 0) continue;

        if (seen == child_index) return child_idx;

        ++seen;

    }

    return -1;
}

int GComponent_IsEffectivelyEnabledAt(GEntityHandle entity, int index) {
    GRYCE_API_GUARD();

    Entity* e = resolve_entity(entity);

    if (!e) return 0;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return 0;

    // 生命周期继承：自身与整条父链都 enabled 才算有效
    return comps[index]->is_effectively_enabled() ? 1 : 0;
}

int GComponent_GetLocalTransformAt(GEntityHandle entity, int index,
                                   float* out_pos3, float* out_quat4, float* out_scale3) {
    GRYCE_API_GUARD();

    if (!out_pos3 || !out_quat4 || !out_scale3) return -1;

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return -1;

    const Component* comp = comps[index];

    const auto& pos = comp->local_position();
    const auto& rot = comp->local_rotation();
    const auto& scl = comp->local_scale();

    out_pos3[0] = pos.x;
    out_pos3[1] = pos.y;
    out_pos3[2] = pos.z;

    out_quat4[0] = rot.x;
    out_quat4[1] = rot.y;
    out_quat4[2] = rot.z;
    out_quat4[3] = rot.w;

    out_scale3[0] = scl.x;
    out_scale3[1] = scl.y;
    out_scale3[2] = scl.z;

    return 0;
}

int GComponent_SetLocalTransformAt(GEntityHandle entity, int index,
                                   const float* pos3, const float* quat4, const float* scale3) {
    GRYCE_API_GUARD();

    if (!pos3 || !quat4 || !scale3) return 0;

    Entity* e = resolve_entity(entity);

    if (!e) return 0;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return 0;

    Component* comp = comps[index];

    comp->set_local_position(gryce_engine::math::Vector3f(pos3[0], pos3[1], pos3[2]));
    comp->set_local_rotation(gryce_engine::math::Quaternionf(quat4[0], quat4[1], quat4[2], quat4[3]));
    comp->set_local_scale(gryce_engine::math::Vector3f(scale3[0], scale3[1], scale3[2]));

    e->mark_dirty();

    return 1;
}

int GComponent_IsDerivedFromAt(GEntityHandle entity, int index, uint64_t base_type_hash) {
    GRYCE_API_GUARD();

    Entity* e = resolve_entity(entity);

    if (!e) return 0;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return 0;

    if (base_type_hash == 0) return 0;

    for (uint64_t h : component_type_chain(comps[index])) {

        if (h == base_type_hash) return 1;

    }

    return 0;
}

int GComponent_GetTypeChainAt(GEntityHandle entity, int index, uint64_t* out_hashes, int max_count) {
    GRYCE_API_GUARD();

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return -1;

    auto chain = component_type_chain(comps[index]);

    const int total = static_cast<int>(chain.size());

    // out_hashes 为空时只返回链长（供调用方分配缓冲）
    if (!out_hashes) return total;

    if (max_count <= 0) return 0;

    const int copy = total < max_count ? total : max_count;

    for (int i = 0; i < copy; ++i) out_hashes[i] = chain[static_cast<size_t>(i)];

    return copy;
}

int GComponent_GetTypeHashByName(const char* type_full_name, uint64_t* out_hash) {
    GRYCE_API_GUARD();

    if (!type_full_name || !out_hash) return -1;

    const std::string name(type_full_name);

    if (name.empty()) return -1;

    // 反射表短名 → 全名 hash（与 GComponent_GetTypeHashAt 同口径）
    const uint64_t full = Registry::instance().full_hash_of(name);

    if (full != 0) {

        *out_hash = full;

        return 0;

    }

    // 工厂短名（含 Script → ScriptComponent 别名）→ 与 GetRegisteredTypeInfo 同口径
    if (ComponentFactory::instance().has_type(name)) {

        *out_hash = registered_type_full_hash(name);

        return 0;

    }

    // 其余按可读全名处理
    *out_hash = hash_type_name(gryce_engine::reflection::demangle_type_name(name.c_str()));

    return 0;
}

int GComponent_GetTypeNameByHash(uint64_t type_hash, char* out_buf, int buf_size) {
    GRYCE_API_GUARD();

    if (!out_buf || buf_size <= 0) return -1;

    const TypeInfo* info = resolve_type_by_hash(type_hash);

    // hash 不认识（反射表里没有的类型，如回退短名 hash 的 ParentComponent）
    if (!info) return -1;

    const std::string full =
        gryce_engine::reflection::demangle_type_name(info->rtti_name.c_str());

    std::strncpy(out_buf, full.c_str(), static_cast<size_t>(buf_size) - 1);

    out_buf[buf_size - 1] = '\0';

    return static_cast<int>(std::strlen(out_buf));
}

int GComponent_MeshSetMaterial(GEntityHandle entity, const char* mesh_path,                               float r, float g, float b,
                               float roughness, float metallic) {
    GRYCE_API_GUARD();
    Entity* e = resolve_entity(entity);
    if (!e) return -1;

    using gryce_engine::components::MeshRenderer;
    MeshRenderer* mr = nullptr;
    for (const auto& comp : e->components()) {
        auto* c = dynamic_cast<MeshRenderer*>(comp.get());
        if (c) { mr = c; break; }
    }

    // 无 MeshRenderer：同步创建并挂到实体（复用 ComponentFactory + 实体所有权接管）。
    if (!mr) {
        auto created = ComponentFactory::instance().create("MeshRenderer");
        if (!created) return -1;
        auto* raw = created.get();
        e->add_component(std::move(created));
        mr = static_cast<MeshRenderer*>(raw);
        e->mark_dirty();
    }

    if (mesh_path && mesh_path[0]) {
        mr->mesh_path = mesh_path;
        e->mark_dirty();
    }

    gryce_engine::render::Material* mat = mr->ensure_material();
    mat->set_albedo(gryce_engine::math::Vector3f(r, g, b));
    mat->set_roughness(roughness);
    mat->set_metallic(metallic);
    return 0;
}

int GComponent_MeshSetMaterialPreset(GEntityHandle entity, const char* preset_name) {
    GRYCE_API_GUARD();
    if (!preset_name || !preset_name[0]) return -1;
    Entity* e = resolve_entity(entity);
    if (!e) return -1;

    using gryce_engine::components::MeshRenderer;
    MeshRenderer* mr = nullptr;
    for (const auto& comp : e->components()) {
        auto* c = dynamic_cast<MeshRenderer*>(comp.get());
        if (c) { mr = c; break; }
    }
    if (!mr) {
        auto created = ComponentFactory::instance().create("MeshRenderer");
        if (!created) return -1;
        auto* raw = created.get();
        e->add_component(std::move(created));
        mr = static_cast<MeshRenderer*>(raw);
    }

    gryce_engine::render::Material* mat = mr->ensure_material();
    if (!mat || !mat->apply_preset(preset_name)) return -1;
    e->mark_dirty();
    return 0;
}

int GComponent_MeshGetPath(GEntityHandle entity, char* out_buf, int buf_size) {
    GRYCE_API_GUARD();
    if (!out_buf || buf_size <= 0) return -1;
    out_buf[0] = '\0';
    Entity* e = resolve_entity(entity);
    if (!e) return -1;

    using gryce_engine::components::MeshRenderer;
    MeshRenderer* mr = nullptr;
    for (const auto& comp : e->components()) {
        auto* c = dynamic_cast<MeshRenderer*>(comp.get());
        if (c) { mr = c; break; }
    }
    if (!mr || mr->mesh_path.empty()) return -1;

    std::strncpy(out_buf, mr->mesh_path.c_str(), buf_size - 1);
    out_buf[buf_size - 1] = '\0';
    return 0;
}

int GComponent_MeshGetMaterial(GEntityHandle entity,
                               float* r, float* g, float* b,
                               float* roughness, float* metallic) {
    GRYCE_API_GUARD();
    if (!r || !g || !b || !roughness || !metallic) return -1;
    *r = 1.0f; *g = 1.0f; *b = 1.0f; *roughness = 0.6f; *metallic = 0.0f;
    Entity* e = resolve_entity(entity);
    if (!e) return -1;

    using gryce_engine::components::MeshRenderer;
    MeshRenderer* mr = nullptr;
    for (const auto& comp : e->components()) {
        auto* c = dynamic_cast<MeshRenderer*>(comp.get());
        if (c) { mr = c; break; }
    }
    if (!mr || !mr->material) return -1;

    const auto& col = mr->material->albedo_color;
    *r = col.x; *g = col.y; *b = col.z;
    *roughness = mr->material->roughness;
    *metallic = mr->material->metallic;
    return 0;
}

} // extern "C"
