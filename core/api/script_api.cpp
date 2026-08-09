#include "GryceCore/script_api.h"

#include "api/internal_state.h"
#include "components/script_component.h"
#include "ecs/systems/script_system.h"
#include "ecs/world.h"
#include "scene/entity.h"
#include "script/lua_runtime.h"

#include <cstring>
#include <string>

namespace {

using gryce_engine::components::ScriptComponent;

ScriptComponent* find_script_component(GEntityHandle h) {
    auto* e = gryce_core::EntityResolver::resolve(h);
    return e ? e->get_component<ScriptComponent>() : nullptr;
}

gryce_engine::ecs::ScriptSystem* script_system() {
    auto* world = gryce_core::g_core_state.world.get();
    return world ? world->get_system<gryce_engine::ecs::ScriptSystem>() : nullptr;
}

} // namespace

extern "C" {

const char* GScript_GetVersion(void) {
    return "GryceSRT 0.1.0 (Lua 5.4.7)";
}

int GScript_RunString(const char* code, char* err_out, int err_cap) {
    auto& rt = gryce_engine::script::LuaRuntime::instance();
    if (!rt.initialized() && !rt.init()) return -1;

    std::string err;
    const bool ok = rt.run_string(code, &err);
    if (!ok && err_out && err_cap > 0) {
        std::strncpy(err_out, err.c_str(), static_cast<size_t>(err_cap) - 1);
        err_out[err_cap - 1] = '\0';
    }
    return ok ? 0 : -1;
}

int GScript_RunFile(const char* path, char* err_out, int err_cap) {
    auto& rt = gryce_engine::script::LuaRuntime::instance();
    if (!rt.initialized() && !rt.init()) return -1;

    std::string err;
    const bool ok = rt.run_file(path, &err);
    if (!ok && err_out && err_cap > 0) {
        std::strncpy(err_out, err.c_str(), static_cast<size_t>(err_cap) - 1);
        err_out[err_cap - 1] = '\0';
    }
    return ok ? 0 : -1;
}

int GScript_GetPropCount(GEntityHandle entity, int* out_count) {
    auto* comp = find_script_component(entity);
    if (!comp || !out_count) return -1;
    *out_count = static_cast<int>(comp->props.size());
    return 0;
}

int GScript_GetPropInfo(GEntityHandle entity, int index,
                        char* name_out, int name_cap, int* out_type) {
    auto* comp = find_script_component(entity);
    if (!comp || index < 0 || index >= static_cast<int>(comp->props.size())) return -1;
    const auto& p = comp->props[index];
    if (name_out && name_cap > 0) {
        std::strncpy(name_out, p.name.c_str(), static_cast<size_t>(name_cap) - 1);
        name_out[name_cap - 1] = '\0';
    }
    if (out_type) *out_type = p.type;
    return 0;
}

int GScript_GetPropFloat(GEntityHandle entity, const char* name, float* out_value) {
    auto* comp = find_script_component(entity);
    auto* sys = script_system();
    if (!comp || !sys || !name || !out_value) return -1;
    int type = 0;
    float f = 0.0f;
    std::string s;
    if (!sys->get_prop(comp, name, type, f, s) || type != 0) return -1;
    *out_value = f;
    return 0;
}

int GScript_SetPropFloat(GEntityHandle entity, const char* name, float value) {
    auto* comp = find_script_component(entity);
    auto* sys = script_system();
    if (!comp || !sys || !name) return -1;
    return sys->set_prop(comp, name, value) ? 0 : -1;
}

int GScript_GetPropString(GEntityHandle entity, const char* name,
                          char* out_value, int value_cap) {
    auto* comp = find_script_component(entity);
    auto* sys = script_system();
    if (!comp || !sys || !name || !out_value || value_cap <= 0) return -1;
    int type = 0;
    float f = 0.0f;
    std::string s;
    if (!sys->get_prop(comp, name, type, f, s) || type != 1) return -1;
    std::strncpy(out_value, s.c_str(), static_cast<size_t>(value_cap) - 1);
    out_value[value_cap - 1] = '\0';
    return 0;
}

int GScript_SetPropString(GEntityHandle entity, const char* name, const char* value) {
    auto* comp = find_script_component(entity);
    auto* sys = script_system();
    if (!comp || !sys || !name || !value) return -1;
    return sys->set_prop(comp, name, std::string(value)) ? 0 : -1;
}

} // extern "C"
