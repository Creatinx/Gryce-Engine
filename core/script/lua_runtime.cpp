#include "script/lua_runtime.h"

#include "GryceCore/types.h"
#include "api/internal_state.h"
#include "components/transform.h"
#include "ecs/world.h"
#include "reflection/reflection.h"
#include "resources/project.h"
#include "scene/entity.h"
#include "scene/scene.h"

// Lua headers do not self-wrap with extern "C"; keep C linkage so the static
// library symbols (compiled as C) resolve from this C++ TU.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "utils/glog/glog_lib.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace gryce_engine::script {

namespace {

using gryce_engine::math::Vector3f;
using gryce_engine::math::Quaternionf;

// engine.log.info / warn / error(...)
int l_log_info(lua_State* L) {
    const char* msg = luaL_optstring(L, 1, "");
    GLOG_INFO("{}", msg ? msg : "");
    return 0;
}

int l_log_warn(lua_State* L) {
    const char* msg = luaL_optstring(L, 1, "");
    GLOG_WARN("{}", msg ? msg : "");
    return 0;
}

int l_log_error(lua_State* L) {
    const char* msg = luaL_optstring(L, 1, "");
    GLOG_ERROR("{}", msg ? msg : "");
    return 0;
}

const luaL_Reg kLogLib[] = {
    {"info",  l_log_info},
    {"warn",  l_log_warn},
    {"error", l_log_error},
    {nullptr, nullptr}
};

// engine.version()
int l_version(lua_State* L) {
    lua_pushfstring(L, "GryceSRT 0.1.0 (%s)", LUA_RELEASE);
    return 1;
}

int l_self(lua_State* L);  // defined below with the entity helpers

const luaL_Reg kEngineLib[] = {
    {"version", l_version},
    {"self", l_self},
    {nullptr, nullptr}
};

// --- entity helpers ------------------------------------------------------

static bool read_vec3(lua_State* L, int idx, Vector3f& out) {
    if (!lua_istable(L, idx)) return false;
    lua_getfield(L, idx, "x");
    lua_getfield(L, idx, "y");
    lua_getfield(L, idx, "z");
    out.x = static_cast<float>(lua_tonumber(L, -3));
    out.y = static_cast<float>(lua_tonumber(L, -2));
    out.z = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 3);
    return true;
}

static void push_vec3(lua_State* L, const Vector3f& v) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.z); lua_setfield(L, -2, "z");
}

static void push_quat(lua_State* L, const Quaternionf& q) {
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, q.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, q.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, q.z); lua_setfield(L, -2, "z");
    lua_pushnumber(L, q.w); lua_setfield(L, -2, "w");
}

// engine.self() -> entity handle of the currently running script
int l_self(lua_State* L) {
    auto* e = gryce_engine::script::LuaRuntime::instance().current_entity();
    if (!e) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, gryce_core::g_core_state.entity_map.lookup(e->uuid()));
    return 1;
}

// engine.entity.get_name(h) -> string
int l_entity_get_name(lua_State* L) {
    const auto h = static_cast<GEntityHandle>(luaL_checkinteger(L, 1));
    auto* e = gryce_core::EntityResolver::resolve(h);
    lua_pushstring(L, e ? e->name().c_str() : "");
    return 1;
}

// engine.entity.find(name) -> handle (0 if not found)
int l_entity_find(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto* scene = gryce_core::g_core_state.world
        ? gryce_core::g_core_state.world->scene() : nullptr;
    if (!scene) { lua_pushinteger(L, 0); return 1; }
    auto* e = scene->find_entity_by_name(name ? name : "");
    if (!e) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, gryce_core::g_core_state.entity_map.lookup(e->uuid()));
    return 1;
}

// engine.entity.get_transform(h) -> {pos={x,y,z}, rot={x,y,z,w}, scale={x,y,z}}
int l_entity_get_transform(lua_State* L) {
    const auto h = static_cast<GEntityHandle>(luaL_checkinteger(L, 1));
    auto* e = gryce_core::EntityResolver::resolve(h);
    if (!e || !e->transform()) { lua_pushnil(L); return 1; }
    auto* t = e->transform();
    lua_createtable(L, 0, 3);
    push_vec3(L, t->position);
    lua_setfield(L, -2, "pos");
    push_quat(L, t->rotation);
    lua_setfield(L, -2, "rot");
    push_vec3(L, t->scale);
    lua_setfield(L, -2, "scale");
    return 1;
}

