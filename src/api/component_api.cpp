#include "GryceCore/component_api.h"
#include "GryceCore/api_guard.h"
#include "GryceCore/core_api.h"

#include "runtime/engine_context.h"

#include "ecs/world.h"

#include "scene/scene.h"

#include "scene/entity.h"

#include "components/component_factory.h"
#include "components/2d/tilemap.h"

#include "reflection/reflection.h"

#include <cstring>

#include <vector>

#include <typeinfo>

#if defined(__GNUC__) || defined(__clang__)
#include <cxxabi.h>
#include <cstdlib>
#endif

using gryce_engine::scene::Entity;

using gryce_engine::reflection::Registry;

using gryce_engine::reflection::FieldType;

using gryce_engine::components::ComponentFactory;

namespace {

// 有界 strlen（不依赖平台是否提供 std::strnlen）
static size_t bounded_strlen(const char* s, size_t max_len) {
    size_t n = 0;
    while (n < max_len && s[n] != '\0') ++n;
    return n;
}

// ---------------------------------------------------------------------------

// Type hash: use std::hash<std::string> on the type name

// ---------------------------------------------------------------------------

static uint64_t hash_type_name(const std::string& name) {

    return std::hash<std::string>{}(name);

}

static Entity* resolve_entity(GEntityHandle h) {

    return gryce_core::EntityResolver::resolve(h);

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

// Helper: get all components on an entity (excluding Transform which is internal)

static std::vector<gryce_engine::components::Component*> get_components(Entity* e) {

    std::vector<gryce_engine::components::Component*> out;

    if (!e) return out;

    for (const auto& comp : e->components()) {

        // Skip Transform (internal, every entity has it)

        if (dynamic_cast<gryce_engine::components::Transform*>(comp.get())) continue;

        out.push_back(comp.get());

    }

    return out;

}

// Helper: get type name from a component via RTTI typeid
static std::string demangle_type_name(const char* raw) {
#if defined(__GNUC__) || defined(__clang__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
    if (status == 0 && demangled) {
        std::string out(demangled);
        std::free(demangled);
        return out;
    }
#endif
    return raw ? std::string(raw) : std::string();
}

static std::string get_component_type_name(gryce_engine::components::Component* comp) {
    if (!comp) return "";
    const char* raw = typeid(*comp).name();
    std::string name = demangle_type_name(raw);
    // MSVC: prefix "class " or "struct " -- strip it
    if (name.rfind("class ", 0) == 0) name.erase(0, 6);
    else if (name.rfind("struct ", 0) == 0) name.erase(0, 7);
    return name;
}

} // namespace

// ---------------------------------------------------------------------------
// Tilemap 瓦片数据
// ---------------------------------------------------------------------------
namespace {

gryce_engine::components::d2::tilemap::Tilemap* resolve_tilemap(
    GEntityHandle entity, uint64_t comp_type_hash) {
    Entity* e = resolve_entity(entity);
    if (!e) return nullptr;
    for (const auto& comp : e->components()) {
        auto* tm = dynamic_cast<gryce_engine::components::d2::tilemap::Tilemap*>(comp.get());
        if (!tm) continue;
        if (hash_type_name(tm->type()) != comp_type_hash) continue;
        return tm;
    }
    return nullptr;
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
    if (!comp) return "";
    const char* raw = typeid(*comp).name();
    std::string demangled = demangle_type_name(raw);
    // MSVC: prefix "class " or "struct " -- strip it
    if (demangled.rfind("class ", 0) == 0) demangled.erase(0, 6);
    else if (demangled.rfind("struct ", 0) == 0) demangled.erase(0, 7);
    return demangled;
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

    std::string type_name = get_component_type_name(comps[index]);

    *out_hash = hash_type_name(type_name);

    return 0;

}

int GComponent_GetTypeNameAt(GEntityHandle entity, int index, char* out_buf, int buf_size) {
    GRYCE_API_GUARD();

    if (!out_buf || buf_size <= 0) return -1;

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto comps = get_components(e);

    if (index < 0 || index >= static_cast<int>(comps.size())) return -1;

    std::string type_name = get_component_type_name(comps[index]);

    std::strncpy(out_buf, type_name.c_str(), static_cast<size_t>(buf_size) - 1);

    out_buf[buf_size - 1] = '\0';

    return static_cast<int>(std::strlen(out_buf));

}

int GComponent_GetPropertyCount(GEntityHandle entity, uint64_t comp_type_hash) {
    GRYCE_API_GUARD();

    Entity* e = resolve_entity(entity);

    if (!e) return 0;

    auto comps = get_components(e);

    for (auto* comp : comps) {

        std::string type_name = get_component_type_name(comp);

        if (hash_type_name(type_name) == comp_type_hash) {

        auto fields = Registry::instance().all_fields(gryce_core::reflection_lookup_name(type_name));

            return static_cast<int>(fields.size());

        }

    }

    return 0;

}

int GComponent_GetPropertyInfo(GEntityHandle entity, uint64_t comp_type_hash, int prop_index,

                               char* out_name, int name_buf_size,

                               int* out_type, int* out_size) {

    if (!out_name || name_buf_size <= 0 || !out_type || !out_size) return -1;

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto comps = get_components(e);

    for (auto* comp : comps) {

        std::string type_name = get_component_type_name(comp);

        if (hash_type_name(type_name) == comp_type_hash) {

        auto fields = Registry::instance().all_fields(gryce_core::reflection_lookup_name(type_name));

            if (prop_index < 0 || prop_index >= static_cast<int>(fields.size())) return -1;

            const auto* f = fields[prop_index];

            std::strncpy(out_name, f->name.c_str(), static_cast<size_t>(name_buf_size) - 1);

            out_name[name_buf_size - 1] = '\0';

            *out_type = field_type_code(f->type);

            *out_size = field_type_size(f->type);

            return 0;

        }

    }

    return -1;

}

int GComponent_GetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,

