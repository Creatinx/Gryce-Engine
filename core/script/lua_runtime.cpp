#include "script/lua_runtime.h"

#include "api/internal_state.h"
#include "components/transform.h"
#include "ecs/world.h"
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

#include <fstream>
#include <sstream>

namespace gryce_engine::script {

namespace {

using gryce_engine::math::Vector3f;
using gryce_engine::math::Quaternionf;

// gryce.log.info / warn / error(...)
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

// gryce.version()
int l_version(lua_State* L) {
    lua_pushfstring(L, "GryceSRT 0.1.0 (%s)", LUA_RELEASE);
    return 1;
}

const luaL_Reg kGryceLib[] = {
    {"version", l_version},
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

// gryce.self() -> entity handle of the currently running script
int l_self(lua_State* L) {
    auto* e = gryce_engine::script::LuaRuntime::instance().current_entity();
    if (!e) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, gryce_core::g_core_state.entity_map.lookup(e->uuid()));
    return 1;
}

// gryce.entity.get_name(h) -> string
int l_entity_get_name(lua_State* L) {
    const auto h = static_cast<GEntityHandle>(luaL_checkinteger(L, 1));
    auto* e = gryce_core::EntityResolver::resolve(h);
    lua_pushstring(L, e ? e->name().c_str() : "");
    return 1;
}

// gryce.entity.find(name) -> handle (0 if not found)
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

// gryce.entity.get_transform(h) -> {pos={x,y,z}, rot={x,y,z,w}, scale={x,y,z}}
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

// gryce.entity.set_transform(h, {pos={x,y,z}}, {rot={x,y,z,w}}, {scale={x,y,z}})
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

// gryce.time.delta() / elapsed()
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

// gryce.input.key_down(key) / mouse_pos()
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

const luaL_Reg kInputLib[] = {
    {"key_down", l_input_key_down},
    {"mouse_pos", l_input_mouse_pos},
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
    register_gryce_bindings();
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

void LuaRuntime::register_gryce_bindings() {
    lua_getglobal(L_, "gryce");
    if (lua_isnil(L_, -1)) {
        lua_pop(L_, 1);
        lua_newtable(L_);
        lua_setglobal(L_, "gryce");
        lua_getglobal(L_, "gryce");
    }

    lua_newtable(L_);
    luaL_setfuncs(L_, kLogLib, 0);
    lua_setfield(L_, -2, "log");

    lua_newtable(L_);
    luaL_setfuncs(L_, kEntityLib, 0);
    lua_setfield(L_, -2, "entity");

    lua_newtable(L_);
    luaL_setfuncs(L_, kTimeLib, 0);
    lua_setfield(L_, -2, "time");

    lua_newtable(L_);
    luaL_setfuncs(L_, kInputLib, 0);
    lua_setfield(L_, -2, "input");

    luaL_setfuncs(L_, kGryceLib, 0);
    lua_pop(L_, 1);
}

} // namespace gryce_engine::script
