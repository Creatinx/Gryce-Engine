#include "GryceCore/core_api.h"
#include "internal_state.h"

#include "ecs/world.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "scene/scene_serializer.h"
#include "scene/uuid.h"
#include "components/component_factory.h"
#include "resources/project.h"
#include "utils/glog/glog_lib.h"

#include <cstring>
#include <mutex>

using namespace gryce_engine;
using gryce_engine::scene::Scene;
using gryce_engine::scene::Entity;
using gryce_engine::scene::SceneSerializer;
using gryce_engine::ecs::World;
using gryce_engine::resources::Project;

namespace gryce_core {

// Definition of the global state (declared in internal_state.h)
GlobalState g_core_state;

// ============================================================================
// Helpers
// ============================================================================
Entity* EntityResolver::resolve(GEntityHandle h) {
    if (!g_core_state.world || h == 0) return nullptr;
    gryce_engine::scene::UUID* uuid = g_core_state.entity_map.resolve_uuid(h);
    if (!uuid) return nullptr;
    Scene* s = g_core_state.world->scene();
    if (!s) return nullptr;
    return s->find_entity_by_uuid(static_cast<const gryce_engine::scene::UUID&>(*uuid));
}

static void fire_callback_entity_selected(GEntityHandle h) {
    if (g_core_state.callbacks.on_entity_selected) {
        g_core_state.callbacks.on_entity_selected(h, g_core_state.callback_user_data);
    }
}

static void fire_callback_entity_deselected() {
    if (g_core_state.callbacks.on_entity_deselected) {
        g_core_state.callbacks.on_entity_deselected(g_core_state.callback_user_data);
    }
}

static void fire_callback_scene_loaded(const char* path) {
    if (g_core_state.callbacks.on_scene_loaded) {
        g_core_state.callbacks.on_scene_loaded(path, g_core_state.callback_user_data);
    }
}

static void fire_callback_entity_list_changed() {
    if (g_core_state.callbacks.on_entity_list_changed) {
        g_core_state.callbacks.on_entity_list_changed(g_core_state.callback_user_data);
    }
}

static void fire_callback_play_mode_changed() {
    if (g_core_state.callbacks.on_play_mode_changed) {
        g_core_state.callbacks.on_play_mode_changed(g_core_state.play_mode, g_core_state.paused, g_core_state.callback_user_data);
    }
}

// ============================================================================
// Command processing
// ============================================================================
static void cmd_load_scene(const char* path) {
    if (!path || path[0] == '\0') return;
    auto scene = SceneSerializer::load_from_file(path);
    if (!scene) {
        utils::GLog::instance().warn("[Core] Failed to load scene: {}", path);
        return;
    }
    if (g_core_state.world) {
        g_core_state.world->attach_scene(std::move(scene));
    }
    g_core_state.current_scene_path = path;
    g_core_state.entity_map.rebuild(g_core_state.world->scene());
    g_core_state.selected_entity = 0;
    g_core_state.deferred_entity_list_changed = true;
    g_core_state.deferred_scene_loaded = true;
    utils::GLog::instance().info("[Core] Scene loaded: {}", path);
}

static void cmd_create_entity(const char* name, GEntityHandle parent_handle) {
    Scene* s = g_core_state.world ? g_core_state.world->scene() : nullptr;
    if (!s) return;
    Entity* e = s->create_entity(name && name[0] ? name : "Entity");
    if (parent_handle != 0) {
        if (Entity* parent = EntityResolver::resolve(parent_handle)) {
            e->set_parent(parent);
        }
    }
    g_core_state.entity_map.alloc(e->uuid());
    g_core_state.deferred_entity_list_changed = true;
}

static void cmd_destroy_entity(GEntityHandle h) {
    Entity* e = EntityResolver::resolve(h);
    if (!e) return;
    Scene* s = g_core_state.world ? g_core_state.world->scene() : nullptr;
    if (!s) return;
    if (g_core_state.selected_entity == h) {
        g_core_state.selected_entity = 0;
        g_core_state.deferred_selection_changed = true;
    }
    s->destroy_entity(e);
    g_core_state.entity_map.remove(h);
    g_core_state.deferred_entity_list_changed = true;
}

static void cmd_select_entity(GEntityHandle h) {
    GEntityHandle old = g_core_state.selected_entity;
    g_core_state.selected_entity = h;
    g_core_state.deferred_selection_changed = true;
    if (old != 0 && old != h) {
        fire_callback_entity_deselected();
    }
    if (h != 0) {
        fire_callback_entity_selected(h);
    }
}

static void cmd_rename_entity(GEntityHandle h, const char* new_name) {
    Entity* e = EntityResolver::resolve(h);
    if (e && new_name) {
        e->set_name(new_name);
        g_core_state.deferred_entity_list_changed = true;
    }
}

static void cmd_reparent_entity(GEntityHandle h, GEntityHandle new_parent) {
    Entity* e = EntityResolver::resolve(h);
    if (!e) return;
    Entity* parent = (new_parent == 0) ? nullptr : EntityResolver::resolve(new_parent);
    e->set_parent(parent);
    g_core_state.deferred_entity_list_changed = true;
}

static void process_command(const GCommand& cmd) {
    switch (cmd.type) {
        case ECMD_LOAD_SCENE: {
            char path[256] = {};
            std::strncpy(path, reinterpret_cast<const char*>(cmd.payload), 255);
            cmd_load_scene(path);
            break;
        }
        case ECMD_CREATE_ENTITY: {
            struct Payload { char name[128]; GEntityHandle parent; };
            static_assert(sizeof(Payload) <= GCMD_PAYLOAD_SIZE, "payload too big");
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            cmd_create_entity(p->name, p->parent);
            break;
        }
        case ECMD_DESTROY_ENTITY: {
            GEntityHandle h = *reinterpret_cast<const GEntityHandle*>(cmd.payload);
            cmd_destroy_entity(h);
            break;
        }
        case ECMD_SELECT_ENTITY: {
            GEntityHandle h = *reinterpret_cast<const GEntityHandle*>(cmd.payload);
            cmd_select_entity(h);
            break;
        }
        case ECMD_RENAME_ENTITY: {
            struct Payload { GEntityHandle h; char name[128]; };
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            cmd_rename_entity(p->h, p->name);
            break;
        }
        case ECMD_REPARENT_ENTITY: {
            struct Payload { GEntityHandle h; GEntityHandle parent; };
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            cmd_reparent_entity(p->h, p->parent);
            break;
        }
        case ECMD_PLAY_MODE: {
            g_core_state.play_mode = true;
            g_core_state.paused = false;
            if (g_core_state.world) g_core_state.world->set_updates_enabled(true);
            fire_callback_play_mode_changed();
            break;
        }
        case ECMD_STOP_MODE: {
            g_core_state.play_mode = false;
            g_core_state.paused = false;
            if (g_core_state.world) g_core_state.world->set_updates_enabled(false);
            fire_callback_play_mode_changed();
            break;
        }
        case ECMD_PAUSE_MODE: {
            g_core_state.paused = !g_core_state.paused;
            fire_callback_play_mode_changed();
            break;
        }
        default:
            break;
    }
}

} // namespace gryce_core

