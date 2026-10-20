#include "api/core_api.h"
#include "api/scene_api.h"
#include "api/edit_api.h"
#include "api/api_guard.h"
#include "runtime/engine_context.h"

#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "ecs/systems/animator_system.h"
#include "ecs/systems/subviewport_system.h"
#include "ecs/systems/hierarchy_system.h"
#include "ecs/systems/physics_system_2d.h"
#include "ecs/systems/physics_system_3d.h"
#include "ecs/systems/audio_system.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "scene/scene_serializer.h"
#include "scene/uuid.h"
#include "reflection/reflection.h"
#include "components/component_factory.h"
#include "components/mesh_renderer.h"
#include "script/script_component.h"
#include "resources/project.h"
#include "resources/pak_bundle.h"
#include "resources/gpack_bundle.h"
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

// 将 16 进制字符串解码为 32 字节密钥，供 PakReader 解密 .gpkg（GPAK v4）。
void load_pak_crypto_key(const std::string& root) {
    if (root.empty()) return;
    try {
        // 解密密钥现在与项目配置合并于根目录唯一的 project.data（JSON）中。
        std::ifstream in(root + "/project.data");
        if (!in) return;
        nlohmann::json j;
        in >> j;
        if (!j.contains("enc_key_hex") || !j["enc_key_hex"].is_string()) return;
        const std::string hex = j["enc_key_hex"].get<std::string>();
        if (hex.size() != 64) return;
        std::string key;
        key.reserve(32);
        auto hexval = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < hex.size(); i += 2) {
            const int hi = hexval(hex[i]);
            const int lo = hexval(hex[i + 1]);
            if (hi < 0 || lo < 0) return;
            key.push_back(static_cast<char>((hi << 4) | lo));
        }
        resources::set_pak_crypto_key(key);
        GLOG_INFO("GCore: loaded .gpkg decryption key from project.data");
    } catch (const std::exception& e) {
        GLOG_WARN("GCore: failed to load pak decryption key ({})", e.what());
    }
}