                           void* out_value, int value_size) {

    if (!prop_name || !out_value || value_size <= 0) return -1;

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto comps = get_components(e);

    for (auto* comp : comps) {

        std::string type_name = get_component_type_name(comp);

        if (hash_type_name(type_name) == comp_type_hash) {

        auto fields = Registry::instance().all_fields(gryce_core::reflection_lookup_name(type_name));

            for (const auto* f : fields) {

                if (f->name == prop_name) {

                    if (!f->read) return -1;

                    int expected_size = field_type_size(f->type);

                    if (value_size < expected_size) return -1;

                    if (f->type == FieldType::String) {
                        // 字符串字段：反射 read 目标类型是 std::string，
                        // 桥接层先读入真实 std::string 再按 C 字符串写出。
                        std::string tmp;
                        f->read(comp, &tmp);
                        std::strncpy(static_cast<char*>(out_value), tmp.c_str(),
                                     static_cast<size_t>(value_size) - 1);
                        static_cast<char*>(out_value)[value_size - 1] = '\0';
                    } else {
                        f->read(comp, out_value);
                    }

                    return 0;

                }

            }

            return -1; // prop not found

        }

    }

    return -1;

}

int GComponent_SetProperty(GEntityHandle entity, uint64_t comp_type_hash, const char* prop_name,

                           const void* value, int value_size) {

    if (!prop_name || !value || value_size <= 0) return -1;

    Entity* e = resolve_entity(entity);

    if (!e) return -1;

    auto comps = get_components(e);

    for (auto* comp : comps) {

        std::string type_name = get_component_type_name(comp);

        if (hash_type_name(type_name) == comp_type_hash) {

        auto fields = Registry::instance().all_fields(gryce_core::reflection_lookup_name(type_name));

            for (const auto* f : fields) {

                if (f->name == prop_name) {

                    if (!f->write || f->read_only) return -1;

                    int expected_size = field_type_size(f->type);

                    if (value_size < expected_size) return -1;

                    bool ok = false;
                    if (f->type == FieldType::String) {
                        // 字符串字段：从 C 字符串构造 std::string 再走反射写
                        const char* cstr = static_cast<const char*>(value);
                        const size_t len = bounded_strlen(cstr, static_cast<size_t>(value_size));
                        std::string tmp(cstr, len);
                        ok = f->write(comp, &tmp);
                    } else {
                        ok = f->write(comp, value);
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

            }

            return -1;

        }

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

    *out_hash = hash_type_name(name);

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

    // Find the type name from the hash by searching registered types

    auto types = ComponentFactory::instance().all_types();

    std::string type_name;

    for (const auto& t : types) {

        if (hash_type_name(t) == comp_type_hash) {

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

} // extern "C"
