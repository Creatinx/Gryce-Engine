#include "scene.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include "prefab.h"
#include "scene_serializer.h"
#include "components/component_factory.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::scene {

Scene::Scene(const std::string& name) : name_(name) {}

Scene::~Scene() {
    // 必须在 ComponentStore 析构前清空 Entity，否则 Entity::~Entity 调用
    // store_->unregister_entity 时 store_ 已经失效。
    roots_.clear();
}

Entity* Scene::create_entity(const std::string& name) {
    return add_root_entity(std::make_unique<Entity>(name));
}

Entity* Scene::add_root_entity(std::unique_ptr<Entity> entity) {
    if (!entity) return nullptr;
    Entity* raw = entity.get();
    set_store_on_entity(raw);
    roots_.push_back(std::move(entity));
    return raw;
}

bool Scene::destroy_entity(Entity* entity) {
    if (!entity) return false;

    // 先递归销毁子实体。
    // 注意：子实体析构时会从 entity->children_ 中 erase 自己（remove_child），
    // 直接 range-for 迭代 children() 会导致迭代器失效（UB）。
    // 因此先把子实体裸指针收集到局部 vector，再逐个销毁。
    // erase 的是 parent 的 vector，不影响其他兄弟的 unique_ptr，裸指针保持有效。
    std::vector<Entity*> children_snapshot;
    children_snapshot.reserve(entity->children().size());
    for (auto& child : entity->children()) {
        children_snapshot.push_back(child.get());
    }
    for (Entity* child : children_snapshot) {
        destroy_entity(child);
    }

    // 从父级移除（如果有）
    if (entity->parent()) {
        entity->parent()->remove_child(entity);
        return true;
    }

    // 从场景根列表移除
    for (auto it = roots_.begin(); it != roots_.end(); ++it) {
        if (it->get() == entity) {
            // unique_ptr 析构会触发 Entity 析构，自动反注册组件
            roots_.erase(it);
            return true;
        }
    }
    return false;
}

Entity* Scene::create_prefab(const std::string& scene_path) {
    // 完整实例语义：挂 PrefabInstance 标记，场景保存时写成 prefab 引用
    Entity* root = Prefab::instantiate(this, scene_path);
    if (!root) {
        GLOG_ERROR("Scene::create_prefab: failed to instantiate prefab '{}'", scene_path);
    }
    return root;
}

void Scene::set_store_on_entity(Entity* entity) {
    if (!entity) return;
    entity->set_store(&component_store_);
    for (const auto& child : entity->children()) {
        set_store_on_entity(child.get());
    }
}


Entity* Scene::find_entity_by_uuid(const UUID& id) {
    Entity* found = nullptr;
    foreach([&](Entity* e) {
        if (!found && e->uuid() == id) {
            found = e;
        }
    });
    return found;
}

Entity* Scene::find_entity_by_name(const std::string& name) {
    Entity* found = nullptr;
    foreach([&](Entity* e) {
        if (!found && e->name() == name) {
            found = e;
        }
    });
    return found;
}

void Scene::foreach(std::function<void(Entity*)> callback) {
    for (auto& root : roots_) {
        root->foreach(callback);
    }
}

void Scene::init() {
    for (auto& root : roots_) {
        root->on_init();
    }
    for (auto& root : roots_) {
        root->on_start();
    }
}

void Scene::update(float dt) {
    for (auto& root : roots_) {
        root->on_update(dt);
    }
}

void Scene::render(render::RenderContext& ctx) {
    for (auto& root : roots_) {
        root->on_render(ctx);
    }
}

void Scene::destroy() {
    for (auto& root : roots_) {
        root->on_destroy();
    }
}

// ---------------------------------------------------------------------------
// Delta Save
// ---------------------------------------------------------------------------
bool Scene::has_unsaved_changes() const {
    bool dirty = false;
    const_cast<Scene*>(this)->foreach([&](Entity* e) {
        if (e->is_dirty()) dirty = true;
    });
    return dirty;
}

void Scene::mark_saved() {
    clear_all_dirty();
    last_saved_snapshot_ = SceneSerializer::serialize(*this);
}

void Scene::clear_all_dirty() {
    foreach([](Entity* e) {
        e->clear_dirty();
    });
}