// Mount every .gpack/.gpkg bundle found in the project root so res:/
// resources can be read from packaged archives (GryceGC output).
void mount_project_bundles(const std::string& root) {
    if (root.empty()) return;
    load_pak_crypto_key(root);
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

// Read the project's single config file (project.data) and apply the fields
// the core owns (currently the main scene; render settings are owned by the
// editor). project.data is the only state file in the game root, containing
// the merged project manifest + runtime settings + package metadata.
void load_project_settings(const std::string& root) {
    if (root.empty()) return;
    try {
        std::ifstream in(root + "/project.data");
        if (!in) return;
        nlohmann::json j;
        in >> j;
        if (j.contains("main_scene") && j["main_scene"].is_string()) {
            Project::instance().set_main_scene(j["main_scene"].get<std::string>());
            GLOG_INFO("GCore: main scene set to '{}'",
                      Project::instance().main_scene());
        }
    } catch (const std::exception& e) {
        GLOG_WARN("GCore: failed to read project.data ({})", e.what());
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

// Definition of the global state (declared in runtime/engine_context.h).
// EngineContext 含 World / Scene 的 unique_ptr，析构需要完整类型，故定义
// 留在本 TU（api/ 层，本就包含这些类型）；输入状态已拆到
// runtime/engine_context.cpp 的 g_input_state，经 input 引用成员接入。
EngineContext g_core_state;

// Shared recursive mutex backing the GRYCE_API_GUARD() macro. A free function
// (instead of an EngineContext member) so the other Gryce DLLs (Platform /
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
    // 换文档：上一份场景的历史不该作用到新场景上（快照里的 UUID 对不上）。
    GCore_ClearUndoHistory();
    g_core_state.deferred_entity_list_changed = true;
    g_core_state.deferred_scene_loaded = true;
    utils::GLog::instance().info("[Core] Scene loaded: {}", path);
}

static void cmd_select_entity(GEntityHandle h);   // 定义在下方，新建实体要复用

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
    const GEntityHandle new_handle = g_core_state.entity_map.alloc(e->uuid());
    g_core_state.deferred_entity_list_changed = true;
    // 新建即选中：编辑器的通用行为（"建完就能直接改属性"），也让 GEdit_CreateEntity
    // 的调用方能在命令消费后用 GEntity_GetSelected 拿到新实体句柄。
    if (new_handle != 0) cmd_select_entity(new_handle);
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

// node 本身是否等于 ancestor，或挂在 ancestor 的子树里。
// 重挂前必须查：`Entity::add_child` 不做防环，把父节点挂到自己的子孙上会
// 构造出 A -> B -> A 的**所有权环**（两边的 children_ 互相持有），之后任何
// 一次 foreach / 序列化都会无限递归，析构时又是悬空指针。编辑器把节点拖到
// 自己的子节点上就会触发，全靠这一道检查兜住。
static bool is_in_subtree_of(const Entity* node, const Entity* ancestor) {
    for (const Entity* p = node; p; p = p->parent()) {
        if (p == ancestor) return true;
    }
    return false;
}

static void cmd_reparent_entity(GEntityHandle h, GEntityHandle new_parent) {
    Entity* e = EntityResolver::resolve(h);
    if (!e) return;
    Scene* s = g_core_state.world ? g_core_state.world->scene() : nullptr;
    if (!s) return;
    Entity* parent = (new_parent == 0) ? nullptr : EntityResolver::resolve(new_parent);
    if (parent == e || e->parent() == parent) return; // 自挂 / 原地重挂
    // 成环：目标是自己的子孙。静默拒绝而不是"尽力重挂"——挂上去之后场景
    // 立刻不可遍历，比什么都不做糟得多。
    if (parent && is_in_subtree_of(parent, e)) return;
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

// ============================================================================
// Undo / Redo（快照式）
//
// 变更类命令执行**前**抓一份整场 JSON。撤销 = 反序列化上一份并替换场景。
// 用整场快照而不是增量：序列化路径已被存档复用、正确性有测试兜底，且恢复后
// 实体句柄按 UUID 保留（EntityHandleMap::rebuild），编辑器的选择与面板目标
// 不会因为一次撤销全部失效。
// ============================================================================
namespace {

struct UndoEntry {
    std::string label;
    std::string json;
};

struct UndoState {
    std::vector<UndoEntry> undo;
    std::vector<UndoEntry> redo;
    int limit = 64;
    bool group_open = false;
    std::string group_base;
    std::string group_label;
};

UndoState& undo_state() {
    static UndoState st;
    return st;
}

std::string capture_scene_json() {
    Scene* s = gryce_core::g_core_state.world ? gryce_core::g_core_state.world->scene() : nullptr;
    if (!s) return std::string();
    try {
        return SceneSerializer::serialize(*s).dump();
    } catch (const std::exception& e) {
        utils::GLog::instance().warn("[Core] undo snapshot failed: {}", e.what());
        return std::string();
    }
}

// 记录命令执行**前**的状态。分组打开期间由 GCore_BeginEdit 统一抓取。
void undo_capture(const char* label) {
    UndoState& st = undo_state();
    if (st.limit <= 0 || st.group_open) return;
    if (gryce_core::g_core_state.play_mode) return;
    std::string json = capture_scene_json();
    if (json.empty()) return;
    st.undo.push_back({label ? label : "edit", std::move(json)});
    while (static_cast<int>(st.undo.size()) > st.limit) st.undo.erase(st.undo.begin());
    st.redo.clear();
}

bool restore_scene_json(const std::string& json) {
    auto& st = gryce_core::g_core_state;
    if (json.empty() || !st.world) return false;
    try {
        auto scene = SceneSerializer::deserialize(nlohmann::json::parse(json));
        if (!scene) return false;
        st.world->attach_scene(std::move(scene));
        st.entity_map.rebuild(st.world->scene());
        // 回到"删除前"时同一句柄依然有效（UUID 保留）；回到"创建前"时该实体
        // 已不存在，必须清掉选择，否则属性面板会拿着悬空句柄。
        if (st.selected_entity != 0 && !EntityResolver::resolve(st.selected_entity)) {
            st.selected_entity = 0;
            st.deferred_selection_changed = true;
            fire_callback_entity_deselected();
        }
        st.deferred_entity_list_changed = true;
        return true;
    } catch (const std::exception& e) {
        utils::GLog::instance().warn("[Core] undo restore failed: {}", e.what());
        return false;
    }
}

// 命令是否属于"改变场景内容"（决定要不要抓快照）。
bool is_mutating_command(GCommandType t) {
    switch (t) {
        case ECMD_CREATE_ENTITY:
        case ECMD_DESTROY_ENTITY:
        case ECMD_RENAME_ENTITY:
        case ECMD_REPARENT_ENTITY:
        case ECMD_SET_TRANSFORM:
        case ECMD_TRANSLATE_ENTITY:
        case ECMD_SET_PROPERTY:
        case ECMD_ADD_COMPONENT:
        case ECMD_REMOVE_COMPONENT:
        case ECMD_SET_MATERIAL:
        case ECMD_IMPORT_ASSET:
            return true;
        default:
            return false;
    }
}

} // namespace

static void process_command(const GCommand& cmd) {
    // 撤销快照要在命令生效**之前**抓，所以放在 switch 之前统一处理；
    // 分组打开时 undo_capture 自己跳过（由 BeginEdit/EndEdit 负责）。
    if (is_mutating_command(cmd.type)) undo_capture("edit");
    switch (cmd.type) {
        case ECMD_LOAD_SCENE: {
            // path 用 ={} 零初始化，strncpy 最多写 [0,255]，path[255] 恒为 '\0'；
            // payload 数组本身 256 字节，读取不越界 —— 字符串必然正确终止。
            char path[256] = {};
            std::strncpy(path, reinterpret_cast<const char*>(cmd.payload), 255);
            cmd_load_scene(path);
            break;
        }
        case ECMD_CREATE_ENTITY: {
            const auto* p = reinterpret_cast<const GCmdCreateEntity*>(cmd.payload);
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
            const auto* p = reinterpret_cast<const GCmdRenameEntity*>(cmd.payload);
            cmd_rename_entity(p->handle, p->name);
            break;
        }
        case ECMD_REPARENT_ENTITY: {
            const auto* p = reinterpret_cast<const GCmdReparentEntity*>(cmd.payload);
            cmd_reparent_entity(p->handle, p->parent);
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
            const auto* p = reinterpret_cast<const GCmdAddComponent*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->handle);
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
            const auto* p = reinterpret_cast<const GCmdRemoveComponent*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->handle);
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
            const auto* p = reinterpret_cast<const GCmdSetProperty*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->handle);
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
            const auto* p = reinterpret_cast<const GCmdSetTransform*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->handle);
            if (e && e->transform()) {
                auto* t = e->transform();
                t->position.x = p->position.x;
                t->position.y = p->position.y;
                t->position.z = p->position.z;
                t->rotation.x = p->rotation.x;
                t->rotation.y = p->rotation.y;
                t->rotation.z = p->rotation.z;
                t->rotation.w = p->rotation.w;
                t->scale.x = p->scale.x;
                t->scale.y = p->scale.y;
                t->scale.z = p->scale.z;
                e->mark_dirty();
            }
            break;
        }
        case ECMD_TRANSLATE_ENTITY: {
            const auto* p = reinterpret_cast<const GCmdTranslateEntity*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->handle);
            if (e && e->transform()) {
                // 在执行时才取当前位置：入队时取会让同一帧的多条增量互相覆盖。
                auto* t = e->transform();
                t->position.x += p->delta.x;
                t->position.y += p->delta.y;
                t->position.z += p->delta.z;
                e->mark_dirty();
            }
            break;
        }
        case ECMD_INPUT_KEY: {
            struct Payload { int key; uint8_t down; };
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            if (p->down) {
                g_core_state.input.keys_down.insert(p->key);
                g_core_state.input.input_events.push_back({INPUT_EVENT_KEY_DOWN, p->key, 0, 0});
            } else {
                g_core_state.input.keys_down.erase(p->key);
                g_core_state.input.input_events.push_back({INPUT_EVENT_KEY_UP, p->key, 0, 0});
            }
            break;
        }
        case ECMD_INPUT_MOUSE_MOVE: {
            struct Payload { float x; float y; };
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            // 累加本帧鼠标增量（相对上一次 move 事件的绝对位置）
            if (g_core_state.input.mouse_snap_x < 0.0f) {
                g_core_state.input.mouse_snap_x = p->x;
                g_core_state.input.mouse_snap_y = p->y;
            } else {
                g_core_state.input.mouse_delta_x += p->x - g_core_state.input.mouse_snap_x;
                g_core_state.input.mouse_delta_y += p->y - g_core_state.input.mouse_snap_y;
                g_core_state.input.mouse_snap_x = p->x;
                g_core_state.input.mouse_snap_y = p->y;
            }
            g_core_state.input.mouse_x = static_cast<int>(p->x);
            g_core_state.input.mouse_y = static_cast<int>(p->y);
            g_core_state.input.input_events.push_back({INPUT_EVENT_MOUSE_MOVE, static_cast<int>(p->x), static_cast<int>(p->y), 0});
            break;
        }
        case ECMD_INPUT_MOUSE_BUTTON: {
            struct Payload { int button; uint8_t down; int x; int y; };
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            if (p->button >= 0 && p->button < 3) {
                g_core_state.input.mouse_button[p->button] = p->down != 0;
                g_core_state.input.input_events.push_back({
                    p->down ? INPUT_EVENT_MOUSE_DOWN : INPUT_EVENT_MOUSE_UP,
                    p->button, p->x, p->y});
            }
            g_core_state.input.mouse_x = p->x;
            g_core_state.input.mouse_y = p->y;
            break;
        }
        case ECMD_INPUT_MOUSE_RESET: {
            // 编辑器切换场景/游戏视图或重新锁定指针后，旧的绝对鼠标基线
            // 已失效：清掉 snap，令下一次 ECMD_INPUT_MOUSE_MOVE 重新基线，
            // 避免第一次移动产生巨大的假 delta（视角猛甩）。
            g_core_state.input.mouse_snap_x = -1.0f;
            g_core_state.input.mouse_snap_y = -1.0f;
            g_core_state.input.mouse_delta_x = 0;
            g_core_state.input.mouse_delta_y = 0;
            break;
        }
        case ECMD_INPUT_MOUSE_DELTA: {
            struct Payload { float dx; float dy; };
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            // GLFW 锁定模式：增量已由 InputManager 相对窗口中心计算并每帧回中，
            // 绝对位置恒为中心，不能走位置差；直接累加（BeginFrame 每帧清零）。
            g_core_state.input.mouse_delta_x += p->dx;
            g_core_state.input.mouse_delta_y += p->dy;
            break;
        }
        // ---- 场景存档 / 单帧步进 / 资产导入 / 材质 ----
        case ECMD_SAVE_SCENE: {
            char path[256] = {};
            std::strncpy(path, reinterpret_cast<const char*>(cmd.payload), 255);
            Scene* s = g_core_state.world ? g_core_state.world->scene() : nullptr;
            if (!s || path[0] == '\0') break;
            if (SceneSerializer::save_to_file(*s, path)) {
                g_core_state.current_scene_path = path;
                if (g_core_state.scene_mode == 0) g_core_state.scene_path_2d = path;
                else g_core_state.scene_path_3d = path;
                utils::GLog::instance().info("[Core] Scene saved: {}", path);
            } else {
                utils::GLog::instance().warn("[Core] Scene save failed: {}", path);
            }
            break;
        }
        case ECMD_STEP_FRAME: {
            // 暂停态下的逐帧前进；帧数在 GCore_BeginFrame 里一拍一帧地消费。
            const int frames = *reinterpret_cast<const int*>(cmd.payload);
            g_core_state.step_frames = frames > 0 ? frames : 1;
            break;
        }
        case ECMD_IMPORT_ASSET: {
            char path[256] = {};
            std::strncpy(path, reinterpret_cast<const char*>(cmd.payload), 255);
            Scene* s = g_core_state.world ? g_core_state.world->scene() : nullptr;
            if (!s || path[0] == '\0') break;
            // 导入 = 在场景根下建一个指向该模型的实体。资源的真正解析仍由
            // MeshRenderer 首次绘制时的 AssetManager 完成，这里只落场景节点，
            // 保证"拖进来就有东西、能被撤销"。
            std::string stem = std::filesystem::path(path).stem().string();
            Entity* e = s->create_entity(stem.empty() ? "Imported" : stem);
            if (!e) break;
            g_core_state.entity_map.alloc(e->uuid());
            auto created = components::ComponentFactory::instance().create("MeshRenderer");
            if (created) {
                e->add_component(std::move(created));
                for (const auto& comp : e->components()) {
                    if (std::strcmp(comp->type(), "MeshRenderer") == 0) {
                        static_cast<components::MeshRenderer*>(comp.get())->mesh_path = path;
                        break;
                    }
                }
                e->mark_dirty();
            }
            g_core_state.deferred_entity_list_changed = true;
            g_core_state.selected_entity = g_core_state.entity_map.lookup(e->uuid());
            g_core_state.deferred_selection_changed = true;
            utils::GLog::instance().info("[Core] Asset imported as entity: {}", path);
            break;
        }
        case ECMD_SET_MATERIAL: {
            const auto* p = reinterpret_cast<const GCmdSetMaterial*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->handle);
            if (!e) break;
            components::MeshRenderer* mr = nullptr;
            for (const auto& comp : e->components()) {
                if (auto* c = dynamic_cast<components::MeshRenderer*>(comp.get())) {
                    mr = c;
                    break;
                }
            }
            if (!mr) {
                auto created = components::ComponentFactory::instance().create("MeshRenderer");
                if (!created) break;
                auto* raw = created.get();
                e->add_component(std::move(created));
                mr = static_cast<components::MeshRenderer*>(raw);
            }
            if (p->mesh_path[0] != '\0') mr->mesh_path = p->mesh_path;
            render::Material* mat = mr->ensure_material();
            if (mat) {
                mat->set_albedo(math::Vector3f(p->albedo[0], p->albedo[1], p->albedo[2]));
                mat->set_roughness(p->roughness);
                mat->set_metallic(p->metallic);
            }
            e->mark_dirty();
            break;
        }
        case ECMD_SET_RENDER_TARGET: {
            // core 不能反向调 renderer 的 API：只登记请求，渲染侧在帧起点取走。
            const auto* p = reinterpret_cast<const GCmdSetRenderTarget*>(cmd.payload);
            if (p->target >= 0 && p->target < 2 && p->width > 0 && p->height > 0) {
                g_core_state.target_request_w[p->target] = p->width;
                g_core_state.target_request_h[p->target] = p->height;
            }
            break;
        }
        // ---- GryceSRT 脚本命令面 ----
        case ECMD_SET_SCRIPT: {
            // payload: { entity, "res:/scripts/x.js" }；空串表示卸载。
            struct Payload { GEntityHandle h; char script_path[120]; };
            static_assert(sizeof(Payload) <= GCMD_PAYLOAD_SIZE, "payload too big");
            const auto* p = reinterpret_cast<const Payload*>(cmd.payload);
            Entity* e = EntityResolver::resolve(p->h);
            if (e) {
                const char* path = p->script_path;
                const size_t n = bounded_strlen(path, sizeof(p->script_path));
                for (const auto& comp : e->components()) {
                    if (std::strcmp(comp->type(), "Script") != 0) continue;
                    auto* sc = static_cast<components::ScriptComponent*>(comp.get());
                    sc->set_script_path(std::string(path, n));
                    g_core_state.deferred_entity_list_changed = true;
                    break;
                }
            }
            break;
        }
        case ECMD_RELOAD_SCRIPTS: {
            // 无 payload：重载当前场景内所有脚本组件（编辑器"重新加载脚本"）。
            Scene* s = g_core_state.world ? g_core_state.world->scene() : nullptr;
            if (s) {
                s->foreach([](Entity* e) {
                    if (!e || !e->enabled) return;
                    for (const auto& comp : e->components()) {
                        if (comp->enabled && std::strcmp(comp->type(), "Script") == 0) {
                            static_cast<components::ScriptComponent*>(comp.get())->reload_now();
                        }
                    }
                });
            }
            break;
        }
        default:
            break;
    }
}

} // namespace gryce_core

