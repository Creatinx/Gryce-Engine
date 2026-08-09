#include "GryceCore/entity_api.h"
#include "GryceCore/api_guard.h"
#include "internal_state.h"

#include "ecs/world.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "scene/scene_serializer.h"
#include "scene/prefab.h"
#include "components/transform.h"
#include "components/prefab_instance.h"
#include "utils/glog/glog_lib.h"

#include <nlohmann/json.hpp>
#include <cstring>

using gryce_engine::scene::Scene;
using gryce_engine::scene::Entity;

namespace gc = gryce_core;

extern "C" {

int GEntity_GetCount(void) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.world || !gc::g_core_state.world->scene()) return 0;
    int count = 0;
    gc::g_core_state.world->scene()->foreach([&](Entity* e) {
        if (e && e->parent() != nullptr) ++count;
    });
    return count;
}

GEntityHandle GEntity_GetAt(int index) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.world || !gc::g_core_state.world->scene() || index < 0) return 0;
    GEntityHandle result = 0;
    int i = 0;
    gc::g_core_state.world->scene()->foreach([&](Entity* e) {
        if (e && e->parent() != nullptr) {
            if (i == index) {
                result = gc::g_core_state.entity_map.lookup(e->uuid());
            }
            ++i;
        }
    });
    return result;
}

int GEntity_GetName(GEntityHandle entity, char* out_buf, int buf_size) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e || !out_buf || buf_size <= 0) return -1;
    std::strncpy(out_buf, e->name().c_str(), static_cast<size_t>(buf_size) - 1);
    out_buf[buf_size - 1] = '\0';
    return static_cast<int>(std::strlen(out_buf));
}

int GEntity_GetPath(GEntityHandle entity, char* out_buf, int buf_size) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e || !out_buf || buf_size <= 0) return -1;
    std::string path = e->name();
    Entity* cur = e->parent();
    while (cur && cur->parent() != nullptr) {
        path = cur->name() + "/" + path;
        cur = cur->parent();
    }
    std::strncpy(out_buf, path.c_str(), static_cast<size_t>(buf_size) - 1);
    out_buf[buf_size - 1] = '\0';
    return static_cast<int>(std::strlen(out_buf));
}

GEntityHandle GEntity_GetParent(GEntityHandle entity) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e) return 0;
    Entity* p = e->parent();
    if (!p || p->parent() == nullptr) return 0;
    return gc::g_core_state.entity_map.lookup(p->uuid());
}

int GEntity_GetChildCount(GEntityHandle entity) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e) return 0;
    return static_cast<int>(e->children().size());
}

GEntityHandle GEntity_GetChildAt(GEntityHandle entity, int index) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e || index < 0 || static_cast<size_t>(index) >= e->children().size()) return 0;
    return gc::g_core_state.entity_map.lookup(e->children()[index]->uuid());
}

int GEntity_GetSiblingIndex(GEntityHandle entity) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e || !e->parent()) return -1;
    const auto& siblings = e->parent()->children();
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].get() == e) return static_cast<int>(i);
    }
    return -1;
}

GEntityHandle GEntity_GetSelected(void) {
    GRYCE_API_GUARD();
    return gc::g_core_state.selected_entity;
}

// --- Transform ---
int GEntity_GetLocalPosition(GEntityHandle entity, GVec3* out_pos) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e || !out_pos) return -1;
    auto* t = e->transform();
    if (!t) return -1;
    out_pos->x = t->position.x;
    out_pos->y = t->position.y;
    out_pos->z = t->position.z;
    return 0;
}

int GEntity_GetLocalRotation(GEntityHandle entity, GQuat* out_rot) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e || !out_rot) return -1;
    auto* t = e->transform();
    if (!t) return -1;
    out_rot->x = t->rotation.x;
    out_rot->y = t->rotation.y;
    out_rot->z = t->rotation.z;
    out_rot->w = t->rotation.w;
    return 0;
}

int GEntity_GetLocalScale(GEntityHandle entity, GVec3* out_scale) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e || !out_scale) return -1;
    auto* t = e->transform();
    if (!t) return -1;
    out_scale->x = t->scale.x;
    out_scale->y = t->scale.y;
    out_scale->z = t->scale.z;
    return 0;
}

int GEntity_GetWorldPosition(GEntityHandle entity, GVec3* out_pos) {
    GRYCE_API_GUARD();
    (void)entity; (void)out_pos;
    return -1; // TODO
}
int GEntity_GetWorldRotation(GEntityHandle entity, GQuat* out_rot) {
    GRYCE_API_GUARD();
    (void)entity; (void)out_rot;
    return -1; // TODO
}
int GEntity_GetWorldScale(GEntityHandle entity, GVec3* out_scale) {
    GRYCE_API_GUARD();
    (void)entity; (void)out_scale;
    return -1; // TODO
}

// --- Transform Setters ---
int GEntity_SetLocalPosition(GEntityHandle entity, const GVec3* pos) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e || !pos) return -1;
    auto* t = e->transform();
    if (!t) return -1;
    t->position.x = pos->x;
    t->position.y = pos->y;
    t->position.z = pos->z;
    e->mark_dirty();
    return 0;
}

int GEntity_SetLocalRotation(GEntityHandle entity, const GQuat* rot) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e || !rot) return -1;
    auto* t = e->transform();
    if (!t) return -1;
    t->rotation.x = rot->x;
    t->rotation.y = rot->y;
    t->rotation.z = rot->z;
    t->rotation.w = rot->w;
    e->mark_dirty();
    return 0;
}

