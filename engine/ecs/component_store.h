#pragma once

#include "export.h"

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
//   - 记录每个 Entity 拥有的组件，便于运行时销毁
//
// 类型视角池
// ----------
// 池是"按类型视角"分桶的：一个组件实例会同时登记进它自己与所有祖先类型的
// 桶里，桶内指针是该类型视角下的正确子对象地址。因此
//   pool_ptr<Component2D>() 能取到所有 2D 派生组件，
//   pool_ptr<ColorRect>() 只取到 ColorRect，
// 两者拿到的是各自正确的指针（多继承时基类地址与派生地址不同）。
//
// 抽象基类（Component2D / BasicRect）永远不会被直接实例化，但基类池照样有
// 内容 —— 这正是"基类查询"能工作的前提。
//
// 注意：本实现是 ECS 的“稀疏集/按类型分池”简化版，尚未使用 archetype。
// 当前阶段优先满足：快速查询、运行时增删、清晰的生命周期。
// ---------------------------------------------------------------------------
class GRYCE_API ComponentStore {
public:
    ComponentStore() = default;
    ~ComponentStore() = default;

    // 禁止拷贝，允许移动
    ComponentStore(const ComponentStore&) = delete;
    ComponentStore& operator=(const ComponentStore&) = delete;
    ComponentStore(ComponentStore&&) = default;
    ComponentStore& operator=(ComponentStore&&) = default;

    // 注册/反注册组件。注册时按类型视角展开，一个实例会进多个池。
    void register_component(EntityID entity, components::Component* comp);
    void unregister_component(EntityID entity, components::Component* comp);
    void unregister_entity(EntityID entity);

    // 查询某个 Entity 是否拥有指定类型组件（基类语义：派生实例同样算拥有）
    bool has_component(EntityID entity, ComponentTypeID type) const;

    // 获取某类组件的密集指针池（ nullptr 表示该类型从未注册）
    // 池内元素是"该类型视角下"的地址，调用方按目标类型 static_cast 即可。
    const std::vector<void*>* pool(ComponentTypeID type) const;

    // 模板便捷接口
    template<typename T>
    std::vector<T*> pool() const {
        static_assert(std::is_base_of_v<components::Component, T>, "T must derive from Component");
        auto* raw = pool(ComponentTypeID(typeid(T)));
        if (!raw) return {};
        std::vector<T*> result;
        result.reserve(raw->size());
        for (void* c : *raw) {
            result.push_back(static_cast<T*>(c));
        }
        return result;
    }

    // 零拷贝池访问：直接返回内部密集池的指针（未注册返回 nullptr）。
    // 池内元素静态类型是 void*，调用方按 T 做一次 static_cast 即可 ——
    // 池是按 T 的类型视角登记的，指针一定指向某个 T 实例的 T 子对象。
    // 热路径（foreach_with_components 等）用这个替代 pool<T>()，省掉
    // 每次调用一次的 vector 堆分配与逐元素填充。
    // 注意：返回的指针在下次注册/反注册后可能失效，不可跨帧保存。
    template<typename T>
    const std::vector<void*>* pool_ptr() const {
        static_assert(std::is_base_of_v<components::Component, T>, "T must derive from Component");
        return pool(ComponentTypeID(typeid(T)));
    }

    template<typename T>
    bool has_component(EntityID entity) const {
        static_assert(std::is_base_of_v<components::Component, T>, "T must derive from Component");
        return has_component(entity, ComponentTypeID(typeid(T)));
    }

    // 实体 / 池数量查询（统计与监控用）
    std::size_t entity_count() const { return entity_components_.size(); }
    std::size_t pool_count() const { return pools_.size(); }

private:
    // type -> [void*]  密集池（元素是该类型视角下的指针）
    std::unordered_map<ComponentTypeID, std::vector<void*>, std::hash<std::type_index>> pools_;

    // entity -> [Component*]  用于快速反注册
    std::unordered_map<EntityID, std::vector<components::Component*>> entity_components_;
};

} // namespace gryce_engine::ecs