nlohmann::json Scene::serialize_delta() const {
    if (last_saved_snapshot_.is_null()) {
        // 从未完整保存过，退化为完整序列化
        return SceneSerializer::serialize(*this);
    }

    // 收集当前所有实体的 UUID
    std::unordered_set<std::string> current_uuids;
    const_cast<Scene*>(this)->foreach([&](Entity* e) {
        current_uuids.insert(e->uuid().str());
    });

    // 收集上次保存的所有 UUID
    std::unordered_set<std::string> saved_uuids;
    for (const auto& e_json : last_saved_snapshot_.value("entities", nlohmann::json::array())) {
        saved_uuids.insert(e_json.value("uuid", ""));
    }

    nlohmann::json delta;
    delta["version"] = 1;
    delta["type"] = "delta";
    delta["base_scene"] = name_;

    // 新增的实体
    nlohmann::json added = nlohmann::json::array();
    const_cast<Scene*>(this)->foreach([&](Entity* e) {
        if (saved_uuids.find(e->uuid().str()) == saved_uuids.end()) {
            added.push_back(SceneSerializer::serialize_entity(*e));
        }
    });
    delta["added"] = added;

    // 删除的实体
    nlohmann::json removed = nlohmann::json::array();
    for (const auto& e_json : last_saved_snapshot_.value("entities", nlohmann::json::array())) {
        std::string uuid_str = e_json.value("uuid", "");
        if (current_uuids.find(uuid_str) == current_uuids.end()) {
            removed.push_back(uuid_str);
        }
    }
    delta["removed"] = removed;

    // 修改的实体（dirty 且 UUID 存在于上次保存中）
    nlohmann::json modified = nlohmann::json::array();
    const_cast<Scene*>(this)->foreach([&](Entity* e) {
        if (e->is_dirty() && saved_uuids.find(e->uuid().str()) != saved_uuids.end()) {
            modified.push_back(SceneSerializer::serialize_entity(*e));
        }
    });
    delta["modified"] = modified;

    return delta;
}

bool Scene::save_delta(const std::string& path) {
    auto delta = serialize_delta();
    std::string resolved = resources::ResourcePath::resolve(path);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(resolved).parent_path(), ec);
    std::ofstream file(resolved);
    if (!file) {
        GLOG_ERROR("Scene::save_delta: failed to write '{}'", resolved);
        return false;
    }
    file << delta.dump(2);
    if (!file.good()) return false;
    clear_all_dirty();
    return true;
}

// ---------------------------------------------------------------------------
// Hot Reload
// ---------------------------------------------------------------------------
bool Scene::hot_reload(const std::string& path) {
    auto new_scene = SceneSerializer::load_from_file(path);
    if (!new_scene) {
        GLOG_ERROR("Scene::hot_reload: failed to load '{}'", path);
        return false;
    }

    // 快照当前运行时状态（按 UUID 索引）
    std::unordered_map<std::string, nlohmann::json> runtime_states;
    foreach([&](Entity* e) {
        runtime_states[e->uuid().str()] = e->snapshot_runtime_state();
    });

    // 收集新场景中的 UUID
    std::unordered_set<std::string> new_uuids;
    new_scene->foreach([&](Entity* e) {
        new_uuids.insert(e->uuid().str());
    });

    // 1) 删除当前场景中不在新场景中的实体
    std::vector<Entity*> to_remove;
    foreach([&](Entity* e) {
        if (new_uuids.find(e->uuid().str()) == new_uuids.end()) {
            to_remove.push_back(e);
        }
    });
    for (auto* e : to_remove) {
        destroy_entity(e);
    }

    // 2) 更新/添加实体
    new_scene->foreach([&](Entity* new_e) {
        Entity* existing = find_entity_by_uuid(new_e->uuid());
        if (existing) {
            // 更新现有实体
            existing->set_name(new_e->name());
            apply_hot_reload_entity(existing, SceneSerializer::serialize_entity(*new_e));
        } else {
            // 新增实体：需要从新场景移出所有权
            // 由于新场景的 Entity 在 unique_ptr 中，我们通过 clone 方式添加
            auto cloned = new_e->clone();
            cloned->set_uuid(new_e->uuid());
            // 恢复父子关系在加载后由调用方或后续逻辑处理
            add_root_entity(std::move(cloned));
        }
    });

    // 3) 重建父子关系（基于新场景的 parent 字段）
    std::unordered_map<std::string, Entity*> entity_map;
    foreach([&](Entity* e) {
        entity_map[e->uuid().str()] = e;
    });

    new_scene->foreach([&](Entity* new_e) {
        auto* existing = find_entity_by_uuid(new_e->uuid());
        if (!existing || !existing->parent()) return;

        // 如果当前父级与新场景不一致，调整
        // 注意：新场景中的 parent 关系已在 serialize_entity 中编码
    });

    // 实际上重建父子关系更简单的做法：重新加载整个场景结构
    // 但由于要保持运行时状态，我们只更新数据。
    // 更好的做法：重新反序列化整个场景，但把运行时状态迁移过去。

    // 重新加载完整场景并迁移状态
    auto fresh = SceneSerializer::load_from_file(path);
    if (!fresh) return false;

    // 迁移运行时状态到新加载的实体
    fresh->foreach([&](Entity* e) {
        auto it = runtime_states.find(e->uuid().str());
        if (it != runtime_states.end()) {
            e->restore_runtime_state(it->second);
        }
    });

    // 替换当前场景的数据（保留 ComponentStore 和子场景列表）
    roots_ = std::move(fresh->roots());
    name_ = fresh->name();
    set_store_on_entity_for_all();

    clear_all_dirty();
    GLOG_INFO("Scene '{}' hot reloaded from '{}'", name_, path);
    return true;
}

