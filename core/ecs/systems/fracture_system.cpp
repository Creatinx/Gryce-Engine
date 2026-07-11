#include "ecs/systems/fracture_system.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "components/destructible_body.h"
#include "components/fragment_body.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/static_body.h"
#include "components/transform.h"
#include "components/mesh_renderer.h"
#include "ecs/query.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

namespace {

math::Vector3f mul_per_component(const math::Vector3f& a, const math::Vector3f& b) {
    return math::Vector3f(a.x * b.x, a.y * b.y, a.z * b.z);
}

} // namespace

void FractureSystem::on_update(scene::Scene& scene, float dt) {
    if (dt <= 0.0f) return;

    std::vector<scene::Entity*> to_destroy;

    // 1. 处理可碎裂 Entity
    foreach_with_components<components::DestructibleBody, components::BoxCollider>(scene,
        [&](scene::Entity* entity, components::DestructibleBody* destructible, components::BoxCollider* collider) {
            if (!destructible || !collider || destructible->fractured) return;

            auto* rb = entity->get_component<components::RigidBody>();
            if (!rb) return;

            if (rb->last_collision_impulse < destructible->fracture_threshold) return;

            destructible->fractured = true;
            to_destroy.push_back(entity);

            auto* t = entity->transform();
            if (!t) return;

            // 计算碎片尺寸
            math::Vector3f world_size = mul_per_component(collider->size, t->scale);
            math::Vector3f world_center = t->position + mul_per_component(collider->center, t->scale);

            int sx = std::max(1, destructible->segments.x);
            int sy = std::max(1, destructible->segments.y);
            int sz = std::max(1, destructible->segments.z);
            int total_fragments = sx * sy * sz;
            if (total_fragments > destructible->max_fragments) {
                // 等比例缩小分段数以满足上限
                float scale = std::cbrt(static_cast<float>(destructible->max_fragments) / static_cast<float>(total_fragments));
                sx = std::max(1, static_cast<int>(sx * scale));
                sy = std::max(1, static_cast<int>(sy * scale));
                sz = std::max(1, static_cast<int>(sz * scale));
                total_fragments = sx * sy * sz;
            }

            math::Vector3f fragment_size(world_size.x / sx, world_size.y / sy, world_size.z / sz);
            float fragment_mass = rb->mass / static_cast<float>(total_fragments);

            // 生成碎片
            for (int ix = 0; ix < sx; ++ix) {
                for (int iy = 0; iy < sy; ++iy) {
                    for (int iz = 0; iz < sz; ++iz) {
                        // 局部归一化中心 [-0.5, 0.5]
                        float nx = (ix + 0.5f) / sx - 0.5f;
                        float ny = (iy + 0.5f) / sy - 0.5f;
                        float nz = (iz + 0.5f) / sz - 0.5f;

                        // 局部偏移
                        math::Vector3f local_offset(
                            nx * world_size.x,
                            ny * world_size.y,
                            nz * world_size.z
                        );

                        // 世界位置
                        math::Vector3f frag_pos = world_center + t->rotation.rotate_vector(local_offset);

                        scene::Entity* frag = scene.create_entity("Fragment");
                        frag->transform()->position = frag_pos;
                        frag->transform()->rotation = t->rotation;
                        frag->transform()->scale = math::Vector3f(
                            fragment_size.x / collider->size.x,
                            fragment_size.y / collider->size.y,
                            fragment_size.z / collider->size.z
                        );

                        auto* frag_rb = frag->add_component<components::RigidBody>();
                        frag_rb->mass = fragment_mass;
                        frag_rb->velocity = rb->velocity;
                        frag_rb->use_gravity = rb->use_gravity;
                        frag_rb->restitution = rb->restitution;
                        frag_rb->friction = rb->friction;

                        // 爆炸冲量：从原中心指向碎片中心
                        math::Vector3f dir = frag_pos - world_center;
                        if (dir.length_sq() > 1e-12f) {
                            dir = dir.normalized();
                        } else {
                            dir = math::Vector3f::up();
                        }
                        frag_rb->velocity += dir * destructible->explosive_impulse;

                        auto* frag_col = frag->add_component<components::BoxCollider>();
                        frag_col->size = collider->size;

                        if (destructible->fragment_lifetime > 0.0f) {
                            frag->add_component<components::FragmentBody>(destructible->fragment_lifetime);
                        }

                        // 复制 MeshRenderer / Material 以渲染碎片
                        auto* src_mr = entity->get_component<components::MeshRenderer>();
                        if (src_mr) {
                            auto* frag_mr = frag->add_component<components::MeshRenderer>(src_mr->mesh_path);
                            if (frag_mr && src_mr->material) {
                                nlohmann::json mat_json;
                                src_mr->material->serialize(mat_json);
                                frag_mr->ensure_material();
                                frag_mr->material->deserialize(mat_json);
                            }
                        }
                    }
                }
            }

            GLOG_INFO("FractureSystem: entity '{}' fractured into {} fragments (impulse={:.2f})",
                      entity->name(), total_fragments, rb->last_collision_impulse);
        });

    // 2. 删除已碎裂原 Entity
    for (scene::Entity* e : to_destroy) {
        scene.destroy_entity(e);
    }

    // 3. 更新碎片生命周期并清理过期碎片
    std::vector<scene::Entity*> expired_fragments;
    foreach_with_component<components::FragmentBody>(scene, [&](scene::Entity* entity, components::FragmentBody* frag) {
        if (!frag || frag->lifetime <= 0.0f) return;
        frag->time_alive += dt;
        if (frag->time_alive >= frag->lifetime) {
            expired_fragments.push_back(entity);
        }
    });

    for (scene::Entity* e : expired_fragments) {
        scene.destroy_entity(e);
    }
}

} // namespace gryce_engine::ecs
