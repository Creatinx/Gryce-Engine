#include "GryceCore/script_api.h"

#include "script/lua_runtime.h"

#include <cstring>
#include <string>

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

} // extern "C"