// engine.entity.set_transform(h, {pos={x,y,z}}, {rot={x,y,z,w}}, {scale={x,y,z}})
int l_entity_set_transform(lua_State* L) {
    const auto h = static_cast<GEntityHandle>(luaL_checkinteger(L, 1));
    auto* e = gryce_core::EntityResolver::resolve(h);
    if (!e || !e->transform()) return 0;
    auto* t = e->transform();
    Vector3f pos, scl;
    Quaternionf rot;
    if (read_vec3(L, 2, pos)) t->position = pos;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "x"); lua_getfield(L, 3, "y");
        lua_getfield(L, 3, "z"); lua_getfield(L, 3, "w");
        rot.x = static_cast<float>(lua_tonumber(L, -4));
        rot.y = static_cast<float>(lua_tonumber(L, -3));
        rot.z = static_cast<float>(lua_tonumber(L, -2));
        rot.w = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 4);
        t->rotation = rot;
    }
    if (read_vec3(L, 4, scl)) t->scale = scl;
    e->mark_dirty();
    return 0;
}

const luaL_Reg kEntityLib[] = {
    {"get_name", l_entity_get_name},
    {"find", l_entity_find},
    {"get_transform", l_entity_get_transform},
    {"set_transform", l_entity_set_transform},
    {nullptr, nullptr}
};

