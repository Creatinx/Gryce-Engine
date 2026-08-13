#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "export.h"
#include "scene/uuid.h"

struct lua_State;

namespace gryce_engine { namespace scene { class Entity; } }
namespace gryce_engine { namespace scene { class Scene; } }

namespace gryce_engine::script {

/// GryceSRT runtime: owns the global Lua state and the `engine.*` bindings.
/// Lifecycle is tied to the core (GCore_Init creates it, GCore_Shutdown
/// destroys it). All script execution happens on the game thread inside
/// GCore_BeginFrame; the editor UI never touches this state.
class GRYCE_API LuaRuntime {
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

    // Scene / entity handle registry（由 ScriptSystem 每帧喂入当前场景）：
    // 脚本通过 engine.entity.* 拿到的句柄在本运行时内唯一且稳定，
    // 与编辑器/独立 exe 的 C API 句柄解耦，运行时创建的实体同样可用。
    void set_current_scene(scene::Scene* scene);
    scene::Scene* current_scene() const { return current_scene_; }

    int entity_handle(scene::Entity* e);
    scene::Entity* entity_by_handle(int h) const;

    // 延迟销毁（脚本遍历期间排队，ScriptSystem 遍历结束后统一执行）
    void queue_destroy(scene::Entity* e);
    std::vector<scene::Entity*> take_pending_destroy();

    // engine.state 共享状态表（跨实体/跨场景存续）
    int state_table_ref() const { return state_table_ref_; }

    void set_delta(float dt) { delta_ = dt; elapsed_ += dt; }
    float delta() const { return delta_; }
    float elapsed() const { return elapsed_; }

private:
    LuaRuntime() = default;
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    void register_engine_bindings();

    lua_State* L_ = nullptr;
    scene::Scene* current_scene_ = nullptr;
    int next_handle_ = 1;
    std::unordered_map<scene::UUID, int> entity_handles_;
    std::unordered_map<int, scene::Entity*> entities_by_handle_;
    std::vector<scene::Entity*> pending_destroy_;
    int state_table_ref_ = -1;
    scene::Entity* current_entity_ = nullptr;
    float delta_ = 0.0f;
    float elapsed_ = 0.0f;
};

} // namespace gryce_engine::script