int GEntity_SetLocalScale(GEntityHandle entity, const GVec3* scale) {
    GRYCE_API_GUARD();
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e || !scale) return -1;
    auto* t = e->transform();
    if (!t) return -1;
    t->scale.x = scale->x;
    t->scale.y = scale->y;
    t->scale.z = scale->z;
    e->mark_dirty();
    return 0;
}

int GEntity_ExportJson(GEntityHandle entity, char* out_buf, int buf_size) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized || !out_buf || buf_size <= 0) return -1;
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e) return -1;

    nlohmann::json out;
    out["version"] = 1;
    out["type"] = "entity";
    out["name"] = e->name();
    nlohmann::json entities = nlohmann::json::array();
    bool first = true;
    e->foreach([&](Entity* ent) {
        if (!ent) return;
        auto j = gryce_engine::scene::SceneSerializer::serialize_entity(*ent);
        if (first) {
            j["parent"] = nullptr; // 截断根的外部父引用
            first = false;
        }
        entities.push_back(std::move(j));
    });
    out["entities"] = std::move(entities);

    const std::string dump = out.dump();
    if (dump.size() >= static_cast<size_t>(buf_size)) return -1;
    std::strncpy(out_buf, dump.c_str(), static_cast<size_t>(buf_size) - 1);
    out_buf[buf_size - 1] = '\0';
    return static_cast<int>(dump.size());
}

GEntityHandle GEntity_ImportJson(const char* json, GEntityHandle parent_handle) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized || !json || !json[0]) return 0;
    Scene* s = gc::g_core_state.world ? gc::g_core_state.world->scene() : nullptr;
    if (!s) return 0;

    try {
        auto parsed = nlohmann::json::parse(json);
        auto temp = gryce_engine::scene::SceneSerializer::deserialize(parsed);
        if (!temp) return 0;

        // 解除临时场景的 store 引用，避免析构时悬垂；数据随后经 clone 深拷贝。
        temp->foreach([](Entity* ent) { if (ent) ent->set_store(nullptr); });

        Entity* parent = gc::EntityResolver::resolve(parent_handle);
        GEntityHandle first_handle = 0;
        for (const auto& root : temp->roots()) {
            if (!root) continue;
            auto cloned = root->clone();
            Entity* raw = cloned.get();

            // 有指定父级：直接挂到父节点下（接管所有权并继承 store）；
            // 无父级（0 或场景根）：作为根级实体加入。
            if (parent && parent != s->root()) {
                parent->add_child(std::move(cloned));
            } else {
                s->add_root_entity(std::move(cloned));
            }

            // 为子树内所有实体分配句柄（含子孙），否则子实体无法被编辑器寻址。
            GEntityHandle root_handle = 0;
            raw->foreach([&](Entity* ent) {
                if (!ent) return;
                GEntityHandle h = gc::g_core_state.entity_map.alloc(ent->uuid());
                if (!root_handle) root_handle = h;
            });
            if (!first_handle) first_handle = root_handle;
        }

        if (first_handle) {
            gc::g_core_state.deferred_entity_list_changed = true;
        }
        return first_handle;
    } catch (const std::exception& ex) {
        gryce_engine::utils::GLog::instance().warn("[Core] GEntity_ImportJson failed: {}", ex.what());
        return 0;
    }
}

int GEntity_SaveAsPrefab(GEntityHandle entity, const char* path) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized || !path || !path[0]) return -1;
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e) return -1;
    return gryce_engine::scene::Prefab::save(e, path) ? 0 : -1;
}

GEntityHandle GEntity_CreatePrefabInstance(const char* prefab_path, GEntityHandle parent_handle) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized || !prefab_path || !prefab_path[0]) return 0;
    Scene* s = gc::g_core_state.world ? gc::g_core_state.world->scene() : nullptr;
    if (!s) return 0;

    Entity* root = gryce_engine::scene::Prefab::instantiate(s, prefab_path);
    if (!root) {
        gryce_engine::utils::GLog::instance().warn(
            "[Core] GEntity_CreatePrefabInstance failed: '{}'", prefab_path);
        return 0;
    }

    // 可选：移到指定父级下（安全路径 detach + add_child）
    Entity* parent = gc::EntityResolver::resolve(parent_handle);
    if (parent && parent != s->root() && root->parent() == s->root()) {
        auto owned = s->root()->detach_child(root);
        if (owned) {
            Entity* raw = owned.get();
            parent->add_child(std::move(owned));
            root = raw;
        }
    }

    GEntityHandle first_handle = 0;
    root->foreach([&](Entity* ent) {
        if (!ent) return;
        GEntityHandle h = gc::g_core_state.entity_map.alloc(ent->uuid());
        if (!first_handle) first_handle = h;
    });
    if (first_handle) gc::g_core_state.deferred_entity_list_changed = true;
    return first_handle;
}

int GEntity_ApplyPrefab(GEntityHandle entity) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized) return -1;
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e) return -1;
    auto* inst = gryce_engine::scene::Prefab::get_instance(e);
    if (!inst || inst->prefab_path.empty()) return -1;
    const bool ok = gryce_engine::scene::Prefab::save(e, inst->prefab_path);
    if (ok) gc::g_core_state.deferred_entity_list_changed = true;
    return ok ? 0 : -1;
}

int GEntity_RevertPrefab(GEntityHandle entity) {
    GRYCE_API_GUARD();
    if (!gc::g_core_state.initialized) return -1;
    Entity* e = gc::EntityResolver::resolve(entity);
    if (!e) return -1;
    const bool ok = gryce_engine::scene::Prefab::revert(e);
    if (ok) gc::g_core_state.deferred_entity_list_changed = true;
    return ok ? 0 : -1;
}

} // extern "C"
