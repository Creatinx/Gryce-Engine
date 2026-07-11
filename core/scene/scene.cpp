#include "scene.h"

#include "prefab.h"
#include "scene_serializer.h"
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

    // 先递归销毁子实体
    for (auto& child : entity->children()) {
        destroy_entity(child.get());
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
    auto prefab = Prefab::load(scene_path);
    if (!prefab) {
        GLOG_ERROR("Scene::create_prefab: failed to load prefab '{}'", scene_path);
        return nullptr;
    }
    return prefab->instantiate(this);
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
    for (auto& root : roots_) {
        root->on_init();
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

} // namespace gryce_engine::scene
