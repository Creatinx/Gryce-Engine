#include "GryceCore/core_api.h"
#include "GryceCore/scene_api.h"
#include "GryceCore/api_guard.h"
#include "internal_state.h"

#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "ecs/systems/animator_system.h"
#include "ecs/systems/fracture_system.h"
#include "ecs/systems/subviewport_system.h"
#include "ecs/systems/script_system.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "scene/scene_serializer.h"
#include "scene/uuid.h"
#include "reflection/reflection.h"
#include "components/script_component.h"
#include "components/component_factory.h"
#include "resources/project.h"
#include "resources/gpack_bundle.h"
#include "script/lua_runtime.h"
#include "utils/glog/glog_lib.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <chrono>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>

using namespace gryce_engine;
using gryce_engine::scene::Scene;
using gryce_engine::scene::Entity;
using gryce_engine::scene::SceneSerializer;
using gryce_engine::ecs::World;
using gryce_engine::resources::Project;

namespace {

// Mount every .gpack/.gpkg bundle found in the project root so res:/
// resources can be read from packaged archives (GryceGC output).
void mount_project_bundles(const std::string& root) {
    if (root.empty()) return;
    // GryceGC puts the archives under <root>/assets/; older layouts placed
    // them directly in the project root, so scan both.
    const std::vector<std::string> dirs = {root, root + "/assets"};
    for (const std::string& dir : dirs) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::string ext = entry.path().extension().string();
            if (ext == ".gpack" || ext == ".gpkg") {
                const int id = assets::AssetManager::instance().mount_bundle(entry.path().string());
                GLOG_INFO("GCore: mounted resource bundle '{}' (id={})",
                          entry.path().string(), id);
            }
        }
    }
}

// Read the project's project_settings.json and apply the fields the core
// owns (currently the main scene; render settings are owned by the editor).
void load_project_settings(const std::string& root) {
    if (root.empty()) return;
    try {
        std::ifstream in(root + "/project_settings.json");
        if (!in) return;
        nlohmann::json j;
        in >> j;
        if (j.contains("main_scene") && j["main_scene"].is_string()) {
            Project::instance().set_main_scene(j["main_scene"].get<std::string>());
            GLOG_INFO("GCore: main scene set to '{}'",
                      Project::instance().main_scene());
        }
    } catch (const std::exception& e) {
        GLOG_WARN("GCore: failed to read project_settings.json ({})", e.what());
    }
}

} // namespace

namespace gryce_core {

// 有界 strlen（不依赖平台是否提供 std::strnlen）
static size_t bounded_strlen(const char* s, size_t max_len) {
    size_t n = 0;
    while (n < max_len && s[n] != '\0') ++n;
    return n;
}

// Definition of the global state (declared in internal_state.h)
GlobalState g_core_state;

// Shared recursive mutex backing the GRYCE_API_GUARD() macro. A free function
// (instead of a GlobalState member) so the other Gryce DLLs (Platform /
// Renderer / Physics) can lock the same instance.
std::recursive_mutex& api_mutex() {
    static std::recursive_mutex instance;
    return instance;
}

// ============================================================================
// Helpers
// ============================================================================
Entity* EntityResolver::resolve(GEntityHandle h) {
    if (!g_core_state.world || h == 0) return nullptr;
    Scene* s = g_core_state.world->scene();
    if (!s) return nullptr;
    // Copy the UUID under the map lock: the map can rehash on a concurrent
    // alloc/remove, invalidating a pointer returned by resolve_uuid().
    gryce_engine::scene::UUID uuid;
    if (!g_core_state.entity_map.resolve_uuid_copy(h, uuid)) return nullptr;
    return s->find_entity_by_uuid(uuid);
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
            snap[i].source_file.c_str(),
            snap[i].source_line,
            g_core_state.callback_user_data);
    }
    g_core_state.log_delivered_count = total;
}

