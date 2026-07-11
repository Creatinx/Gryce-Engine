#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>

#include "ecs/component_store.h"
#include "scene/entity.h"

namespace gryce_engine::scene {

// ---------------------------------------------------------------------------
// Scene — 场景，包含若干根实体
// ---------------------------------------------------------------------------
class Scene {
public:
    explicit Scene(const std::string& name = "Scene");
    ~Scene();

    const std::string& name() const { return name_; }
    void set_name(const std::string& name) { name_ = name; }

    // 组件存储池访问
    ecs::ComponentStore& component_store() { return component_store_; }
    const ecs::ComponentStore& component_store() const { return component_store_; }

    Entity* create_entity(const std::string& name = "Entity");
    Entity* add_root_entity(std::unique_ptr<Entity> entity);

    // 运行时销毁实体（递归销毁子实体并反注册所有组件）
    bool destroy_entity(Entity* entity);

    // 把另一个 .gesc 场景作为 prefab 实例化到本场景
    // 返回第一个根实体（方便调用方继续操作）
    Entity* create_prefab(const std::string& scene_path);

    const std::vector<std::unique_ptr<Entity>>& roots() const { return roots_; }
    std::vector<std::unique_ptr<Entity>>& roots() { return roots_; }

    Entity* find_entity_by_uuid(const UUID& id);
    Entity* find_entity_by_name(const std::string& name);

    // 遍历所有实体（先根后子）
    void foreach(std::function<void(Entity*)> callback);

    // 场景级生命周期驱动
    void init();
    void update(float dt);
    void render(render::RenderContext& ctx);
    void destroy();

private:
    void set_store_on_entity(Entity* entity);

    std::string name_;
    std::vector<std::unique_ptr<Entity>> roots_;
    ecs::ComponentStore component_store_;
};

} // namespace gryce_engine::scene