// engine.component.get(h, type_name, prop_name) -> value
int l_component_get(lua_State* L) {
    const auto h = static_cast<GEntityHandle>(luaL_checkinteger(L, 1));
    const char* type_name = luaL_checkstring(L, 2);
    const char* prop_name = luaL_checkstring(L, 3);
    auto* e = gryce_core::EntityResolver::resolve(h);
    if (!e || !type_name || !prop_name) { lua_pushnil(L); return 1; }

    auto* comp = e->get_component_by_type(type_name);
    if (!comp) { lua_pushnil(L); return 1; }
    auto fields = gryce_engine::reflection::Registry::instance().all_fields(
        gryce_core::reflection_lookup_name(type_name));
    for (const auto* f : fields) {
        if (f->name != prop_name || !f->read) continue;
        switch (f->type) {
            case gryce_engine::reflection::FieldType::Float: {
                float v = 0.0f; f->read(comp, &v); lua_pushnumber(L, v); return 1;
            }
            case gryce_engine::reflection::FieldType::Double: {
                double v = 0.0; f->read(comp, &v); lua_pushnumber(L, v); return 1;
            }
            case gryce_engine::reflection::FieldType::Int: {
                int v = 0; f->read(comp, &v); lua_pushinteger(L, v); return 1;
            }
            case gryce_engine::reflection::FieldType::Bool: {
                bool v = false; f->read(comp, &v); lua_pushboolean(L, v); return 1;
            }
            case gryce_engine::reflection::FieldType::String: {
                std::string v; f->read(comp, &v);
                lua_pushlstring(L, v.c_str(), v.size()); return 1;
            }
            case gryce_engine::reflection::FieldType::Vector2f: {
                math::Vector2f v; f->read(comp, &v);
                lua_createtable(L, 0, 2);
                lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
                lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
                return 1;
            }
            case gryce_engine::reflection::FieldType::Vector3f: {
                math::Vector3f v; f->read(comp, &v);
                push_vec3(L, v);
                return 1;
            }
            case gryce_engine::reflection::FieldType::Vector4f: {
                math::Vector4f v; f->read(comp, &v);
                lua_createtable(L, 0, 4);
                lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
                lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
                lua_pushnumber(L, v.z); lua_setfield(L, -2, "z");
                lua_pushnumber(L, v.w); lua_setfield(L, -2, "w");
                return 1;
            }
            case gryce_engine::reflection::FieldType::Quaternionf: {
                math::Quaternionf v; f->read(comp, &v);
                push_quat(L, v);
                return 1;
            }
            default:
                lua_pushnil(L);
                return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

// engine.component.set(h, type_name, prop_name, value)
int l_component_set(lua_State* L) {
    const auto h = static_cast<GEntityHandle>(luaL_checkinteger(L, 1));
    const char* type_name = luaL_checkstring(L, 2);
    const char* prop_name = luaL_checkstring(L, 3);
    auto* e = gryce_core::EntityResolver::resolve(h);
    if (!e || !type_name || !prop_name) return 0;

    auto* comp = e->get_component_by_type(type_name);
    if (!comp) return 0;
    auto fields = gryce_engine::reflection::Registry::instance().all_fields(
        gryce_core::reflection_lookup_name(type_name));
    for (const auto* f : fields) {
        if (f->name != prop_name || !f->write || f->read_only) continue;
        switch (f->type) {
            case gryce_engine::reflection::FieldType::Float: {
                float v = static_cast<float>(luaL_checknumber(L, 4));
                if (f->write(comp, &v)) e->mark_dirty();
                return 0;
            }
            case gryce_engine::reflection::FieldType::Double: {
                double v = static_cast<double>(luaL_checknumber(L, 4));
                if (f->write(comp, &v)) e->mark_dirty();
                return 0;
            }
            case gryce_engine::reflection::FieldType::Int: {
                int v = static_cast<int>(luaL_checkinteger(L, 4));
                if (f->write(comp, &v)) e->mark_dirty();
                return 0;
            }
            case gryce_engine::reflection::FieldType::Bool: {
                bool v = lua_toboolean(L, 4) != 0;
                if (f->write(comp, &v)) e->mark_dirty();
                return 0;
            }
            case gryce_engine::reflection::FieldType::String: {
                const char* s = luaL_checkstring(L, 4);
                std::string v = s ? s : "";
                if (f->write(comp, &v)) e->mark_dirty();
                return 0;
            }
            case gryce_engine::reflection::FieldType::Vector3f: {
                math::Vector3f v;
                if (read_vec3(L, 4, v)) {
                    if (f->write(comp, &v)) e->mark_dirty();
                }
                return 0;
            }
            case gryce_engine::reflection::FieldType::Vector2f: {
                math::Vector2f v;
                if (lua_istable(L, 4)) {
                    lua_getfield(L, 4, "x");
                    lua_getfield(L, 4, "y");
                    v.x = static_cast<float>(lua_tonumber(L, -2));
                    v.y = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 2);
                    if (f->write(comp, &v)) e->mark_dirty();
                }
                return 0;
            }
            case gryce_engine::reflection::FieldType::Quaternionf: {
                math::Quaternionf v;
                if (lua_istable(L, 4)) {
                    lua_getfield(L, 4, "x"); lua_getfield(L, 4, "y");
                    lua_getfield(L, 4, "z"); lua_getfield(L, 4, "w");
                    v.x = static_cast<float>(lua_tonumber(L, -4));
                    v.y = static_cast<float>(lua_tonumber(L, -3));
                    v.z = static_cast<float>(lua_tonumber(L, -2));
                    v.w = static_cast<float>(lua_tonumber(L, -1));
                    lua_pop(L, 4);
                    if (f->write(comp, &v)) e->mark_dirty();
                }
                return 0;
            }
            default:
                return 0;
        }
    }
    return 0;
}

const luaL_Reg kComponentLib[] = {
    {"get", l_component_get},
    {"set", l_component_set},
    {nullptr, nullptr}
};

// engine.time.delta() / elapsed()
int l_time_delta(lua_State* L) {
    lua_pushnumber(L, gryce_engine::script::LuaRuntime::instance().delta());
    return 1;
}

int l_time_elapsed(lua_State* L) {
    lua_pushnumber(L, gryce_engine::script::LuaRuntime::instance().elapsed());
    return 1;
}

const luaL_Reg kTimeLib[] = {
    {"delta", l_time_delta},
    {"elapsed", l_time_elapsed},
    {nullptr, nullptr}
};

// engine.input.key_down(key) / mouse_pos()
int l_input_key_down(lua_State* L) {
    const int key = static_cast<int>(luaL_checkinteger(L, 1));
    const bool down = gryce_core::g_core_state.keys_down.count(key) > 0;
    lua_pushboolean(L, down);
    return 1;
}

int l_input_mouse_pos(lua_State* L) {
    lua_pushinteger(L, gryce_core::g_core_state.mouse_x);
    lua_pushinteger(L, gryce_core::g_core_state.mouse_y);
    return 2;
}

int l_input_mouse_down(lua_State* L) {
    const int b = static_cast<int>(luaL_checkinteger(L, 1));
    const bool down = b >= 0 && b < 3 && gryce_core::g_core_state.mouse_button[b];
    lua_pushboolean(L, down);
    return 1;
}

const luaL_Reg kInputLib[] = {
    {"key_down", l_input_key_down},
    {"mouse_pos", l_input_mouse_pos},
    {"mouse_down", l_input_mouse_down},
    {nullptr, nullptr}
};

// engine.scene.load(path) -> 0 on success, -1 on failure.
// The switch is deferred through the command buffer so it happens after the
// current frame's world update (safe to call from on_update).
int l_scene_load(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!path || !path[0]) {
        lua_pushinteger(L, -1);
        return 1;
    }
    if (!gryce_core::g_core_state.initialized || !gryce_core::g_core_state.world) {
        lua_pushinteger(L, -1);
        return 1;
    }
    GCommand cmd{};
    cmd.type = ECMD_LOAD_SCENE;
    std::strncpy(reinterpret_cast<char*>(cmd.payload), path, GCMD_PAYLOAD_SIZE - 1);
    const bool ok = gryce_core::g_core_state.cmdbuf.push(cmd);
    lua_pushinteger(L, ok ? 0 : -1);
    return 1;
}

// engine.scene.current() -> res:/ path of the active scene, or nil.
int l_scene_current(lua_State* L) {
    const std::string& p = gryce_core::g_core_state.current_scene_path;
    if (p.empty()) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, p.c_str());
    }
    return 1;
}

