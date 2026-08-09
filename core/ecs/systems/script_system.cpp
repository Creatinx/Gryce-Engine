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
    std::ifstream in(full, std::ios::binary);
    if (!in) {
        comp->last_error = "cannot open script file: " + full;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    if (src.empty()) {
        comp->last_error = "empty script: " + comp->script_path;
        return false;
    }

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
    return true;
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
    for (auto* comp : loaded_) unload(comp);
    loaded_.clear();
    seen_.clear();
}

void ScriptSystem::on_shutdown(scene::Scene& scene) {
    (void)scene;
    reload_all();
}

} // namespace gryce_engine::ecs
