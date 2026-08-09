#include "ecs/systems/script_system.h"

#include "components/script_component.h"
#include "resources/resource_path.h"
#include "scene/entity.h"
#include "scene/scene.h"
#include "script/lua_runtime.h"
#include "utils/glog/glog_lib.h"

#include <algorithm>
#include <fstream>
#include <sstream>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace gryce_engine::ecs {

namespace {

inline bool contains(const std::vector<components::ScriptComponent*>& v,
                     components::ScriptComponent* c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

} // namespace

void ScriptSystem::on_update(scene::Scene& scene, float dt) {
    auto& rt = script::LuaRuntime::instance();
    if (!rt.initialized()) return;

    rt.set_delta(dt);
    seen_.clear();

    scene.root()->foreach([&](scene::Entity* e) {
        process_entity(e, dt);
    });

    // Drop bookkeeping for components that no longer exist (removed entity /
    // component). The component object itself is gone, so only drop the entry.
    loaded_.erase(std::remove_if(loaded_.begin(), loaded_.end(),
        [&](components::ScriptComponent* c) { return !contains(seen_, c); }),
        loaded_.end());
}

void ScriptSystem::process_entity(scene::Entity* e, float dt) {
    auto* comp = e->get_component<components::ScriptComponent>();
    if (!comp) return;
    seen_.push_back(comp);

    if (!comp->enabled || comp->script_path.empty()) {
        if (comp->script_loaded) unload(comp);
        return;
    }

    if (!comp->script_loaded) {
        if (!load(comp)) {
            handle_error(comp);
            return;
        }
    }

    call_method(comp, "on_update", dt, true);
}

bool ScriptSystem::load(components::ScriptComponent* comp) {
    auto& rt = script::LuaRuntime::instance();
    lua_State* L = rt.state();
    if (!L) return false;

    comp->last_error.clear();
    comp->reported_error = false;

    const std::string full = resources::ResourcePath::resolve(comp->script_path);
    std::string src;
    auto it = source_cache_.find(comp->script_path);
    if (it != source_cache_.end()) {
        src = it->second;
    } else {
        std::ifstream in(full, std::ios::binary);
        if (!in) {
            comp->last_error = "cannot open script file: " + full;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        src = ss.str();
        source_cache_[comp->script_path] = src;
    }
    if (src.empty()) {
        comp->last_error = "empty script: " + comp->script_path;
        return false;
    }

    // Preserve Inspector-edited props across a hot reload: remember the old
    // values, let the script re-declare defaults, then re-apply old values.
    const auto old_props = comp->props;

    // Per-component environment: env.__index = _G
    lua_newtable(L);                          // env
    lua_newtable(L);                          // mt
    lua_pushglobaltable(L);                   // _G
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);                  // env (mt popped)

    if (luaL_loadbuffer(L, src.c_str(), src.size(),
                        comp->script_path.c_str()) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        comp->last_error = msg ? msg : "script compile error";
        lua_pop(L, 2);                        // error + env
        return false;
    }

    lua_pushvalue(L, -2);                     // env copy
    lua_setupvalue(L, -2, 1);                 // chunk._ENV = env

    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        comp->last_error = msg ? msg : "script load error";
        lua_pop(L, 2);                        // error + env
        return false;
    }

    lua_pushvalue(L, -1);                     // env copy
    comp->env_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);                            // env

    comp->chunk_ref = -1;                     // top-level already executed
    comp->script_loaded = true;
    comp->start_called = false;
    comp->reported_error = false;

    call_method(comp, "on_start");
    sync_props_from_env(comp);
    for (const auto& old : old_props) {
        for (auto& p : comp->props) {
            if (p.name == old.name && p.type == old.type) {
                if (p.type == 1) {
                    p.s = old.s;
                    write_prop_to_env(comp, old.name.c_str(), old.s);
                } else {
                    p.f = old.f;
                    write_prop_to_env(comp, old.name.c_str(), old.f);
                }
                break;
            }
        }
    }
    return true;
}

