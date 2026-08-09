#pragma once

#include <string>

struct lua_State;

namespace gryce_engine::script {

/// GryceSRT runtime: owns the global Lua state and the `gryce.*` bindings.
/// Lifecycle is tied to the core (GCore_Init creates it, GCore_Shutdown
/// destroys it). All script execution happens on the game thread inside
/// GCore_BeginFrame; the editor UI never touches this state.
class LuaRuntime {
public:
    static LuaRuntime& instance();

    bool init();
    void shutdown();

    lua_State* state() const { return L_; }
    bool initialized() const { return L_ != nullptr; }

    /// Runs a Lua chunk. Returns true on success; on failure returns false and
    /// stores the error message in err (if provided).
    bool run_string(const char* code, std::string* err = nullptr);
    bool run_file(const char* path, std::string* err = nullptr);

private:
    LuaRuntime() = default;
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    void register_gryce_bindings();

    lua_State* L_ = nullptr;
};

} // namespace gryce_engine::script
