#pragma once

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include <type_traits>
#include <typeindex>
#include <unordered_map>

#include "export.h"
#include "components/component.h"
#include "components/transform.h"
#include "components/hierarchy_components.h"
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
class GRYCE_API Entity {
public:
    explicit Entity(const std::string& name = "Entity");

    // ECS 唯一标识（运行时分配，与序列化用的 UUID 分离）
    ecs::EntityID id() const { return id_; }

    // 关联组件存储池（由 Scene 设置）。设置时会将已存在的组件注册进存储池。
    void set_store(ecs::ComponentStore* store);
    ecs::ComponentStore* store() const { return store_; }
    ~Entity();

    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;
    Entity(Entity&&) = default;
    Entity& operator=(Entity&&) = default;

    // Dirty tracking for delta save / hot reload
    void mark_dirty() { dirty_ = true; }
    bool is_dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    // 标识
    const std::string& name() const { return name_; }
    void set_name(const std::string& name) { name_ = name; mark_dirty(); }

    const UUID& uuid() const { return uuid_; }
    void set_uuid(const UUID& id) { uuid_ = id; }

    // Prefab 模板成员标记：非空表示本实体是某个 Prefab 实例展开出的模板成员，
    // 值为该实体在预制体文件中的模板 UUID（实例根本身为空串）。
    // 场景序列化据此把实例写成紧凑的 prefab 引用形式。
    const std::string& prefab_template_uuid() const { return prefab_template_uuid_; }
    void set_prefab_template_uuid(const std::string& id) { prefab_template_uuid_ = id; }

    bool enabled = true; // Entity 级开关，影响组件 update/render 及查询

    // 层级
    Entity* parent() const { return parent_; }
    void set_parent(Entity* parent);

    Entity* add_child(std::unique_ptr<Entity> child);
    // 在指定位置插入子实体（index 越界时追加到末尾）
    Entity* insert_child(std::unique_ptr<Entity> child, size_t index);
    bool remove_child(Entity* child);
    // 把子实体从层级中摘下并移交所有权（不销毁，不触发组件反注册）
    std::unique_ptr<Entity> detach_child(Entity* child);
    // 摘下全部子实体并移交所有权（本实体的 children 被清空）
    std::vector<std::unique_ptr<Entity>> detach_all_children();
    // 把 other 的全部子实体搬移为本实体的子实体（other 被清空，不转移 store）
    void adopt_children_of(Entity& other);
    const std::vector<std::unique_ptr<Entity>>& children() const { return children_; }

    // 组件
    template<typename T, typename... Args>
    T* add_component(Args&&... args) {
        static_assert(std::is_base_of_v<components::Component, T>, "T must derive from Component");
        auto comp = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = comp.get();
        ptr->on_attach(this);
        components_.push_back(std::move(comp));
        index_component(ptr);
        if (store_) {
            store_->register_component(id_, ptr);
        }
        // 生命周期：awake（组件已挂载但场景尚未开始）。
        // 与 add_component(unique_ptr) 保持一致 —— 模板版过去漏了这一步，
        // 导致 Timer / ScriptComponent 通过模板挂载时收不到 on_awake。
        ptr->on_awake();
        mark_dirty();
        return ptr;
    }

    template<typename T>
    T* get_component() const {
        static_assert(std::is_base_of_v<components::Component, T>, "T must derive from Component");
        // O(1) 哈希查找。索引是"类型视角索引"：每个组件实例向它的所有祖先
        // 类型各登记一份该祖先视角下的指针，所以基类查询与精确查询都命中
        // 同一张表。原实现是"线性扫描 + dynamic_cast"（支持基类，但慢），
        // 中途被改成 typeid 精确哈希（快，但丢了基类语义 —— Component2D
        // 是抽象类，精确查询恒空，2D 渲染系统因此收集不到任何组件）。
        auto it = component_index_.find(std::type_index(typeid(T)));
        if (it == component_index_.end() || it->second.empty()) {
            return nullptr;
        }
        return static_cast<T*>(it->second.front());
    }

    // 类型擦除的组件查找：按类型索引取"该类型视角下"的组件指针。
    // 基类语义与 get_component<T>() 一致；供测试与工具（无法写模板的场合）使用。
    void* find_component_view(std::type_index type) const {
        auto it = component_index_.find(type);
        if (it == component_index_.end() || it->second.empty()) {
            return nullptr;
        }
        return it->second.front();
    }

    // 指定类型视角下的组件个数（同类型多实例计数；统计与测试用）
    std::size_t component_view_count(std::type_index type) const {
        auto it = component_index_.find(type);
        return it == component_index_.end() ? 0 : it->second.size();
    }

    // 指定类型视角下的第 index 个组件指针（按挂载顺序；越界返回 nullptr）
    void* component_view_at(std::type_index type, std::size_t index) const {
        auto it = component_index_.find(type);
        if (it == component_index_.end() || index >= it->second.size()) {
            return nullptr;
        }
        return it->second[index];
    }

    components::Component* add_component(std::unique_ptr<components::Component> comp);
    bool remove_component(components::Component* comp);
    components::Component* get_component_by_type(const std::string& type) const;
    const std::vector<std::unique_ptr<components::Component>>& components() const { return components_; }

