#include "GryceCore/core_api.h"
#include "internal_state.h"

#include "ecs/world.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "scene/scene_serializer.h"
#include "scene/uuid.h"
#include "reflection/reflection.h"
#include "components/component_factory.h"
#include "resources/project.h"
#include "utils/glog/glog_lib.h"

#include <cstddef>
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

// Forward: defined in component_api.cpp
std::string get_component_type_name(gryce_engine::components::Component* comp);

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
// Log forwarding — MemoryLogSink -> editor Console callback
// ============================================================================
static void drain_log_messages() {
    if (!g_core_state.callbacks.on_log_message) return;
    auto* sink = utils::MemoryLogSink::from_glog();
    if (!sink) return;

    auto snap = sink->snapshot();
    const size_t total = snap.size();
    if (total < g_core_state.log_delivered_count) {
        // Sink was cleared or rewound; resync.
        g_core_state.log_delivered_count = 0;
    }
    for (size_t i = g_core_state.log_delivered_count; i < total; ++i) {
        g_core_state.callbacks.on_log_message(
            static_cast<int>(snap[i].level),
            snap[i].message.c_str(),
            g_core_state.callback_user_data);
    }
    g_core_state.log_delivered_count = total;
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
        case ECMD_ADD_COMPONENT: {
            struct Payload { GEntityHandle h; char type_name[128]; };
            static_assert(sizeof(Payload) <= GCMD_PAYLOAD_SIZE, "payload too big");
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->h);
            if (e) {
                auto comp = gryce_engine::components::ComponentFactory::instance().create(p->type_name);
                if (comp) {
                    e->add_component(std::move(comp));
                    g_core_state.deferred_entity_list_changed = true;
                }
            }
            break;
        }
        case ECMD_REMOVE_COMPONENT: {
            struct Payload { GEntityHandle h; uint64_t type_hash; };
            static_assert(sizeof(Payload) <= GCMD_PAYLOAD_SIZE, "payload too big");
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->h);
            if (e) {
                for (const auto& comp : e->components()) {
                    std::string type_name = get_component_type_name(comp.get());
                    if (std::hash<std::string>{}(type_name) == p->type_hash) {
                        e->remove_component(comp.get());
                        g_core_state.deferred_entity_list_changed = true;
                        break;
                    }
                }
            }
            break;
        }
        case ECMD_SET_PROPERTY: {
            struct Payload { GEntityHandle h; uint64_t type_hash; char prop_name[64]; uint8_t value[128]; };
            static_assert(sizeof(Payload) <= GCMD_PAYLOAD_SIZE, "payload too big");
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->h);
            if (e) {
                for (const auto& comp : e->components()) {
                    std::string type_name = get_component_type_name(comp.get());
                    if (std::hash<std::string>{}(type_name) == p->type_hash) {
                        auto fields = gryce_engine::reflection::Registry::instance().all_fields(type_name);
                        for (const auto* f : fields) {
                            if (f->name == p->prop_name && f->write && !f->read_only) {
                                f->write(comp.get(), p->value);
                                e->mark_dirty();
                                break;
                            }
                        }
                        break;
                    }
                }
            }
            break;
        }
        case ECMD_SET_TRANSFORM: {
            struct Payload { GEntityHandle h; GVec3 pos; GQuat rot; GVec3 scale; };
            static_assert(sizeof(Payload) <= GCMD_PAYLOAD_SIZE, "payload too big");
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->h);
            if (e && e->transform()) {
                auto* t = e->transform();
                t->position.x = p->pos.x; t->position.y = p->pos.y; t->position.z = p->pos.z;
                t->rotation.x = p->rot.x; t->rotation.y = p->rot.y; t->rotation.z = p->rot.z; t->rotation.w = p->rot.w;
                t->scale.x = p->scale.x; t->scale.y = p->scale.y; t->scale.z = p->scale.z;
                e->mark_dirty();
            }
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

    // Install the memory-backed sink so the editor console can receive engine
    // logs (set_logger wraps the given logger in an AsyncLogger).
    utils::GLog::instance().set_logger(
        std::make_unique<utils::MemoryLogSink>(std::make_unique<utils::ConsoleLogger>()));

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
    // Forward new engine log entries to the editor console before firing
    // deferred callbacks, so UI updates happen on the same frame.
    gryce_core::drain_log_messages();

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
    if (!out_buf || buf_size <= 0) return -1;
    auto* sink = utils::MemoryLogSink::from_glog();
    if (!sink) {
        out_buf[0] = '\0';
        return 0;
    }
    auto snap = sink->snapshot();
    std::string joined;
    joined.reserve(static_cast<size_t>(buf_size));
    for (const auto& entry : snap) {
        if (!joined.empty()) joined += '\n';
        joined += entry.message;
        if (joined.size() + 8 >= static_cast<size_t>(buf_size)) break;
    }
    if (joined.size() >= static_cast<size_t>(buf_size)) {
        joined.resize(static_cast<size_t>(buf_size) - 1);
    }
    std::memcpy(out_buf, joined.c_str(), joined.size() + 1);
    return static_cast<int>(joined.size());
}

void* GCore_GetInternalWorldPtr(void) {
    return gryce_core::g_core_state.world.get();
}

} // extern "C"
