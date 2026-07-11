#pragma once

#include <typeindex>
#include <unordered_map>
#include <vector>

#include "ecs/types.h"

namespace gryce_engine::components {
    class Component;
} // namespace gryce_engine::components

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// ComponentStore — ECS 组件存储池
//
// 提供按组件类型快速查询的能力：
//   - 每个组件类型对应一个密集指针数组（cache-friendly 遍历）
//   - 记录每个 Entity 拥有的组件类型，便于运行时销毁
//
// 注意：本实现是 ECS 的“稀疏集/按类型分池”简化版，尚未使用 archetype。
// 当前阶段优先满足：快速查询、运行时增删、清晰的生命周期。
// ---------------------------------------------------------------------------
class ComponentStore {
public:
    ComponentStore() = default;
    ~ComponentStore() = default;

    // 禁止拷贝，允许移动
    ComponentStore(const ComponentStore&) = delete;
    ComponentStore& operator=(const ComponentStore&) = delete;
    ComponentStore(ComponentStore&&) = default;
    ComponentStore& operator=(ComponentStore&&) = default;

    // 注册/反注册组件
    void register_component(EntityID entity, ComponentTypeID type, components::Component* comp);
    void unregister_component(EntityID entity, ComponentTypeID type);
    void unregister_entity(EntityID entity);

    // 查询某个 Entity 是否拥有指定类型组件
    bool has_component(EntityID entity, ComponentTypeID type) const;

    // 获取某类组件的密集指针池（ nullptr 表示该类型从未注册）
    const std::vector<components::Component*>* pool(ComponentTypeID type) const;

    // 模板便捷接口
    template<typename T>
    std::vector<T*> pool() const {
        static_assert(std::is_base_of_v<components::Component, T>, "T must derive from Component");
        auto* raw = pool(ComponentTypeID(typeid(T)));
        if (!raw) return {};
        std::vector<T*> result;
        result.reserve(raw->size());
        for (auto* c : *raw) {
            result.push_back(static_cast<T*>(c));
        }
        return result;
    }

    template<typename T>
    bool has_component(EntityID entity) const {
        static_assert(std::is_base_of_v<components::Component, T>, "T must derive from Component");
        return has_component(entity, ComponentTypeID(typeid(T)));
    }

    // 调试/统计
    std::size_t entity_count() const { return entity_components_.size(); }
    std::size_t pool_count() const { return pools_.size(); }

private:
    // type -> [Component*]  密集池
    std::unordered_map<ComponentTypeID, std::vector<components::Component*>, std::hash<std::type_index>> pools_;

    // entity -> [ComponentTypeID]  用于快速反注册
    std::unordered_map<EntityID, std::vector<ComponentTypeID>> entity_components_;
};

} // namespace gryce_engine::ecs
