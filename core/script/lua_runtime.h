#pragma once

#include <string>

struct lua_State;

namespace gryce_engine { namespace scene { class Entity; } }

namespace gryce_engine::script {

/// GryceSRT runtime: owns the global Lua state and the `engine.*` bindings.
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

    // Per-frame context pushed by ScriptSystem before invoking a script:
    void set_current_entity(scene::Entity* e) { current_entity_ = e; }
    scene::Entity* current_entity() const { return current_entity_; }

    void set_delta(float dt) { delta_ = dt; elapsed_ += dt; }
    float delta() const { return delta_; }
    float elapsed() const { return elapsed_; }

private:
    LuaRuntime() = default;
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    void register_engine_bindings();

    lua_State* L_ = nullptr;
    scene::Entity* current_entity_ = nullptr;
    float delta_ = 0.0f;
    float elapsed_ = 0.0f;
};

} // namespace gryce_engine::script
