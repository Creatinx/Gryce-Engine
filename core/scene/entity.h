#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <type_traits>

#include "components/component.h"
#include "components/transform.h"
#include "ecs/component_store.h"
#include "ecs/types.h"
#include "scene/uuid.h"

namespace gryce_engine::scene {

// ---------------------------------------------------------------------------
// Entity — 场景中的实体（Godot Node + Unity GameObject 混合风格）
// - 每个 Entity 默认拥有一个 Transform 组件
// - 可挂载多个 Component
// - 支持父子层级
// ---------------------------------------------------------------------------
class Entity {
public:
    explicit Entity(const std::string& name = "Entity");

    // ECS 唯一标识（运行时分配，与序列化用的 UUID 分离）
    ecs::EntityID id() const { return id_; }

    // 关联组件存储池（由 Scene 设置）。设置时会将已存在的组件注册进存储池。
    void set_store(ecs::ComponentStore* store);
    ~Entity();

    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;
    Entity(Entity&&) = default;
    Entity& operator=(Entity&&) = default;

    // 标识
    const std::string& name() const { return name_; }
    void set_name(const std::string& name) { name_ = name; }

    const UUID& uuid() const { return uuid_; }
    void set_uuid(const UUID& id) { uuid_ = id; }

    bool enabled = true; // Entity 级开关，影响组件 update/render 及查询

    // 层级
    Entity* parent() const { return parent_; }
    void set_parent(Entity* parent);

    Entity* add_child(std::unique_ptr<Entity> child);
    bool remove_child(Entity* child);
    const std::vector<std::unique_ptr<Entity>>& children() const { return children_; }

    // 组件
    template<typename T, typename... Args>
    T* add_component(Args&&... args) {
        static_assert(std::is_base_of_v<components::Component, T>, "T must derive from Component");
        auto comp = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = comp.get();
        ptr->on_attach(this);
        components_.push_back(std::move(comp));
        if (store_) {
            store_->register_component(id_, std::type_index(typeid(T)), ptr);
        }
        return ptr;
    }

    template<typename T>
    T* get_component() const {
        static_assert(std::is_base_of_v<components::Component, T>, "T must derive from Component");
        for (const auto& comp : components_) {
            if (auto* ptr = dynamic_cast<T*>(comp.get())) {
                return ptr;
            }
        }
        return nullptr;
    }

    components::Component* add_component(std::unique_ptr<components::Component> comp);
    bool remove_component(components::Component* comp);
    components::Component* get_component_by_type(const std::string& type) const;
    const std::vector<std::unique_ptr<components::Component>>& components() const { return components_; }

    // Transform 快捷访问
    components::Transform* transform() const { return transform_; }

    // 世界变换矩阵（递归乘以父级）
    math::Matrix4f world_transform() const;

    // 遍历（先根后子）
    void foreach(std::function<void(Entity*)> callback);

    // 组件生命周期驱动
    void on_init();
    void on_update(float dt);
    void on_render(render::RenderContext& ctx);
    void on_destroy();

private:
    static ecs::EntityID generate_id();

    std::string name_;
    UUID uuid_;
    ecs::EntityID id_ = ecs::k_invalid_entity;

    Entity* parent_ = nullptr;
    std::vector<std::unique_ptr<Entity>> children_;

    std::vector<std::unique_ptr<components::Component>> components_;
    components::Transform* transform_ = nullptr; // 指向 components_ 中的 Transform

    ecs::ComponentStore* store_ = nullptr;
};

} // namespace gryce_engine::scene