void ScriptSystem::sync_props_from_env(components::ScriptComponent* comp) {
    auto& rt = script::LuaRuntime::instance();
    lua_State* L = rt.state();
    if (!L || comp->env_ref < 0) return;

    comp->props.clear();
    lua_rawgeti(L, LUA_REGISTRYINDEX, comp->env_ref);   // env
    lua_getfield(L, -1, "props");                        // env, props
    if (lua_istable(L, -1)) {
        lua_pushnil(L);                                   // env, props, key
        while (lua_next(L, -2) != 0) {                    // env, props, key, val
            if (lua_type(L, -2) == LUA_TSTRING) {
                components::ScriptProp p;
                p.name = lua_tostring(L, -2) ? lua_tostring(L, -2) : "";
                if (lua_isnumber(L, -1)) {
                    p.type = 0;
                    p.f = static_cast<float>(lua_tonumber(L, -1));
                } else if (lua_isstring(L, -1)) {
                    p.type = 1;
                    p.s = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
                }
                comp->props.push_back(std::move(p));
            }
            lua_pop(L, 1);                                // env, props, key
        }
    }
    lua_pop(L, 2);                                        // (empty)
}

bool ScriptSystem::get_prop(components::ScriptComponent* comp, const char* name,
                            int& out_type, float& out_f, std::string& out_s) {
    if (!comp || !name) return false;
    for (const auto& p : comp->props) {
        if (p.name == name) {
            out_type = p.type;
            out_f = p.f;
            out_s = p.s;
            return true;
        }
    }
    return false;
}

bool ScriptSystem::set_prop(components::ScriptComponent* comp, const char* name, float value) {
    if (!comp || !name) return false;
    for (auto& p : comp->props) {
        if (p.name == name && p.type == 0) {
            p.f = value;
            write_prop_to_env(comp, name, value);
            return true;
        }
    }
    return false;
}

bool ScriptSystem::set_prop(components::ScriptComponent* comp, const char* name,
                            const std::string& value) {
    if (!comp || !name) return false;
    for (auto& p : comp->props) {
        if (p.name == name && p.type == 1) {
            p.s = value;
            write_prop_to_env(comp, name, value);
            return true;
        }
    }
    return false;
}

void ScriptSystem::write_prop_to_env(components::ScriptComponent* comp,
                                     const char* name, float value) {
    auto& rt = script::LuaRuntime::instance();
    lua_State* L = rt.state();
    if (!L || comp->env_ref < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, comp->env_ref);   // env
    lua_pushnumber(L, value);
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

void ScriptSystem::write_prop_to_env(components::ScriptComponent* comp,
                                     const char* name, const std::string& value) {
    auto& rt = script::LuaRuntime::instance();
    lua_State* L = rt.state();
    if (!L || comp->env_ref < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, comp->env_ref);   // env
    lua_pushlstring(L, value.c_str(), value.size());
    lua_setfield(L, -2, name);
    lua_pop(L, 1);
}

void ScriptSystem::unload(components::ScriptComponent* comp) {
    if (comp->script_loaded && comp->env_ref >= 0) {
        call_method(comp, "on_destroy");
    }
    auto& rt = script::LuaRuntime::instance();
    lua_State* L = rt.state();
    if (L) {
        if (comp->env_ref >= 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, comp->env_ref);
            comp->env_ref = -1;
        }
        if (comp->chunk_ref >= 0) {
            luaL_unref(L, LUA_REGISTRYINDEX, comp->chunk_ref);
            comp->chunk_ref = -1;
        }
    }
    comp->script_loaded = false;
    comp->start_called = false;
}

void ScriptSystem::call_method(components::ScriptComponent* comp,
                               const char* method, float arg, bool has_arg) {
    auto& rt = script::LuaRuntime::instance();
    lua_State* L = rt.state();
    if (!L || comp->env_ref < 0) return;

    lua_rawgeti(L, LUA_REGISTRYINDEX, comp->env_ref);   // env
    lua_getfield(L, -1, method);                        // env, fn
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }

    rt.set_current_entity(comp->owner());
    if (has_arg) lua_pushnumber(L, arg);
    const int rc = lua_pcall(L, has_arg ? 1 : 0, 0, 0);
    if (rc != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        comp->last_error = msg ? msg : "script error";
        lua_pop(L, 1);                                  // error
        lua_pop(L, 1);                                  // env
        handle_error(comp);
        return;
    }
    lua_pop(L, 1);                                      // env
}

void ScriptSystem::handle_error(components::ScriptComponent* comp) {
    if (comp->reported_error) return;
    comp->reported_error = true;
    GLOG_ERROR("GryceSRT: script error on '{}': {}",
               comp->owner() ? comp->owner()->name() : "?",
               comp->last_error);
}

void ScriptSystem::reload_all() {
    source_cache_.clear();
    for (auto* comp : loaded_) unload(comp);
    loaded_.clear();
    seen_.clear();
}

void ScriptSystem::on_shutdown(scene::Scene& scene) {
    (void)scene;
    reload_all();
}

} // namespace gryce_engine::ecs