// ============================================================================
// 编辑命令的入队辅助
// ============================================================================
namespace {

// 定长字符串拷贝：超长截断并保证 '\0' 结尾（payload 里的 name/path 字段都是定长）。
void edit_copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

int push_edit(GCommandType type, const void* payload, size_t size) {
    GCommand cmd{};
    cmd.type = type;
    if (payload && size > 0) {
        if (size > GCMD_PAYLOAD_SIZE) size = GCMD_PAYLOAD_SIZE;
        std::memcpy(cmd.payload, payload, size);
    }
    return GCore_PushCommand(&cmd) == 0 ? 1 : 0;
}

} // namespace

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

    // 编辑器/运行时统一注册核心系统：动画驱动、层级同步。
    gryce_core::g_core_state.world->register_system(std::make_unique<ecs::AnimatorSystem>());
    gryce_core::g_core_state.world->register_system(std::make_unique<ecs::HierarchySystem>());
    // 物理与音频：2D（Box2D，含风/浮力）、3D（Jolt）、音频（Amplitude）。
    // 均为无资源时的安全空转，项目未使用对应组件时不产生开销。
    gryce_core::g_core_state.world->register_system(std::make_unique<ecs::PhysicsSystem2D>());
    gryce_core::g_core_state.world->register_system(std::make_unique<ecs::PhysicsSystem3D>());
    gryce_core::g_core_state.world->register_system(std::make_unique<ecs::AudioSystem>());
    gryce_core::g_core_state.world->init();

    gryce_core::g_core_state.entity_map.rebuild(gryce_core::g_core_state.world->scene());

    // GryceSRT: ScriptVM is lazily initialized by ScriptSystem on first use.

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

    // 每帧开始清零累计的鼠标增量；随后由 process_command 期间推送的
    // ECMD_INPUT_MOUSE_MOVE 重新累加，供脚本在 on_update 读取本帧增量。
    // 注意：必须在本帧命令处理之前清零，否则会抹掉刚累加的 delta。
    gryce_core::g_core_state.input.mouse_delta_x = 0;
    gryce_core::g_core_state.input.mouse_delta_y = 0;
    // input_events 只承载"本帧"事件（fill by process_command），帧起点清空
    // 防止长会话下无界增长。clear 不释放容量，热路径复用已分配的缓冲。
    gryce_core::g_core_state.input.input_events.clear();

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
        } else if (gryce_core::g_core_state.step_frames > 0) {
            // 单帧步进：暂停态下按固定步长前进，一帧一拍。
            gryce_core::g_core_state.world->update(1.0f / 60.0f);
            --gryce_core::g_core_state.step_frames;
        }
        // 暂停时不驱动任何系统。
    } else {
        gryce_core::g_core_state.step_frames = 0;
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

// ---------------------------------------------------------------------------
// 撤销 / 重做
// ---------------------------------------------------------------------------
int GCore_Undo(void) {
    GRYCE_API_GUARD();
    auto& st = gryce_core::g_core_state;
    if (!st.initialized || !st.world) return 0;
    // 分组还开着时先收口：拖拽中途按 Ctrl+Z 不应该把"半个拖拽"当成一步。
    GCore_EndEdit();
    auto& u = gryce_core::undo_state();
    if (u.undo.empty()) return 0;
    std::string current = gryce_core::capture_scene_json();
    gryce_core::UndoEntry entry = std::move(u.undo.back());
    u.undo.pop_back();
    if (!gryce_core::restore_scene_json(entry.json)) {
        u.undo.push_back(std::move(entry));
        return 0;
    }
    u.redo.push_back({entry.label, std::move(current)});
    while (static_cast<int>(u.redo.size()) > u.limit) u.redo.erase(u.redo.begin());
    return 1;
}

int GCore_Redo(void) {
    GRYCE_API_GUARD();
    auto& st = gryce_core::g_core_state;
    if (!st.initialized || !st.world) return 0;
    GCore_EndEdit();
    auto& u = gryce_core::undo_state();
    if (u.redo.empty()) return 0;
    std::string current = gryce_core::capture_scene_json();
    gryce_core::UndoEntry entry = std::move(u.redo.back());
    u.redo.pop_back();
    if (!gryce_core::restore_scene_json(entry.json)) {
        u.redo.push_back(std::move(entry));
        return 0;
    }
    u.undo.push_back({entry.label, std::move(current)});
    while (static_cast<int>(u.undo.size()) > u.limit) u.undo.erase(u.undo.begin());
    return 1;
}

int GCore_GetUndoCount(void) {
    return static_cast<int>(gryce_core::undo_state().undo.size());
}

int GCore_GetRedoCount(void) {
    return static_cast<int>(gryce_core::undo_state().redo.size());
}

void GCore_ClearUndoHistory(void) {
    GRYCE_API_GUARD();
    auto& u = gryce_core::undo_state();
    u.undo.clear();
    u.redo.clear();
    u.group_open = false;
    u.group_base.clear();
}

void GCore_SetUndoLimit(int limit) {
    GRYCE_API_GUARD();
    auto& u = gryce_core::undo_state();
    u.limit = limit < 0 ? 0 : limit;
    while (static_cast<int>(u.undo.size()) > u.limit) u.undo.erase(u.undo.begin());
    while (static_cast<int>(u.redo.size()) > u.limit) u.redo.erase(u.redo.begin());
}

void GCore_BeginEdit(const char* label) {
    GRYCE_API_GUARD();
    auto& u = gryce_core::undo_state();
    if (u.group_open) return;   // 嵌套只认最外层，内层不重复抓快照
    u.group_base = gryce_core::capture_scene_json();
    u.group_label = (label && label[0]) ? label : "edit";
    u.group_open = true;
}

void GCore_EndEdit(void) {
    GRYCE_API_GUARD();
    auto& u = gryce_core::undo_state();
    if (!u.group_open) return;
    u.group_open = false;
    if (u.limit <= 0 || gryce_core::g_core_state.play_mode || u.group_base.empty()) {
        u.group_base.clear();
        return;
    }
    u.undo.push_back({u.group_label, std::move(u.group_base)});
    u.group_base.clear();
    while (static_cast<int>(u.undo.size()) > u.limit) u.undo.erase(u.undo.begin());
    u.redo.clear();
}

int GCore_TakeRenderTargetRequest(int target, int* out_w, int* out_h) {
    GRYCE_API_GUARD();
    if (target < 0 || target > 1) return 0;
    auto& st = gryce_core::g_core_state;
    if (st.target_request_w[target] <= 0) return 0;
    if (out_w) *out_w = st.target_request_w[target];
    if (out_h) *out_h = st.target_request_h[target];
    st.target_request_w[target] = -1;
    st.target_request_h[target] = -1;
    return 1;
}

// ---------------------------------------------------------------------------
// 编辑命令面（edit_api.h）
//
// 全部只是把语义参数装进 payload 再入队；真正的执行在 process_command。
// 这样编辑器不需要知道 payload 布局，也不需要在两个地方维护同一份结构体。
// ---------------------------------------------------------------------------
int GEdit_CreateEntity(const char* name, GEntityHandle parent) {
    GCmdCreateEntity p{};
    edit_copy_str(p.name, sizeof(p.name), name);
    p.parent = parent;
    return push_edit(ECMD_CREATE_ENTITY, &p, sizeof(p));
}

int GEdit_DestroyEntity(GEntityHandle entity) {
    return push_edit(ECMD_DESTROY_ENTITY, &entity, sizeof(entity));
}

int GEdit_RenameEntity(GEntityHandle entity, const char* name) {
    GCmdRenameEntity p{};
    p.handle = entity;
    edit_copy_str(p.name, sizeof(p.name), name);
    return push_edit(ECMD_RENAME_ENTITY, &p, sizeof(p));
}

int GEdit_ReparentEntity(GEntityHandle entity, GEntityHandle parent) {
    GCmdReparentEntity p{};
    p.handle = entity;
    p.parent = parent;
    return push_edit(ECMD_REPARENT_ENTITY, &p, sizeof(p));
}

int GEdit_SelectEntity(GEntityHandle entity) {
    return push_edit(ECMD_SELECT_ENTITY, &entity, sizeof(entity));
}

int GEdit_SetTransform(GEntityHandle entity, const GVec3* position,
                       const GQuat* rotation, const GVec3* scale) {
    gryce_engine::scene::Entity* e = gryce_core::EntityResolver::resolve(entity);
    if (!e || !e->transform()) return 0;
    const auto* t = e->transform();
    GCmdSetTransform p{};
    p.handle = entity;
    // 缺省分量用当前值补齐：Inspector 的单个数值框、Gizmo 的单轴拖拽都只带
    // 一个分量，剩下的必须原样保留，否则一拖就把另外两轴清零。
    p.position = position ? *position
                          : GVec3{t->position.x, t->position.y, t->position.z};
    p.scale = scale ? *scale : GVec3{t->scale.x, t->scale.y, t->scale.z};
    const bool zero_rot = !rotation || (rotation->x == 0.0f && rotation->y == 0.0f &&
                                        rotation->z == 0.0f && rotation->w == 0.0f);
    p.rotation = zero_rot ? GQuat{t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w}
                          : *rotation;
    return push_edit(ECMD_SET_TRANSFORM, &p, sizeof(p));
}

int GEdit_TranslateEntity(GEntityHandle entity, const GVec3* delta) {
    if (!delta) return 0;
    // 只校验句柄存在，不读当前值：增量在 process_command 里执行时才叠加，
    // 这样同一帧内"先 SetTransform 再 Translate"两条命令不会互相吃掉。
    gryce_engine::scene::Entity* e = gryce_core::EntityResolver::resolve(entity);
    if (!e || !e->transform()) return 0;
    GCmdTranslateEntity p{};
    p.handle = entity;
    p.delta = *delta;
    return push_edit(ECMD_TRANSLATE_ENTITY, &p, sizeof(p));
}

int GEdit_SetProperty(GEntityHandle entity, uint64_t type_hash, const char* prop_name,
                      const void* value, int value_size) {
    if (!prop_name || prop_name[0] == '\0') return 0;
    GCmdSetProperty p{};
    p.handle = entity;
    p.type_hash = type_hash;
    edit_copy_str(p.prop_name, sizeof(p.prop_name), prop_name);
    if (value && value_size > 0) {
        if (value_size > static_cast<int>(sizeof(p.value))) return 0;   // 超长字段不支持
        std::memcpy(p.value, value, static_cast<size_t>(value_size));
    }
    return push_edit(ECMD_SET_PROPERTY, &p, sizeof(p));
}

int GEdit_AddComponent(GEntityHandle entity, const char* type_name) {
    if (!type_name || type_name[0] == '\0') return 0;
    GCmdAddComponent p{};
    p.handle = entity;
    edit_copy_str(p.type_name, sizeof(p.type_name), type_name);
    return push_edit(ECMD_ADD_COMPONENT, &p, sizeof(p));
}

int GEdit_RemoveComponent(GEntityHandle entity, uint64_t type_hash) {
    GCmdRemoveComponent p{};
    p.handle = entity;
    p.type_hash = type_hash;
    return push_edit(ECMD_REMOVE_COMPONENT, &p, sizeof(p));
}

int GEdit_SetMaterial(GEntityHandle entity, const char* mesh_path,
                      float r, float g, float b, float roughness, float metallic) {
    GCmdSetMaterial p{};
    p.handle = entity;
    edit_copy_str(p.mesh_path, sizeof(p.mesh_path), mesh_path);
    p.albedo[0] = r;
    p.albedo[1] = g;
    p.albedo[2] = b;
    p.roughness = roughness;
    p.metallic = metallic;
    return push_edit(ECMD_SET_MATERIAL, &p, sizeof(p));
}

int GEdit_SetRenderTargetSize(int target, int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    GCmdSetRenderTarget p{};
    p.target = target;
    p.width = width;
    p.height = height;
    return push_edit(ECMD_SET_RENDER_TARGET, &p, sizeof(p));
}

int GEdit_SaveScene(const char* path) {
    char buf[256] = {};
    edit_copy_str(buf, sizeof(buf), path);
    return push_edit(ECMD_SAVE_SCENE, buf, sizeof(buf));
}

int GEdit_ImportAsset(const char* path) {
    char buf[256] = {};
    edit_copy_str(buf, sizeof(buf), path);
    return push_edit(ECMD_IMPORT_ASSET, buf, sizeof(buf));
}

int GEdit_SetPlayMode(int mode) {
    const bool playing = gryce_core::g_core_state.play_mode;
    switch (mode) {
        case 0: return playing ? push_edit(ECMD_STOP_MODE, nullptr, 0) : 1;
        case 1: return playing ? 1 : push_edit(ECMD_PLAY_MODE, nullptr, 0);
        case 2: return playing ? push_edit(ECMD_PAUSE_MODE, nullptr, 0) : 0;
        default: return 0;
    }
}

int GEdit_StepFrame(int frames) {
    const int n = frames > 0 ? frames : 1;
    return push_edit(ECMD_STEP_FRAME, &n, sizeof(n));
}

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
void GCore_RegisterCallback_OnMouseLock(GOnMouseLock cb) {
    GRYCE_API_GUARD();
    gryce_core::g_core_state.callbacks.on_mouse_lock = cb;
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
