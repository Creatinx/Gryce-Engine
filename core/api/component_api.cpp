#include "GryceCore/component_api.h"
#include "GryceCore/core_api.h"

#include "internal_state.h"

#include "ecs/world.h"

#include "scene/scene.h"

#include "scene/entity.h"

#include "components/component_factory.h"

#include "reflection/reflection.h"

#include <cstring>

#include <vector>

#include <typeinfo>

using gryce_engine::scene::Entity;

using gryce_engine::reflection::Registry;

using gryce_engine::reflection::FieldType;

using gryce_engine::components::ComponentFactory;

namespace {

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

static std::string get_component_type_name(gryce_engine::components::Component* comp) {

    if (!comp) return "";

    const char* raw = typeid(*comp).name();

    // MSVC: prefix "class " or "struct " ¡ª strip it

    if (std::strncmp(raw, "class ", 6) == 0) raw += 6;

    else if (std::strncmp(raw, "struct ", 7) == 0) raw += 7;

    return std::string(raw);

}

} // namespace

namespace gryce_core {
std::string get_component_type_name(gryce_engine::components::Component* comp) {
    if (!comp) return "";
    const char* raw = typeid(*comp).name();
    // MSVC: prefix "class " or "struct " -- strip it
    if (std::strncmp(raw, "class ", 6) == 0) raw += 6;
    else if (std::strncmp(raw, "struct ", 7) == 0) raw += 7;
    return std::string(raw);
}
} // namespace gryce_core

extern "C" {

int GComponent_GetCount(GEntityHandle entity) {

    Entity* e = resolve_entity(entity);

    if (!e) return 0;

    auto comps = get_components(e);

    return static_cast<int>(comps.size());

}

int GComponent_GetTypeHashAt(GEntityHandle entity, int index, uint64_t* out_hash) {

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

    Entity* e = resolve_entity(entity);

    if (!e) return 0;

    auto comps = get_components(e);

    for (auto* comp : comps) {

        std::string type_name = get_component_type_name(comp);

        if (hash_type_name(type_name) == comp_type_hash) {

            auto fields = Registry::instance().all_fields(type_name);

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

            auto fields = Registry::instance().all_fields(type_name);

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

            auto fields = Registry::instance().all_fields(type_name);

            for (const auto* f : fields) {

                if (f->name == prop_name) {

                    if (!f->read) return -1;

                    int expected_size = field_type_size(f->type);

                    if (value_size < expected_size) return -1;

                    f->read(comp, out_value);

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

            auto fields = Registry::instance().all_fields(type_name);

            for (const auto* f : fields) {

                if (f->name == prop_name) {

                    if (!f->write || f->read_only) return -1;

                    int expected_size = field_type_size(f->type);

                    if (value_size < expected_size) return -1;

                    bool ok = f->write(comp, value);

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

    return static_cast<int>(ComponentFactory::instance().all_types().size());

}

int GComponent_GetRegisteredTypeInfo(int index, uint64_t* out_hash, char* out_name, int name_buf_size) {

    if (!out_hash || !out_name || name_buf_size <= 0) return -1;

    auto types = ComponentFactory::instance().all_types();

    if (index < 0 || index >= static_cast<int>(types.size())) return -1;

    const std::string& name = types[index];

    *out_hash = hash_type_name(name);

    std::strncpy(out_name, name.c_str(), static_cast<size_t>(name_buf_size) - 1);

    out_name[name_buf_size - 1] = '\0';

    return 0;

}

int GComponent_AddComponent(GEntityHandle entity, uint64_t comp_type_hash) {

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