const luaL_Reg kSceneLib[] = {
    {"load", l_scene_load},
    {"current", l_scene_current},
    {nullptr, nullptr}
};

} // namespace

LuaRuntime& LuaRuntime::instance() {
    static LuaRuntime rt;
    return rt;
}

bool LuaRuntime::init() {
    if (L_) return true;

    L_ = luaL_newstate();
    if (!L_) {
        GLOG_ERROR("GryceSRT: failed to create Lua state");
        return false;
    }

    luaL_openlibs(L_);
    register_engine_bindings();
    GLOG_INFO("GryceSRT: Lua runtime initialized ({})", LUA_RELEASE);
    return true;
}

void LuaRuntime::shutdown() {
    if (L_) {
        lua_close(L_);
        L_ = nullptr;
        GLOG_INFO("GryceSRT: Lua runtime shut down");
    }
}

bool LuaRuntime::run_string(const char* code, std::string* err) {
    if (!L_ || !code) return false;

    if (luaL_loadstring(L_, code) != LUA_OK) {
        if (err) {
            const char* e = lua_tostring(L_, -1);
            *err = e ? e : "unknown Lua compile error";
        }
        lua_pop(L_, 1);
        return false;
    }

    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        if (err) {
            const char* e = lua_tostring(L_, -1);
            *err = e ? e : "unknown Lua runtime error";
        }
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

bool LuaRuntime::run_file(const char* path, std::string* err) {
    if (!L_ || !path) return false;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = std::string("cannot open script file: ") + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return run_string(ss.str().c_str(), err);
}

void LuaRuntime::register_engine_bindings() {
    lua_getglobal(L_, "engine");
    if (lua_isnil(L_, -1)) {
        lua_pop(L_, 1);
        lua_newtable(L_);
        lua_setglobal(L_, "engine");
        lua_getglobal(L_, "engine");
    }

    lua_newtable(L_);
    luaL_setfuncs(L_, kLogLib, 0);
    lua_setfield(L_, -2, "log");

    lua_newtable(L_);
    luaL_setfuncs(L_, kEntityLib, 0);
    lua_setfield(L_, -2, "entity");

    lua_newtable(L_);
    luaL_setfuncs(L_, kComponentLib, 0);
    lua_setfield(L_, -2, "component");

    lua_newtable(L_);
    luaL_setfuncs(L_, kTimeLib, 0);
    lua_setfield(L_, -2, "time");

    lua_newtable(L_);
    luaL_setfuncs(L_, kInputLib, 0);
    lua_setfield(L_, -2, "input");

    lua_newtable(L_);
    luaL_setfuncs(L_, kSceneLib, 0);
    lua_setfield(L_, -2, "scene");

    luaL_setfuncs(L_, kEngineLib, 0);
    lua_pop(L_, 1);

    // Make require("GryceEngineUtils") resolve against the project's
    // scripts/ folder (project root is set before the runtime inits).
    const std::string root = resources::Project::instance().root();
    if (!root.empty()) {
        const std::string scripts = root + "/scripts/?.lua";
        lua_getglobal(L_, "package");
        lua_getfield(L_, -1, "path");
        const char* old = lua_tostring(L_, -1);
        const std::string new_path = scripts + ";" + (old ? old : "");
        lua_pop(L_, 1);
        lua_pushstring(L_, new_path.c_str());
        lua_setfield(L_, -2, "path");
        lua_pop(L_, 1);
    }
}

} // namespace gryce_engine::script