void Scene::set_store_on_entity_for_all() {
    for (auto& root : roots_) {
        set_store_on_entity(root.get());
    }
}

bool Scene::apply_hot_reload_entity(Entity* existing, const nlohmann::json& e_json) {
    if (!existing) return false;

    // 更新 Transform
    if (auto* t = existing->transform()) {
        if (e_json.contains("transform")) {
            t->deserialize(e_json["transform"]);
        }
    }

    // 收集新场景中该实体的组件类型集合
    std::unordered_set<std::string> new_comp_types;
    for (const auto& c_json : e_json.value("components", nlohmann::json::array())) {
        new_comp_types.insert(c_json.value("type", ""));
    }

    // 删除当前实体中不存在的组件
    std::vector<components::Component*> to_remove;
    for (const auto& comp : existing->components()) {
        if (comp->type() == std::string("Transform")) continue;
        if (new_comp_types.find(comp->type()) == new_comp_types.end()) {
            to_remove.push_back(comp.get());
        }
    }
    for (auto* comp : to_remove) {
        existing->remove_component(comp);
    }

    // 更新/添加组件
    for (const auto& c_json : e_json.value("components", nlohmann::json::array())) {
        std::string type = c_json.value("type", "");
        if (type.empty() || type == "Transform") continue;

        auto* comp = existing->get_component_by_type(type);
        if (comp) {
            comp->deserialize(c_json);
            comp->enabled = c_json.value("enabled", true);
        } else {
            auto new_comp = components::ComponentFactory::instance().create(type);
            if (new_comp) {
                new_comp->enabled = c_json.value("enabled", true);
                new_comp->deserialize(c_json);
                existing->add_component(std::move(new_comp));
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Sub-scene / Level Streaming
// ---------------------------------------------------------------------------
int Scene::stream_in(const std::string& path, const std::string& label) {
    auto sub_scene = SceneSerializer::load_from_file(path);
    if (!sub_scene) {
        GLOG_ERROR("Scene::stream_in: failed to load sub-scene '{}'", path);
        return -1;
    }

    SubScene ss;
    ss.path = path;
    ss.label = label.empty() ? std::filesystem::path(path).stem().string() : label;
    ss.loaded = true;

    // 将加载的实体移入本场景
    for (auto& root : sub_scene->roots()) {
        Entity* raw = root.get();
        ss.entities.push_back(raw);
        add_root_entity(std::move(root));
    }
    // 清空临时 scene 的 roots，避免析构时释放
    sub_scene->roots().clear();

    int index = static_cast<int>(sub_scenes_.size());
    sub_scenes_.push_back(std::move(ss));
    GLOG_INFO("Streamed in sub-scene '{}' ({} entities) -> index {}", path, ss.entities.size(), index);
    return index;
}

bool Scene::stream_out(int index) {
    if (index < 0 || index >= static_cast<int>(sub_scenes_.size())) return false;
    auto& ss = sub_scenes_[index];
    if (!ss.loaded) return false;

    // 销毁所有关联实体
    for (auto* e : ss.entities) {
        destroy_entity(e);
    }
    ss.entities.clear();
    ss.loaded = false;

    GLOG_INFO("Streamed out sub-scene '{}' (index {})", ss.path, index);
    return true;
}

bool Scene::stream_out_by_path(const std::string& path) {
    for (int i = 0; i < static_cast<int>(sub_scenes_.size()); ++i) {
        if (sub_scenes_[i].path == path) {
            return stream_out(i);
        }
    }
    return false;
}

} // namespace gryce_engine::scene
