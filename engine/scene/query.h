#pragma once

#include <functional>
#include <type_traits>
#include <vector>

#include "components/component.h"
#include "ecs/component_store.h"
#include "scene/entity.h"
#include "scene/scene.h"

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// Query — ECS 查询辅助
// 优先使用 ComponentStore 的密集池进行快速遍历；未关联 Scene 时回退到树遍历。
// ---------------------------------------------------------------------------

// 遍历单个 Entity 及其所有后代
inline void foreach_entity(scene::Entity* root, std::function<void(scene::Entity*)> fn) {
    if (!root) return;
    fn(root);
    for (const auto& child : root->children()) {
        foreach_entity(child.get(), fn);
    }
}

// 遍历 Scene 中所有 Entity（所有根及其后代）
inline void foreach_entity(scene::Scene& scene, std::function<void(scene::Entity*)> fn) {
    for (const auto& root : scene.roots()) {
        foreach_entity(root.get(), fn);
    }
}

// 遍历拥有组件 T 的所有 Entity（使用 ComponentStore 密集池）
// 保留 std::function 版本以兼容少量按函数指针传递的调用点；热路径请用下面的
// 模板重载（lambda 按引用直达，零 std::function 构造、零池拷贝）。
template<typename T>
void foreach_with_component(scene::Scene& scene, std::function<void(scene::Entity*, T*)> fn) {
    foreach_with_component<T>(scene, [&fn](scene::Entity* e, T* c) { fn(e, c); });
}

// 遍历同时拥有组件 A 和 B 的所有 Entity（使用较小池做主循环）
//
// 【性能】原实现在模板参数上直接收 std::function，调用方每次传 lambda 都会
// 构造一个 std::function（可能堆分配）；且 pool<A>() / pool<B>() 各自返回一个
// **新构造的 vector**（堆分配 + 逐元素 static_cast）。在 4096 实体的压力场景里
// 这些固定成本叠加后非常可观。
// 现改为：函数模板 + 泛型 F，lambda 按引用直达（零构造/零堆分配），
// 池改为只取"池指针"（pool_ptr）不再拷贝 vector。
//
// 注意保留原有语义：优先用较小的池做主循环；主循环里用 get_component<B>()
// 判存在性 —— 该方法已改为 O(1) 哈希查找（见 Entity::component_index_）。
template<typename A, typename B, typename F>
void foreach_with_components(scene::Scene& scene, F&& fn) {
    static_assert(std::is_base_of_v<components::Component, A>, "A must derive from Component");
    static_assert(std::is_base_of_v<components::Component, B>, "B must derive from Component");

    const auto& store = scene.component_store();
    auto* pool_a = store.pool_ptr<A>();
    auto* pool_b = store.pool_ptr<B>();

    if (pool_a && pool_b && !pool_a->empty() && !pool_b->empty()) {
        // 选较小的池做主循环
        if (pool_a->size() <= pool_b->size()) {
            for (void* raw : *pool_a) {
                auto* ca = static_cast<A*>(raw);
                if (!ca || !ca->enabled) continue;
                scene::Entity* owner = ca->owner();
                if (owner && owner->enabled) {
                    if (auto* cb = owner->get_component<B>()) {
                        fn(owner, ca, cb);
                    }
                }
            }
        } else {
            for (void* raw : *pool_b) {
                auto* cb = static_cast<B*>(raw);
                if (!cb || !cb->enabled) continue;
                scene::Entity* owner = cb->owner();
                if (owner && owner->enabled) {
                    if (auto* ca = owner->get_component<A>()) {
                        fn(owner, ca, cb);
                    }
                }
            }
        }
        return;
    }

    // 回退：树遍历
    foreach_entity(scene, [&](scene::Entity* e) {
        A* ca = e->get_component<A>();
        B* cb = e->get_component<B>();
        if (ca && cb) {
            fn(e, ca, cb);
        }
    });
}

// 单组件版本同样改为模板 + 池指针，避免 std::function 与池拷贝。
template<typename T, typename F>
void foreach_with_component(scene::Scene& scene, F&& fn) {
    static_assert(std::is_base_of_v<components::Component, T>, "T must derive from Component");
    const auto* pool = scene.component_store().pool_ptr<T>();
    if (pool && !pool->empty()) {
        for (void* raw : *pool) {
            auto* comp = static_cast<T*>(raw);
            if (!comp || !comp->enabled) continue;
            scene::Entity* owner = comp->owner();
            if (owner && owner->enabled) fn(owner, comp);
        }
        return;
    }
    foreach_entity(scene, [&](scene::Entity* e) {
        if (auto* comp = e->get_component<T>()) {
            fn(e, comp);
        }
    });
}

} // namespace gryce_engine::ecs
