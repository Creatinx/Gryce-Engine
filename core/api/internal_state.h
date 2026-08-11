#pragma once

#include "GryceCore/types.h"
#include "command_buffer.h"
#include "entity_handle_map.h"

#include <memory>
#include <string>
#include <unordered_set>
#include <atomic>
#include <mutex>

namespace gryce_engine { namespace ecs { class World; } }
namespace gryce_engine { namespace components { class Component; } }

namespace gryce_core {

// ---------------------------------------------------------------------------
// Shared internal state — single instance lives in core_api.cpp
// ---------------------------------------------------------------------------
struct CallbackTable {
    GOnEntitySelected on_entity_selected = nullptr;
    GOnEntityDeselected on_entity_deselected = nullptr;
    GOnSceneLoaded on_scene_loaded = nullptr;
    GOnPlayModeChanged on_play_mode_changed = nullptr;
    GOnEntityListChanged on_entity_list_changed = nullptr;
    GOnComponentChanged on_component_changed = nullptr;
    GOnLogMessage on_log_message = nullptr;
};

struct GlobalState {
    bool initialized = false;
    std::mutex init_mutex;
    std::unique_ptr<gryce_engine::ecs::World> world;
    CommandBuffer cmdbuf;
    EntityHandleMap entity_map;
    CallbackTable callbacks;
    void* callback_user_data = nullptr;

    bool play_mode = false;
    bool paused = false;
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

    // --- 输入状态（供 engine.input 查询；由 ECMD_INPUT_* 命令更新）---
    std::unordered_set<int> keys_down;
    int mouse_x = 0;
    int mouse_y = 0;
    bool mouse_button[3] = {};

    bool deferred_entity_list_changed = false;
    bool deferred_selection_changed = false;
    bool deferred_scene_loaded = false;

    // Console: number of MemoryLogSink entries already delivered to the editor
    size_t log_delivered_count = 0;
};

// Defined in core_api.cpp
extern GlobalState g_core_state;

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