    // -----------------------------------------------------------------------
    // 用户可见组件序列（过滤引擎内部组件）
    //
    // Transform / ParentComponent / ChildrenComponent 是每个实体都有的内部
    // 组件，对外（编辑器 Inspector、脚本层）没有意义。C API 的组件下标与
    // 脚本层的组件下标都必须以本序列为准 —— 两边共用同一个辅助函数，
    // 否则「按下标取组件」在两侧会错位（编辑器取到 A、脚本取到 B）。
    //
    // 实现放在头文件里：脚本测试目标只编被测源码、不链实体实现，
    // 脚本绑定层要按同一口径取组件，头文件内联实现免掉这条链接依赖。
    // -----------------------------------------------------------------------
    std::vector<components::Component*> user_components() const {
        std::vector<components::Component*> out;
        out.reserve(components_.size());
        for (const auto& comp : components_) {
            if (dynamic_cast<components::Transform*>(comp.get())) continue;
            if (dynamic_cast<components::ParentComponent*>(comp.get())) continue;
            if (dynamic_cast<components::ChildrenComponent*>(comp.get())) continue;
            out.push_back(comp.get());
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // 组件树（组件实例之间的父子挂载关系）
    //
    // 与"基类 / 子类"正交：基类/子类说的是类型继承，父/子组件说的是实例挂载。
    // 一个 ColorRect 既可以是 BasicRect 的子类，也可以是某个 Sprite2D 的子组件。
    // -----------------------------------------------------------------------

    // 组件树根（没有父组件的组件），按挂载顺序
    std::vector<components::Component*> component_roots() const;

    // 深度优先遍历整棵组件树（先根后子；同一实体内的所有组件树）
    void foreach_component_tree(std::function<void(components::Component*)> callback) const;

    // 改挂：把 child 挂到 parent 之下（parent 传空表示让 child 成为组件树根）。
    // child 与 parent 必须都属于本实体；防环由 Component::set_component_parent 兜底。
    // 头文件内联实现（理由同 user_components）。
    bool set_component_parent(components::Component* child, components::Component* parent) {
        if (!child) return false;
        if (child->owner() != this) return false;
        if (parent && parent->owner() != this) return false;
        return child->set_component_parent(parent);
    }

    // 组件在组件树中的相对变换（父链局部矩阵连乘，不含实体自身的变换）
    math::Matrix4f component_relative_transform(const components::Component* comp) const;

    // 组件的世界变换 = 实体世界变换 × 组件树相对变换
    math::Matrix4f component_world_transform(const components::Component* comp) const;

    // Transform 快捷访问
    components::Transform* transform() const { return transform_; }

    // ECS 层级组件快捷访问（用于系统查询）
    components::ParentComponent* parent_component() const { return parent_comp_; }
    components::ChildrenComponent* children_component() const { return children_comp_; }

    // 世界变换矩阵（递归乘以父级）
    math::Matrix4f world_transform() const;

    // 遍历（先根后子）
    void foreach(std::function<void(Entity*)> callback);

    // 组件生命周期驱动
    void on_init();
    void on_start();
    void on_enable();
    void on_disable();
    void on_update(float dt);
    void on_render(render::RenderContext& ctx);
    void on_destroy();

    // 深拷贝自身（生成新 UUID 和新 EntityID），不关联任何 store/parent
    std::unique_ptr<Entity> clone() const;

    // 运行时状态快照（用于热重载时保留）
    nlohmann::json snapshot_runtime_state() const;
    void restore_runtime_state(const nlohmann::json& json);

private:
    static ecs::EntityID generate_id();

    // 组件类型视角索引：类型 -> 该类型视角下的组件指针列表。
    // 每个组件实例向它的所有祖先类型各登记一份指针，因此：
    //   - get_component<Derived>() 与 get_component<Base>() 命中同一实例；
    //   - 拿到的是该类型正确的子对象地址（多继承时各基类地址不同）；
    //   - 列表而非单值 → 同类型多实例不再互相覆盖。
    std::unordered_map<std::type_index, std::vector<void*>> component_index_;

    // 索引维护：挂载时登记全部类型视角，卸载时按指针精确擦除。
    void index_component(components::Component* comp);
    void unindex_component(components::Component* comp);

    std::string name_;
    UUID uuid_;
    ecs::EntityID id_ = ecs::k_invalid_entity;
    std::string prefab_template_uuid_; // 见 prefab_template_uuid() 注释

    Entity* parent_ = nullptr;
    std::vector<std::unique_ptr<Entity>> children_;

    std::vector<std::unique_ptr<components::Component>> components_;
    components::Transform* transform_ = nullptr; // 指向 components_ 中的 Transform
    components::ParentComponent* parent_comp_ = nullptr; // 指向 components_ 中的 ParentComponent
    components::ChildrenComponent* children_comp_ = nullptr; // 指向 components_ 中的 ChildrenComponent

    ecs::ComponentStore* store_ = nullptr;

    bool dirty_ = false; // 标记自上次保存后是否变更
    bool destroy_notified_ = false; // on_destroy 幂等保护：析构链与显式调用只生效一次
};

} // namespace gryce_engine::scene
