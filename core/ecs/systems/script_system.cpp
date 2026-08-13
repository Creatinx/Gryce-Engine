#include "ecs/systems/script_system.h"

#include "components/script_component.h"
#include "assets/asset_manager.h"
#include "resources/resource_path.h"
#include "scene/entity.h"
#include "scene/scene.h"
#include "ecs/world.h"
#include "script/lua_runtime.h"
#include "api/internal_state.h"
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
    rt.set_current_scene(&scene);
    seen_.clear();

    // 输入事件分发：把本帧缓存的事件逐条派发给定义 _input 的脚本
    // （先分发再执行 on_update，类比 Godot 的 _input 早于 _process）。
    dispatch_input_events();

    // 快照遍历：脚本在 on_update 里通过 engine.entity.create 创建实体是安全的
    // （新实体下一帧才进入脚本驱动，避免遍历期间修改场景层级）。
    std::vector<scene::Entity*> entities;
    scene.root()->foreach([&](scene::Entity* e) {
        entities.push_back(e);
    });

    // 按 process_priority 降序收集所有脚本组件，值越大 on_update 越先执行。
    std::vector<components::ScriptComponent*> comps;
    for (scene::Entity* e : entities) {
        auto* comp = e->get_component<components::ScriptComponent>();
        if (!comp) continue;
        comps.push_back(comp);
        seen_.push_back(comp);
    }
    std::stable_sort(comps.begin(), comps.end(),
        [](components::ScriptComponent* a, components::ScriptComponent* b) {
            return a->process_priority > b->process_priority;
        });
    for (components::ScriptComponent* comp : comps) {
        process_entity(comp, dt);
    }

    // engine.entity.destroy 的延迟销毁：先卸载脚本（env/unref），再销毁实体，
    // 避免遍历期间销毁 + 脚本环境泄漏。
    for (scene::Entity* e : rt.take_pending_destroy()) {
        if (!e) continue;
        if (auto* comp = e->get_component<components::ScriptComponent>()) {
            unload(comp);
            loaded_.erase(std::remove(loaded_.begin(), loaded_.end(), comp), loaded_.end());
        }
        scene.destroy_entity(e);
    }

    // Drop bookkeeping for components that no longer exist (removed via the
    // editor or other paths; the component object is still alive so unload is
    // safe and releases its Lua environment).
    loaded_.erase(std::remove_if(loaded_.begin(), loaded_.end(),
        [&](components::ScriptComponent* c) {
            if (contains(seen_, c)) return false;
            unload(c);
            return true;
        }),
        loaded_.end());
}

void ScriptSystem::process_entity(components::ScriptComponent* comp, float dt) {
    if (!comp) return;

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

    // per-node 暂停（pause_mode）：全局暂停时，只有 pause_mode 的脚本继续更新。
    if (gryce_core::g_core_state.paused && !comp->pause_mode) {
        return;
    }

    call_method(comp, "on_update", dt, true);
}

// 把本帧缓存的输入事件逐条派发给定义了 _input 的脚本组件。
// _input(type, a, b, c)：type 为 engine.input 的常量，a/b/c 是位置参数。
void ScriptSystem::dispatch_input_events() {
    auto& state = gryce_core::g_core_state;
    if (state.input_events.empty()) return;

    std::vector<components::ScriptComponent*> comps;
    if (state.world) {
        scene::Scene* scene = state.world->scene();
        if (scene) {
            scene->root()->foreach([&](scene::Entity* e) {
                auto* c = e->get_component<components::ScriptComponent>();
                if (c && c->script_loaded) comps.push_back(c);
            });
        }
    }

    for (const auto& ev : state.input_events) {
        for (components::ScriptComponent* comp : comps) {
            call_method_int(comp, "_input", ev.type, ev.a, ev.b, ev.c);
        }
    }
    state.input_events.clear();
}

bool ScriptSystem::load(components::ScriptComponent* comp) {
    auto& rt = script::LuaRuntime::instance();
    lua_State* L = rt.state();
    if (!L) return false;

    comp->last_error.clear();
    comp->reported_error = false;

    const std::string full = assets::AssetManager::instance().resolve_for_reading(comp->script_path);
    if (full.empty()) {
        comp->last_error = "cannot resolve script: " + comp->script_path;
        return false;
    }
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

    // 热重载前清空旧信号连接并释放其回调引用，避免 on_start 里重复 connect。
    for (const auto& sig : comp->signals) {
        if (sig.callback_ref >= 0) luaL_unref(L, LUA_REGISTRYINDEX, sig.callback_ref);
    }
    comp->signals.clear();

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

// 调用脚本的带整型参数方法（用于 _input 事件分发）。
void ScriptSystem::call_method_int(components::ScriptComponent* comp,
                                   const char* method, int type, int a, int b, int c) {
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
    lua_pushinteger(L, type);
    lua_pushinteger(L, a);
    lua_pushinteger(L, b);
    lua_pushinteger(L, c);
    const int rc = lua_pcall(L, 4, 0, 0);
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
    script::LuaRuntime::instance().set_current_scene(nullptr);
}

} // namespace gryce_engine::ecs
