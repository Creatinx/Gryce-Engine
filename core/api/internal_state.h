#pragma once

#include "GryceCore/types.h"
#include "command_buffer.h"
#include "entity_handle_map.h"

#include <memory>
#include <string>
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
    GEntityHandle selected_entity = 0;
    std::string current_scene_path;

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