// ============================================================================
// C API Implementation
// ============================================================================
extern "C" {

int GCore_Init(const GCoreInitDesc* desc) {
    if (!desc || desc->version != sizeof(GCoreInitDesc)) return -1;

    std::lock_guard lock(gryce_core::g_core_state.init_mutex);
    if (gryce_core::g_core_state.initialized) return 0;

    utils::glog_initialize();
    utils::GLog::instance().set_min_level(utils::LogLevel::Info);

    if (desc->project_root && desc->project_root[0]) {
        Project::instance().set_root(desc->project_root);
    }

    components::register_builtin_components();

    gryce_core::g_core_state.world = std::make_unique<World>();
    gryce_core::g_core_state.world->init();

    auto default_scene = std::make_unique<Scene>("Untitled");
    gryce_core::g_core_state.world->attach_scene(std::move(default_scene));
    gryce_core::g_core_state.entity_map.rebuild(gryce_core::g_core_state.world->scene());

    gryce_core::g_core_state.initialized = true;
    return 0;
}

void GCore_Shutdown(void) {
    std::lock_guard lock(gryce_core::g_core_state.init_mutex);
    if (!gryce_core::g_core_state.initialized) return;

    if (gryce_core::g_core_state.world) {
        gryce_core::g_core_state.world->shutdown();
        gryce_core::g_core_state.world.reset();
    }

    gryce_core::g_core_state.entity_map.clear();
    gryce_core::g_core_state.selected_entity = 0;
    gryce_core::g_core_state.current_scene_path.clear();
    gryce_core::g_core_state.play_mode = false;
    gryce_core::g_core_state.paused = false;
    gryce_core::g_core_state.initialized = false;
}

bool GCore_IsInitialized(void) {
    return gryce_core::g_core_state.initialized;
}

void GCore_BeginFrame(float dt) {
    if (!gryce_core::g_core_state.initialized || !gryce_core::g_core_state.world) return;

    gryce_core::g_core_state.cmdbuf.swap();
    int count = 0;
    const GCommand* cmds = gryce_core::g_core_state.cmdbuf.consume(count);
    for (int i = 0; i < count; ++i) {
        gryce_core::process_command(cmds[i]);
    }

    if (gryce_core::g_core_state.play_mode && !gryce_core::g_core_state.paused) {
        gryce_core::g_core_state.world->update(dt);
    }
}

void GCore_EndFrame(void) {
    if (gryce_core::g_core_state.deferred_scene_loaded) {
        gryce_core::g_core_state.deferred_scene_loaded = false;
        gryce_core::fire_callback_scene_loaded(gryce_core::g_core_state.current_scene_path.c_str());
    }
    if (gryce_core::g_core_state.deferred_entity_list_changed) {
        gryce_core::g_core_state.deferred_entity_list_changed = false;
        gryce_core::fire_callback_entity_list_changed();
    }
    if (gryce_core::g_core_state.deferred_selection_changed) {
        gryce_core::g_core_state.deferred_selection_changed = false;
    }
}

int GCore_PushCommand(const GCommand* cmd) {
    if (!cmd || !gryce_core::g_core_state.initialized) return -1;
    bool ok = gryce_core::g_core_state.cmdbuf.push(*cmd);
    return ok ? 0 : -1;
}

int GCore_PushCommands(const GCommand* cmds, int count) {
    if (!cmds || count <= 0 || !gryce_core::g_core_state.initialized) return -1;
    int dropped = 0;
    gryce_core::g_core_state.cmdbuf.push_batch(cmds, count, &dropped);
    return dropped;
}

int GCore_GetCmdQueueCapacity(void) {
    return gryce_core::g_core_state.cmdbuf.capacity_remaining();
}

int GCore_GetDroppedCmdCount(void) {
    return gryce_core::g_core_state.cmdbuf.dropped_since_last_call();
}

bool GCore_IsPlaying(void) { return gryce_core::g_core_state.play_mode; }
bool GCore_IsPaused(void) { return gryce_core::g_core_state.paused; }

void GCore_SetCallback_UserData(void* user_data) {
    gryce_core::g_core_state.callback_user_data = user_data;
}

void GCore_RegisterCallback_OnEntitySelected(GOnEntitySelected cb) {
    gryce_core::g_core_state.callbacks.on_entity_selected = cb;
}
void GCore_RegisterCallback_OnEntityDeselected(GOnEntityDeselected cb) {
    gryce_core::g_core_state.callbacks.on_entity_deselected = cb;
}
void GCore_RegisterCallback_OnSceneLoaded(GOnSceneLoaded cb) {
    gryce_core::g_core_state.callbacks.on_scene_loaded = cb;
}
void GCore_RegisterCallback_OnPlayModeChanged(GOnPlayModeChanged cb) {
    gryce_core::g_core_state.callbacks.on_play_mode_changed = cb;
}
void GCore_RegisterCallback_OnEntityListChanged(GOnEntityListChanged cb) {
    gryce_core::g_core_state.callbacks.on_entity_list_changed = cb;
}
void GCore_RegisterCallback_OnComponentChanged(GOnComponentChanged cb) {
    gryce_core::g_core_state.callbacks.on_component_changed = cb;
}
void GCore_RegisterCallback_OnLogMessage(GOnLogMessage cb) {
    gryce_core::g_core_state.callbacks.on_log_message = cb;
}

int GCore_GetLogMessages(char* out_buf, int buf_size) {
    (void)out_buf; (void)buf_size;
    return 0;
}

void* GCore_GetInternalWorldPtr(void) {
    return gryce_core::g_core_state.world.get();
}

} // extern "C"
