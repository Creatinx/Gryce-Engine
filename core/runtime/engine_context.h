#pragma once

#include "GryceCore/types.h"
#include "runtime/command_buffer.h"
#include "runtime/entity_handle_map.h"

#include <memory>
#include <string>
#include <vector>
#include <unordered_set>
#include <atomic>
#include <mutex>

namespace gryce_engine { namespace ecs { class World; } }
namespace gryce_engine { namespace scene { class Scene; } }
namespace gryce_engine { namespace components { class Component; } }

namespace gryce_core {

// ---------------------------------------------------------------------------
// 引擎运行时上下文（core/runtime）
//
// 引擎的全局运行时状态从 api/internal_state.h 移到这里，作为与 C API 胶水
// 层无关的领域模块：script/ecs 等核心代码只依赖 runtime/engine_context.h，
// 不再反向依赖 api/。后续可按需注入到 World / LuaRuntime 而非直接读全局。
// ---------------------------------------------------------------------------

// 回调表（C API 层向编辑器转发事件）
struct CallbackTable {
    GOnEntitySelected on_entity_selected = nullptr;
    GOnEntityDeselected on_entity_deselected = nullptr;
    GOnSceneLoaded on_scene_loaded = nullptr;
    GOnPlayModeChanged on_play_mode_changed = nullptr;
    GOnEntityListChanged on_entity_list_changed = nullptr;
    GOnComponentChanged on_component_changed = nullptr;
    GOnLogMessage on_log_message = nullptr;
    GOnMouseLock on_mouse_lock = nullptr;
};

// Input event kinds dispatched to scripts' _input handler (matches the C
// constants exposed on engine.input so scripts can branch on the event type).
enum InputEventKinds {
    INPUT_EVENT_KEY_DOWN = 1,
    INPUT_EVENT_KEY_UP = 2,
    INPUT_EVENT_MOUSE_MOVE = 3,
    INPUT_EVENT_MOUSE_DOWN = 4,
    INPUT_EVENT_MOUSE_UP = 5,
};

// A single input event queued this frame by process_command and drained by
// ScriptSystem::on_update (before per-entity scheduling). a/b/c are the
// positional args forwarded to the script's _input(type, a, b, c).
struct InputEvent {
    int type = 0;
    int a = 0;
    int b = 0;
    int c = 0;
};

// 输入状态（供 engine.input 查询；由 ECMD_INPUT_* 命令更新）
struct InputState {
    std::unordered_set<int> keys_down;
    int mouse_x = 0;
    int mouse_y = 0;
    bool mouse_button[3] = {};
    // 本帧累计鼠标移动增量（像素）。由 ECMD_INPUT_MOUSE_MOVE 累加，
    // 在 GCore_BeginFrame 每帧开始前清零。用 float 保留亚像素精度，
    // 避免慢速移动时被 int 截断成 0 导致视角卡顿。
    float mouse_delta_x = 0.0f;
    float mouse_delta_y = 0.0f;
    // 上次已计入 delta 的绝对位置快照（用于计算跨 move 事件的增量）
    float mouse_snap_x = -1.0f;
    float mouse_snap_y = -1.0f;
    // 鼠标锁定状态（FPS 视角用；由 engine.input.mouse_locked 请求驱动）
    bool mouse_locked = false;
    // Input events queued this frame (filled by process_command, drained by
    // ScriptSystem::on_update). Cleared each frame after dispatch.
    std::vector<InputEvent> input_events;
};

// ---------------------------------------------------------------------------
// 引擎全局上下文
// ---------------------------------------------------------------------------
struct EngineContext {
    bool initialized = false;
    std::mutex init_mutex;
    std::unique_ptr<gryce_engine::ecs::World> world;
    CommandBuffer cmdbuf;
    EntityHandleMap entity_map;
    CallbackTable callbacks;
    void* callback_user_data = nullptr;

    bool play_mode = false;
    bool paused = false;
    // Game entry (GryceGame): when true, GCore_Init loads the project's main
    // scene right after startup. Set via GCore_SetAutoLoadMainScene; the
    // editor leaves it false and manages scenes itself.
    bool auto_load_main_scene = false;
    // Play Mode 进入时保存的场景快照（JSON）；Stop 时据此恢复场景。
    std::string play_snapshot_json;
    GEntityHandle selected_entity = 0;
    std::string current_scene_path;

    // --- 2D / 3D 双场景槽（编辑器热切换，不释放场景内存）---
    // scene_mode: 0 = 2D 场景编辑器，1 = 3D 场景编辑器（当前活动场景归属）
    int scene_mode = 0;
    std::unique_ptr<gryce_engine::scene::Scene> scene_slot_2d;
    std::unique_ptr<gryce_engine::scene::Scene> scene_slot_3d;
    std::string scene_path_2d;
    std::string scene_path_3d;

    // 输入状态
    InputState input;

    bool deferred_entity_list_changed = false;
    bool deferred_selection_changed = false;
    bool deferred_scene_loaded = false;

    // Console: number of MemoryLogSink entries already delivered to the editor
    size_t log_delivered_count = 0;
};

// Defined in core_api.cpp
extern EngineContext g_core_state;

// Helper: resolve EntityHandle -> Entity* via UUID
struct EntityResolver {
    static gryce_engine::scene::Entity* resolve(GEntityHandle h);
};

// Helper: get type name string from a Component* pointer (defined in core_api.cpp)
std::string get_component_type_name(gryce_engine::components::Component* comp);

// 反射注册表使用短名，组件类型名是完整命名空间名；查询前剥离前缀。
// （defined in core_api.cpp）
std::string reflection_lookup_name(const std::string& full_name);

} // namespace gryce_core
