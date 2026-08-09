#include "script/lua_runtime.h"

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

    luaL_setfuncs(L_, kGryceLib, 0);
    lua_pop(L_, 1);
}

} // namespace gryce_engine::script
