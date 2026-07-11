#include "ecs/systems/physics_system.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "components/rigid_body.h"
#include "components/static_body.h"
#include "components/box_collider.h"
#include "components/sphere_collider.h"
#include "components/plane_collider.h"
#include "components/transform.h"
#include "components/physical_material.h"
#include "ecs/query.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

namespace {

constexpr math::Vector3f k_gravity(0.0f, -9.81f, 0.0f);
constexpr float k_max_substep_dt = 1.0f / 60.0f;
constexpr float k_slop = 0.005f;
constexpr float k_bias_factor = 0.2f;
constexpr float k_sleep_threshold = 0.05f;
constexpr int k_sleep_frames = 30;
constexpr float k_wake_threshold = 0.01f;
constexpr float k_drag_scale = 3.0f;          // 速度阻尼系数，越大空气阻力越明显（与质量无关）
constexpr float k_mass_scale = 0.001f;        // 密度×体积后缩小，避免质量过大导致重力枪失效

struct AABB {
    math::Vector3f center;
    math::Vector3f half_extents;

    math::Vector3f min() const { return center - half_extents; }
    math::Vector3f max() const { return center + half_extents; }
};

struct Sphere {
    math::Vector3f center;
    float radius = 0.0f;
};

math::Vector3f mul_per_component(const math::Vector3f& a, const math::Vector3f& b) {
    return math::Vector3f(a.x * b.x, a.y * b.y, a.z * b.z);
}

AABB compute_aabb(scene::Entity* entity, components::BoxCollider* collider) {
    auto* t = entity->transform();
    math::Vector3f center = t->position + mul_per_component(collider->center, t->scale);
    math::Vector3f half_extents = mul_per_component(collider->size, t->scale) * 0.5f;
    return { center, half_extents };
}

Sphere compute_sphere(scene::Entity* entity, components::SphereCollider* collider) {
    auto* t = entity->transform();
    math::Vector3f center = t->position + mul_per_component(collider->center, t->scale);
    float scale_max = std::max({ std::abs(t->scale.x), std::abs(t->scale.y), std::abs(t->scale.z) });
    return { center, collider->radius * scale_max };
}

// 根据碰撞体粗略估算体积，用于由密度推导质量
float compute_collider_volume(scene::Entity* entity) {
    auto* t = entity->transform();
    if (!t) return 1.0f;
    if (auto* box = entity->get_component<components::BoxCollider>()) {
        math::Vector3f size = mul_per_component(box->size, t->scale);
        return std::abs(size.x * size.y * size.z);
    }
    if (auto* sphere = entity->get_component<components::SphereCollider>()) {
        float scale_max = std::max({ std::abs(t->scale.x), std::abs(t->scale.y), std::abs(t->scale.z) });
        float r = sphere->radius * scale_max;
        return (4.0f / 3.0f) * 3.14159265f * r * r * r;
    }
    return 1.0f;
}

// 将 PhysicalMaterial 的属性同步到 RigidBody，使材质预设真正影响物理
void apply_physical_material(scene::Entity* entity, components::RigidBody* rb) {
    if (!entity || !rb) return;
    auto* pm = entity->get_component<components::PhysicalMaterial>();
    if (!pm) return;

    float volume = compute_collider_volume(entity);
    rb->mass = std::max(0.001f, pm->density * volume * k_mass_scale);
    rb->restitution = math::clamp(1.0f - pm->softness, 0.0f, 1.0f);
    rb->friction = math::clamp(pm->friction, 0.0f, 1.0f);
}

struct ColliderInfo {
    scene::Entity* entity = nullptr;
    components::BoxCollider* box = nullptr;
    components::SphereCollider* sphere = nullptr;
    components::PlaneCollider* plane = nullptr;
    bool is_static = false;
    float inv_mass = 0.0f; // 0 for static / kinematic
};

std::vector<ColliderInfo> gather_colliders(scene::Scene& scene) {
    std::vector<ColliderInfo> colliders;
    foreach_with_component<components::Transform>(scene, [&](scene::Entity* entity, components::Transform* /*t*/) {
        auto* box = entity->get_component<components::BoxCollider>();
        auto* sphere = entity->get_component<components::SphereCollider>();
        auto* plane = entity->get_component<components::PlaneCollider>();
        if (!box && !sphere && !plane) return;

        ColliderInfo info;
        info.entity = entity;
        info.box = box;
        info.sphere = sphere;
        info.plane = plane;

        auto* rb = entity->get_component<components::RigidBody>();
        auto* sb = entity->get_component<components::StaticBody>();
        if (sb || (rb && rb->is_kinematic)) {
            info.is_static = true;
            info.inv_mass = 0.0f;
        } else if (rb) {
            info.is_static = false;
            info.inv_mass = rb->mass > 0.0f ? 1.0f / rb->mass : 0.0f;
        } else {
            // 没有 RigidBody/StaticBody 的碰撞体视为静态
            info.is_static = true;
            info.inv_mass = 0.0f;
        }
        colliders.push_back(info);
    });
    return colliders;
}

// 返回法线方向为 A -> B
bool aabb_vs_aabb(const AABB& a, const AABB& b, math::Vector3f& out_normal, float& out_penetration) {
    math::Vector3f n = b.center - a.center;
    float overlap_x = a.half_extents.x + b.half_extents.x - std::abs(n.x);
    if (overlap_x <= 0.0f) return false;
    float overlap_y = a.half_extents.y + b.half_extents.y - std::abs(n.y);
    if (overlap_y <= 0.0f) return false;
    float overlap_z = a.half_extents.z + b.half_extents.z - std::abs(n.z);
    if (overlap_z <= 0.0f) return false;

    // 找到最小穿透轴
    if (overlap_x < overlap_y && overlap_x < overlap_z) {
        out_penetration = overlap_x;
        out_normal = math::Vector3f(n.x > 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
    } else if (overlap_y < overlap_z) {
        out_penetration = overlap_y;
        out_normal = math::Vector3f(0.0f, n.y > 0.0f ? 1.0f : -1.0f, 0.0f);
    } else {
        out_penetration = overlap_z;
        out_normal = math::Vector3f(0.0f, 0.0f, n.z > 0.0f ? 1.0f : -1.0f);
    }
    return true;
}

// 返回法线方向为 A -> B
bool sphere_vs_sphere(const Sphere& a, const Sphere& b, math::Vector3f& out_normal, float& out_penetration) {
    math::Vector3f n = b.center - a.center;
    float dist_sq = n.length_sq();
    float radius_sum = a.radius + b.radius;
    if (dist_sq > radius_sum * radius_sum) return false;

    float dist = std::sqrt(dist_sq);
    if (dist < 1e-6f) {
        // 球心重合，默认沿 Y 轴分离
        out_normal = math::Vector3f::up();
        out_penetration = radius_sum;
        return true;
    }
    out_normal = n / dist;
    out_penetration = radius_sum - dist;
    return true;
}

// sphere 为 A，box 为 B；返回法线方向为 A -> B
bool sphere_vs_aabb(const Sphere& sphere, const AABB& box, math::Vector3f& out_normal, float& out_penetration) {
    math::Vector3f closest = sphere.center.clamp(box.min(), box.max());
    math::Vector3f diff = closest - sphere.center; // A -> B 方向（球心到盒子上最近点）
    float dist_sq = diff.length_sq();
    float radius_sq = sphere.radius * sphere.radius;

    if (dist_sq > radius_sq) return false;

    if (dist_sq < 1e-12f) {
        // 球心在盒子内部：选择穿透最小的轴
        math::Vector3f to_center = sphere.center - box.center;
        float px = box.half_extents.x - std::abs(to_center.x);
        float py = box.half_extents.y - std::abs(to_center.y);
        float pz = box.half_extents.z - std::abs(to_center.z);
        if (px <= py && px <= pz) {
            out_penetration = px;
            out_normal = math::Vector3f(to_center.x > 0.0f ? -1.0f : 1.0f, 0.0f, 0.0f);
        } else if (py <= pz) {
            out_penetration = py;
            out_normal = math::Vector3f(0.0f, to_center.y > 0.0f ? -1.0f : 1.0f, 0.0f);
        } else {
            out_penetration = pz;
            out_normal = math::Vector3f(0.0f, 0.0f, to_center.z > 0.0f ? -1.0f : 1.0f);
        }
        return true;
    }

    float dist = std::sqrt(dist_sq);
    out_penetration = sphere.radius - dist;
    out_normal = diff / dist;
    return true;
}

void compute_world_plane(const components::Transform* t,
                         const components::PlaneCollider* plane,
                         math::Vector3f& out_normal, float& out_offset) {
    out_normal = t->rotation.rotate_vector(plane->normal).normalized();
    math::Vector3f plane_point = t->position + out_normal * plane->offset;
    out_offset = -out_normal.dot(plane_point);
}

// sphere 为 A，plane 为 B；法线方向为 A -> B（从球指向平面，与平面法向相反）
bool sphere_vs_plane(const Sphere& sphere, const math::Vector3f& plane_normal, float plane_offset,
                     math::Vector3f& out_normal, float& out_penetration) {
    float signed_dist = plane_normal.dot(sphere.center) + plane_offset;
    if (signed_dist >= sphere.radius) return false;
    out_penetration = sphere.radius - signed_dist;
    out_normal = plane_normal * -1.0f;
    return true;
}

// box 为 A，plane 为 B；法线方向为 A -> B（与平面法向相反）
bool aabb_vs_plane(const AABB& box, const math::Vector3f& plane_normal, float plane_offset,
                   math::Vector3f& out_normal, float& out_penetration) {
    // 枚举 AABB 的 8 个角点，找最深穿透点
    float min_dist = std::numeric_limits<float>::max();
    math::Vector3f deepest_corner = box.center;

    for (int ix = 0; ix < 2; ++ix) {
        float x = (ix == 0) ? box.min().x : box.max().x;
        for (int iy = 0; iy < 2; ++iy) {
            float y = (iy == 0) ? box.min().y : box.max().y;
            for (int iz = 0; iz < 2; ++iz) {
                float z = (iz == 0) ? box.min().z : box.max().z;
                math::Vector3f corner(x, y, z);
                float dist = plane_normal.dot(corner) + plane_offset;
                if (dist < min_dist) {
                    min_dist = dist;
                    deepest_corner = corner;
                }
            }
        }
    }

    if (min_dist >= 0.0f) return false;
    out_penetration = -min_dist;
    out_normal = plane_normal * -1.0f;
    return true;
}

bool detect_collision(const ColliderInfo& info_a, const ColliderInfo& info_b,
                      math::Vector3f& out_normal, float& out_penetration) {
    // 处理平面碰撞体：A 为动态，B 为平面
    if (info_b.plane) {
        math::Vector3f plane_normal;
        float plane_offset;
        compute_world_plane(info_b.entity->transform(), info_b.plane, plane_normal, plane_offset);
        if (info_a.box) {
            return aabb_vs_plane(compute_aabb(info_a.entity, info_a.box), plane_normal, plane_offset,
                                 out_normal, out_penetration);
        }
        if (info_a.sphere) {
            return sphere_vs_plane(compute_sphere(info_a.entity, info_a.sphere), plane_normal, plane_offset,
                                   out_normal, out_penetration);
        }
        return false;
    }
    if (info_a.plane) {
        // 交换，让平面始终在 B
        bool hit = detect_collision(info_b, info_a, out_normal, out_penetration);
        if (hit) {
            out_normal = out_normal * -1.0f;
        }
        return hit;
    }

    if (info_a.box && info_b.box) {
        return aabb_vs_aabb(compute_aabb(info_a.entity, info_a.box),
                            compute_aabb(info_b.entity, info_b.box),
                            out_normal, out_penetration);
    }
    if (info_a.sphere && info_b.sphere) {
        return sphere_vs_sphere(compute_sphere(info_a.entity, info_a.sphere),
                                compute_sphere(info_b.entity, info_b.sphere),
                                out_normal, out_penetration);
    }
    if (info_a.sphere && info_b.box) {
        return sphere_vs_aabb(compute_sphere(info_a.entity, info_a.sphere),
                              compute_aabb(info_b.entity, info_b.box),
                              out_normal, out_penetration);
    }
    if (info_a.box && info_b.sphere) {
        bool hit = sphere_vs_aabb(compute_sphere(info_b.entity, info_b.sphere),
                                  compute_aabb(info_a.entity, info_a.box),
                                  out_normal, out_penetration);
        if (hit) {
            out_normal = out_normal * -1.0f; // 转为 A -> B
        }
        return hit;
    }
    return false;
}

void resolve_collision_pair(const ColliderInfo& info_a, const ColliderInfo& info_b,
                            const math::Vector3f& normal, float penetration) {
    // normal 方向为 A -> B
    auto* rb_a = info_a.entity->get_component<components::RigidBody>();
    auto* rb_b = info_b.entity->get_component<components::RigidBody>();
    auto* t_a = info_a.entity->transform();
    auto* t_b = info_b.entity->transform();
    if (!t_a || !t_b) return;

    float inv_mass_a = (info_a.is_static || !rb_a || rb_a->is_kinematic || rb_a->mass <= 0.0f)
                       ? 0.0f : 1.0f / rb_a->mass;
    float inv_mass_b = (info_b.is_static || !rb_b || rb_b->is_kinematic || rb_b->mass <= 0.0f)
                       ? 0.0f : 1.0f / rb_b->mass;

    float total_inv_mass = inv_mass_a + inv_mass_b;
    if (total_inv_mass <= 0.0f) return;

    // 触发器不参与碰撞响应
    if ((info_a.box && info_a.box->is_trigger) ||
        (info_a.sphere && info_a.sphere->is_trigger) ||
        (info_a.plane && info_a.plane->is_trigger) ||
        (info_b.box && info_b.box->is_trigger) ||
        (info_b.sphere && info_b.sphere->is_trigger) ||
        (info_b.plane && info_b.plane->is_trigger)) {
        return;
    }

    // 位置修正（带 slop，防止下沉/抖动）
    float correction = std::max(0.0f, penetration - k_slop) * k_bias_factor;
    t_a->position -= normal * correction * (inv_mass_a / total_inv_mass);
    t_b->position += normal * correction * (inv_mass_b / total_inv_mass);

    if (!rb_a && !rb_b) return;

    // 相对速度沿法线方向（A -> B）
    math::Vector3f relative_vel = math::Vector3f::zero();
    if (rb_a) relative_vel += rb_a->velocity;
    if (rb_b) relative_vel -= rb_b->velocity;
    float vel_along_normal = relative_vel.dot(normal);
    if (vel_along_normal <= 0.0f) return; // 分离或静止

    // 恢复系数限制在 [0, 1]，避免能量增加
    float restitution = rb_a ? math::clamp(rb_a->restitution, 0.0f, 1.0f) : 0.0f;
    if (rb_b) {
        restitution = std::min(restitution, math::clamp(rb_b->restitution, 0.0f, 1.0f));
    }

    float impulse = -(1.0f + restitution) * vel_along_normal;
    impulse /= total_inv_mass;

    math::Vector3f impulse_vec = normal * impulse;
    if (inv_mass_a > 0.0f && rb_a) {
        rb_a->velocity += impulse_vec * inv_mass_a;
    }
    if (inv_mass_b > 0.0f && rb_b) {
        rb_b->velocity -= impulse_vec * inv_mass_b;
    }

    // 简化摩擦：削弱相对切向速度
    math::Vector3f tangent = relative_vel - normal * vel_along_normal;
    float tangent_len_sq = tangent.length_sq();
    if (tangent_len_sq > 1e-6f) {
        tangent = tangent / std::sqrt(tangent_len_sq);
        float friction = rb_a ? math::clamp(rb_a->friction, 0.0f, 1.0f) : 0.0f;
        if (rb_b) {
            friction = std::min(friction, math::clamp(rb_b->friction, 0.0f, 1.0f));
        }
        math::Vector3f friction_impulse = tangent * impulse * friction;
        if (inv_mass_a > 0.0f && rb_a) {
            rb_a->velocity -= friction_impulse * inv_mass_a;
        }
        if (inv_mass_b > 0.0f && rb_b) {
            rb_b->velocity += friction_impulse * inv_mass_b;
        }
    }

    // 碰撞唤醒
    if (std::abs(impulse) > k_wake_threshold) {
        if (rb_a) { rb_a->is_sleeping = false; rb_a->sleep_frames = 0; }
        if (rb_b) { rb_b->is_sleeping = false; rb_b->sleep_frames = 0; }
    }

    // 记录上一帧碰撞冲量（取最大值）
    if (rb_a) {
        rb_a->last_collision_impulse = std::max(rb_a->last_collision_impulse, std::abs(impulse));
    }
    if (rb_b) {
        rb_b->last_collision_impulse = std::max(rb_b->last_collision_impulse, std::abs(impulse));
    }
}

} // namespace

void PhysicsSystem::on_update(scene::Scene& scene, float dt) {
    if (dt <= 0.0f) return;

    // 基本连续碰撞检测：将大步长拆分为最多 1/60s 的子步
    int substeps = std::max(1, static_cast<int>(dt / k_max_substep_dt));
    float sub_dt = dt / static_cast<float>(substeps);

    // 每帧开始时重置碰撞冲量记录
    foreach_with_component<components::RigidBody>(scene, [&](scene::Entity* /*entity*/, components::RigidBody* rb) {
        if (rb) rb->last_collision_impulse = 0.0f;
    });

    for (int step = 0; step < substeps; ++step) {
        // 1. 收集所有碰撞体（位置每子步可能变化）
        std::vector<ColliderInfo> colliders = gather_colliders(scene);

        // 2. 对 RigidBody 施加重力、空气阻力并积分
        foreach_with_component<components::RigidBody>(scene, [&](scene::Entity* entity, components::RigidBody* rb) {
            if (!rb || !rb->enabled || rb->is_kinematic || rb->is_sleeping) return;
            auto* t = entity->transform();
            if (!t) return;

            // 同步 PhysicalMaterial 到 RigidBody（质量、弹性、摩擦）
            apply_physical_material(entity, rb);

            math::Vector3f acc = rb->acceleration;
            if (rb->use_gravity) {
                acc += k_gravity;
            }

            rb->velocity += acc * sub_dt;

            // 空气阻力：按速度比例衰减，与质量无关，drag_coefficient 越大减速越快
            auto* pm = entity->get_component<components::PhysicalMaterial>();
            if (pm && pm->drag_coefficient > 0.0f) {
                float damping = pm->drag_coefficient * k_drag_scale * sub_dt;
                rb->velocity *= (1.0f - math::clamp(damping, 0.0f, 0.99f));
            }

            t->position += rb->velocity * sub_dt;
        });

        // 3. 碰撞检测与响应（每对只处理一次）
        for (size_t i = 0; i < colliders.size(); ++i) {
            const auto& info_a = colliders[i];
            if (!info_a.box && !info_a.sphere && !info_a.plane) continue;

            for (size_t j = i + 1; j < colliders.size(); ++j) {
                const auto& info_b = colliders[j];
                if (!info_b.box && !info_b.sphere && !info_b.plane) continue;

                math::Vector3f normal;
                float penetration = 0.0f;
                if (detect_collision(info_a, info_b, normal, penetration)) {
                    resolve_collision_pair(info_a, info_b, normal, penetration);
                }
            }
        }
    }

    // 4. 应用线性阻尼并清空加速度
    foreach_with_component<components::RigidBody>(scene, [&](scene::Entity* /*entity*/, components::RigidBody* rb) {
        if (!rb || rb->is_kinematic) return;

        rb->velocity *= (1.0f - rb->linear_damping);
        rb->acceleration = math::Vector3f::zero();

        // 睡眠计数（按帧计，而非子步）
        if (rb->is_sleeping) return;
        if (rb->velocity.length() < k_sleep_threshold) {
            rb->sleep_frames++;
            if (rb->sleep_frames >= k_sleep_frames) {
                rb->is_sleeping = true;
                rb->velocity = math::Vector3f::zero();
                rb->sleep_frames = 0;
            }
        } else {
            rb->sleep_frames = 0;
        }
    });
}

} // namespace gryce_engine::ecs