std::string reflection_lookup_name(const std::string& full_name) {
    GRYCE_API_GUARD();
    // 反射注册表使用短名；取最后一个 "::" 之后的段。
    // 2D 组件位于 d2::basic_rect / d2::sprite / d2::light 等嵌套命名空间，
    // 仅剥 "gryce_engine::components::" 前缀会留下 "d2::xxx::Type" 查不到。
    const size_t pos = full_name.rfind("::");
    if (pos != std::string::npos) return full_name.substr(pos + 2);
    return full_name;
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
    Entity* e = nullptr;
    if (parent_handle != 0) {
        if (Entity* parent = EntityResolver::resolve(parent_handle)) {
            // 直接挂在目标父级下（add_child 接管所有权），
            // 避免 create_entity + set_parent 的销毁性 remove_child 悬垂。
            if (parent != s->root()) {
                e = parent->add_child(std::make_unique<Entity>(name && name[0] ? name : "Entity"));
            }
        }
    }
    if (!e) e = s->create_entity(name && name[0] ? name : "Entity");
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
    Scene* s = g_core_state.world ? g_core_state.world->scene() : nullptr;
    if (!s) return;
    Entity* parent = (new_parent == 0) ? nullptr : EntityResolver::resolve(new_parent);
    if (parent == e || e->parent() == parent) return; // 自挂 / 原地重挂
    if (!e->parent()) return;
    // 安全重挂：detach_child 转移所有权后再 add_child / add_root_entity，
    // 不能走 set_parent（remove_child 会销毁被挂实体）。
    auto owned = e->parent()->detach_child(e);
    if (!owned) return;
    Entity* raw = owned.get();
    if (parent && parent != s->root()) {
        parent->add_child(std::move(owned));
    } else {
        s->add_root_entity(std::move(owned));
    }
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
            // 进入 Play 前保存场景快照，Stop 时恢复到播放前状态
            //（播放期间的物理/动画/编辑改动全部丢弃，类 Unity 行为）。
            if (!g_core_state.play_mode && g_core_state.world && g_core_state.world->scene()) {
                g_core_state.play_snapshot_json =
                    SceneSerializer::serialize(*g_core_state.world->scene()).dump();
            }
            g_core_state.play_mode = true;
            g_core_state.paused = false;
            if (g_core_state.world) g_core_state.world->set_updates_enabled(true);
            fire_callback_play_mode_changed();
            break;
        }
        case ECMD_STOP_MODE: {
            const bool was_playing = g_core_state.play_mode;
            g_core_state.play_mode = false;
            g_core_state.paused = false;
            if (g_core_state.world) {
                if (was_playing && !g_core_state.play_snapshot_json.empty()) {
                    try {
                        auto restored = SceneSerializer::deserialize(
                            nlohmann::json::parse(g_core_state.play_snapshot_json));
                        if (restored) {
                            // attach_scene 会 shutdown 旧场景（物理/动画系统清理）并
                            // 重新 init，Play 期间产生的刚体/动画状态随之复位。
                            g_core_state.world->attach_scene(std::move(restored));
                            g_core_state.entity_map.rebuild(g_core_state.world->scene());
                            g_core_state.selected_entity = 0;
                            g_core_state.deferred_entity_list_changed = true;
                            utils::GLog::instance().info(
                                "[Core] Play Mode stopped: scene restored from snapshot");
                        }
                    } catch (const std::exception& ex) {
                        utils::GLog::instance().warn(
                            "[Core] Play Mode restore failed: {}", ex.what());
                    }
                }
                g_core_state.world->set_updates_enabled(false);
            }
            g_core_state.play_snapshot_json.clear();
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
                        auto fields = gryce_engine::reflection::Registry::instance().all_fields(
                            gryce_core::reflection_lookup_name(type_name));
                        for (const auto* f : fields) {
                            if (f->name == p->prop_name && f->write && !f->read_only) {
                                if (f->type == gryce_engine::reflection::FieldType::String) {
                                    const char* cstr = reinterpret_cast<const char*>(p->value);
                                    const size_t len = bounded_strlen(cstr, sizeof(p->value));
                                    std::string tmp(cstr, len);
                                    f->write(comp.get(), &tmp);
                                } else {
                                    f->write(comp.get(), p->value);
                                }
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
        case ECMD_SET_SCRIPT: {
            struct Payload { GEntityHandle h; char path[128]; };
            static_assert(sizeof(Payload) <= GCMD_PAYLOAD_SIZE, "payload too big");
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->h);
            if (e) {
                auto* comp = e->get_component<components::ScriptComponent>();
                if (comp) {
                    comp->script_path = p->path;
                    comp->script_loaded = false;
                    comp->start_called = false;
                    comp->reported_error = false;
                    comp->last_error.clear();
                    e->mark_dirty();
                    g_core_state.deferred_entity_list_changed = true;
                }
            }
            break;
        }
        case ECMD_RELOAD_SCRIPTS: {
            if (g_core_state.world) {
                if (auto* sys = g_core_state.world->get_system<ecs::ScriptSystem>()) {
                    sys->reload_all();
                }
            }
            break;
        }
        case ECMD_INPUT_KEY: {
            struct Payload { int key; uint8_t down; };
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            if (p->down) {
                g_core_state.keys_down.insert(p->key);
                g_core_state.input_events.push_back({INPUT_EVENT_KEY_DOWN, p->key, 0, 0});
            } else {
                g_core_state.keys_down.erase(p->key);
                g_core_state.input_events.push_back({INPUT_EVENT_KEY_UP, p->key, 0, 0});
            }
            break;
        }
        case ECMD_INPUT_MOUSE_MOVE: {
            struct Payload { int x; int y; };
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            g_core_state.mouse_x = p->x;
            g_core_state.mouse_y = p->y;
            g_core_state.input_events.push_back({INPUT_EVENT_MOUSE_MOVE, p->x, p->y, 0});
            break;
        }
        case ECMD_INPUT_MOUSE_BUTTON: {
            struct Payload { int button; uint8_t down; int x; int y; };
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            if (p->button >= 0 && p->button < 3) {
                g_core_state.mouse_button[p->button] = p->down != 0;
                g_core_state.input_events.push_back({
                    p->down ? INPUT_EVENT_MOUSE_DOWN : INPUT_EVENT_MOUSE_UP,
                    p->button, p->x, p->y});
            }
            g_core_state.mouse_x = p->x;
            g_core_state.mouse_y = p->y;
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
    GRYCE_API_GUARD();
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

    // Mount every .gpack/.gpkg bundle in the project root so res:/ resources
    // (scenes, shaders, scripts, assets) resolve from packaged archives
    // produced by GryceGC. Files on disk still take precedence at load time.
    mount_project_bundles(Project::instance().root());
    load_project_settings(Project::instance().root());

    components::register_builtin_components();

    gryce_core::g_core_state.world = std::make_unique<World>();
    auto default_scene = std::make_unique<Scene>("Untitled");
    gryce_core::g_core_state.world->attach_scene(std::move(default_scene));

    // 编辑器/运行时统一注册核心系统：动画驱动、碎裂。
    // （物理系统位于 GrycePhysics.dll，由 GPhysics_AttachSystems 注册。）
    gryce_core::g_core_state.world->register_system(std::make_unique<ecs::AnimatorSystem>());
    gryce_core::g_core_state.world->register_system(std::make_unique<ecs::FractureSystem>());
    gryce_core::g_core_state.world->register_system(std::make_unique<ecs::SubViewportSystem>());
    gryce_core::g_core_state.world->register_system(std::make_unique<ecs::ScriptSystem>());
    gryce_core::g_core_state.world->init();

    gryce_core::g_core_state.entity_map.rebuild(gryce_core::g_core_state.world->scene());

    // GryceSRT: bring up the Lua runtime together with the core.
    script::LuaRuntime::instance().init();

    gryce_core::g_core_state.initialized = true;

    // Game entry (GryceGame template): enter the project's main scene right
    // after startup. The editor leaves the flag off and manages scenes itself.
    if (gryce_core::g_core_state.auto_load_main_scene) {
        const std::string main_scene = Project::instance().main_scene();
        if (GScene_Load(main_scene.c_str()) != 0) {
            GLOG_ERROR("GCore_Init: failed to load main scene '{}'", main_scene);
        } else {
            GLOG_INFO("GCore: main scene loaded '{}'", main_scene);
        }
    }
    return 0;
}

void GCore_SetAutoLoadMainScene(bool enable) {
    GRYCE_API_GUARD();
    gryce_core::g_core_state.auto_load_main_scene = enable;
}

void GCore_Shutdown(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(gryce_core::g_core_state.init_mutex);
    if (!gryce_core::g_core_state.initialized) return;

    script::LuaRuntime::instance().shutdown();

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
    GRYCE_API_GUARD();
    return gryce_core::g_core_state.initialized;
}

void GCore_BeginFrame(float dt) {
    GRYCE_API_GUARD();
    if (!gryce_core::g_core_state.initialized || !gryce_core::g_core_state.world) return;

    gryce_core::g_core_state.cmdbuf.swap();
    int count = 0;
    const GCommand* cmds = gryce_core::g_core_state.cmdbuf.consume(count);
    // Process commands under a small time budget: a burst of editor commands
    // (gizmo drags at high mouse rate, mass edits) applies within one or two
    // frames, while a heavy command (scene load) cannot block the tick for
    // long. Unprocessed commands are re-queued for the next frame.
    constexpr int k_max_commands_per_frame = 512;
    constexpr auto k_command_budget = std::chrono::microseconds(4000);
    const auto start = std::chrono::steady_clock::now();
    int processed = 0;
    for (; processed < count && processed < k_max_commands_per_frame; ++processed) {
        gryce_core::process_command(cmds[processed]);
        if (std::chrono::steady_clock::now() - start >= k_command_budget) {
            // 本命令已经处理完成；break 不会执行 for 的自增，
            // 手动 +1 避免它被下面的 re-queue 再次入队。
            ++processed;
            break;
        }
    }
    for (int i = processed; i < count; ++i) {
        gryce_core::g_core_state.cmdbuf.push(cmds[i]);
    }

    if (gryce_core::g_core_state.play_mode) {
        if (!gryce_core::g_core_state.paused) {
            gryce_core::g_core_state.world->update(dt);
        } else {
            // 暂停时仅驱动脚本系统，让 pause_mode=true 的脚本继续 on_update
            // （其余系统保持冻结，类比 Godot 按 process_mode 选择性暂停）。
            if (auto* sys = gryce_core::g_core_state.world->get_system<ecs::ScriptSystem>()) {
                sys->on_update(*gryce_core::g_core_state.world->scene(), 0.0f);
            }
        }
    }
}

void GCore_EndFrame(void) {
    GRYCE_API_GUARD();
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
    GRYCE_API_GUARD();
    if (!cmd || !gryce_core::g_core_state.initialized) return -1;
    bool ok = gryce_core::g_core_state.cmdbuf.push(*cmd);
    return ok ? 0 : -1;
}

int GCore_PushCommands(const GCommand* cmds, int count) {
    GRYCE_API_GUARD();
    if (!cmds || count <= 0 || !gryce_core::g_core_state.initialized) return -1;
    int dropped = 0;
    gryce_core::g_core_state.cmdbuf.push_batch(cmds, count, &dropped);
    return dropped;
}

int GCore_GetCmdQueueCapacity(void) {
    GRYCE_API_GUARD();
    return gryce_core::g_core_state.cmdbuf.capacity_remaining();
}

int GCore_GetDroppedCmdCount(void) {
    GRYCE_API_GUARD();
    return gryce_core::g_core_state.cmdbuf.dropped_since_last_call();
}

bool GCore_IsPlaying(void) { return gryce_core::g_core_state.play_mode; }
bool GCore_IsPaused(void) { return gryce_core::g_core_state.paused; }

void GCore_SetCallback_UserData(void* user_data) {
    GRYCE_API_GUARD();
    gryce_core::g_core_state.callback_user_data = user_data;
}

void GCore_RegisterCallback_OnEntitySelected(GOnEntitySelected cb) {
    GRYCE_API_GUARD();
    gryce_core::g_core_state.callbacks.on_entity_selected = cb;
}
void GCore_RegisterCallback_OnEntityDeselected(GOnEntityDeselected cb) {
    GRYCE_API_GUARD();
    gryce_core::g_core_state.callbacks.on_entity_deselected = cb;
}
void GCore_RegisterCallback_OnSceneLoaded(GOnSceneLoaded cb) {
    GRYCE_API_GUARD();
    gryce_core::g_core_state.callbacks.on_scene_loaded = cb;
}
void GCore_RegisterCallback_OnPlayModeChanged(GOnPlayModeChanged cb) {
    GRYCE_API_GUARD();
    gryce_core::g_core_state.callbacks.on_play_mode_changed = cb;
}
void GCore_RegisterCallback_OnEntityListChanged(GOnEntityListChanged cb) {
    GRYCE_API_GUARD();
    gryce_core::g_core_state.callbacks.on_entity_list_changed = cb;
}
void GCore_RegisterCallback_OnComponentChanged(GOnComponentChanged cb) {
    GRYCE_API_GUARD();
    gryce_core::g_core_state.callbacks.on_component_changed = cb;
}
void GCore_RegisterCallback_OnLogMessage(GOnLogMessage cb) {
    GRYCE_API_GUARD();
    gryce_core::g_core_state.callbacks.on_log_message = cb;
}

int GCore_GetLogMessages(char* out_buf, int buf_size) {
    GRYCE_API_GUARD();
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

// ============================================================================
// GPack packaging API (used by GryceGC to assemble .gpkg archives)
// ============================================================================
GPackHandle GCore_PackCreate(void) {
    try {
        auto* writer = new resources::GPackWriter();
        return reinterpret_cast<GPackHandle>(writer);
    } catch (const std::exception& e) {
        GLOG_ERROR("GCore_PackCreate: failed ({})", e.what());
        return nullptr;
    } catch (...) {
        GLOG_ERROR("GCore_PackCreate: unknown failure");
        return nullptr;
    }
}

int GCore_PackAddFile(GPackHandle handle, const char* internal_path, const char* source_path) {
    if (!handle || !internal_path || !source_path) return -1;
    try {
        auto* writer = reinterpret_cast<resources::GPackWriter*>(handle);
        return writer->add_file(internal_path, source_path) ? 0 : -1;
    } catch (const std::exception& e) {
        GLOG_ERROR("GCore_PackAddFile('{}'): failed ({})", internal_path, e.what());
        return -1;
    } catch (...) {
        GLOG_ERROR("GCore_PackAddFile('{}'): unknown failure", internal_path);
        return -1;
    }
}

int GCore_PackWrite(GPackHandle handle, const char* output_path) {
    if (!handle || !output_path) return -1;
    try {
        auto* writer = reinterpret_cast<resources::GPackWriter*>(handle);
        return writer->write(output_path) ? 0 : -1;
    } catch (const std::exception& e) {
        GLOG_ERROR("GCore_PackWrite('{}'): failed ({})", output_path, e.what());
        return -1;
    } catch (...) {
        GLOG_ERROR("GCore_PackWrite('{}'): unknown failure", output_path);
        return -1;
    }
}

void GCore_PackDestroy(GPackHandle handle) {
    if (!handle) return;
    delete reinterpret_cast<resources::GPackWriter*>(handle);
}

void* GCore_GetInternalWorldPtr(void) {
    GRYCE_API_GUARD();
    return gryce_core::g_core_state.world.get();
}

} // extern "C"
