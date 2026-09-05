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
template<typename T>
void foreach_with_component(scene::Scene& scene, std::function<void(scene::Entity*, T*)> fn) {
    static_assert(std::is_base_of_v<components::Component, T>, "T must derive from Component");
    auto pool = scene.component_store().pool<T>();
    if (!pool.empty()) {
        for (T* comp : pool) {
            if (!comp || !comp->enabled) continue;
            scene::Entity* owner = comp->owner();
            if (owner && owner->enabled) fn(owner, comp);
        }
        return;
    }
    // 回退：存储池为空时使用树遍历（兼容未注册场景）
    foreach_entity(scene, [&](scene::Entity* e) {
        if (auto* comp = e->get_component<T>()) {
            fn(e, comp);
        }
    });
}

// 遍历同时拥有组件 A 和 B 的所有 Entity（使用较小池做主循环）
template<typename A, typename B>
void foreach_with_components(scene::Scene& scene, std::function<void(scene::Entity*, A*, B*)> fn) {
    static_assert(std::is_base_of_v<components::Component, A>, "A must derive from Component");
    static_assert(std::is_base_of_v<components::Component, B>, "B must derive from Component");

    auto pool_a = scene.component_store().pool<A>();
    auto pool_b = scene.component_store().pool<B>();

    if (!pool_a.empty() && !pool_b.empty()) {
        // 选较小的池做主循环
        if (pool_a.size() <= pool_b.size()) {
            for (A* ca : pool_a) {
                if (!ca || !ca->enabled) continue;
                scene::Entity* owner = ca->owner();
                if (owner && owner->enabled) {
                    if (auto* cb = owner->get_component<B>()) {
                        fn(owner, ca, cb);
                    }
                }
            }
        } else {
            for (B* cb : pool_b) {
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

} // namespace gryce_engine::ecs
