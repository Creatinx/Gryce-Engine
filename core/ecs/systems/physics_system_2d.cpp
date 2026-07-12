#include "ecs/systems/physics_system_2d.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "components/rigid_body_2d.h"
#include "components/static_body_2d.h"
#include "components/box_collider_2d.h"
#include "components/circle_collider_2d.h"
#include "components/transform.h"
#include "ecs/query.h"
#include "physics/physics_factory.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

namespace {

constexpr float k_pi = 3.14159265358979323846f;
constexpr float k_sleep_threshold = 0.05f;
constexpr int k_sleep_frames = 30;

float quat_to_z(const math::Quaternionf& q) {
    // 从四元数提取绕 Z 轴的旋转角
    return std::atan2(2.0f * (q.w * q.z + q.x * q.y),
                      1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}

math::Quaternionf z_to_quat(float angle) {
    return math::Quaternionf::from_axis_angle(math::Vector3f(0.0f, 0.0f, 1.0f), angle);
}

float compute_density_for_box(float mass, const math::Vector2f& half_extents) {
    float area = 4.0f * half_extents.x * half_extents.y;
    if (area <= 1e-6f) return 1.0f;
    return mass / area;
}

float compute_density_for_circle(float mass, float radius) {
    float area = k_pi * radius * radius;
    if (area <= 1e-6f) return 1.0f;
    return mass / area;
}

} // namespace

// ---------------------------------------------------------------------------
// PhysicsSystem2D 内部实现
// ---------------------------------------------------------------------------
struct PhysicsSystem2D::Impl {
    std::unique_ptr<physics::IPhysicsWorld2D> world;

    struct Slot {
        scene::Entity* entity = nullptr;
        physics::BodyHandle body = physics::k_invalid_body;
        std::vector<physics::ShapeHandle> shapes;

        // 用于检测 collider/transform 变化
        math::Vector2f last_position;
        float last_angle = 0.0f;
        math::Vector2f last_scale{1.0f, 1.0f};

        math::Vector2f last_box_size;
        math::Vector2f last_box_center;
        bool has_box = false;

        float last_circle_radius = 0.0f;
        math::Vector2f last_circle_center;
        bool has_circle = false;

        bool seen_this_frame = false;
    };

    std::unordered_map<scene::UUID, Slot> slots;
    bool initialized = false;
    bool create_failed = false;

    // Box2D 等外部后端要求每帧使用固定小步长，不能一次性 step 整个 dt
    float time_accumulator = 0.0f;

    physics::IPhysicsWorld2D* ensure_world(const math::Vector2f& gravity) {
        if (create_failed) return nullptr;
        if (!world) {
            world = physics::create_physics_world_2d("box2d");
            if (!world || !world->init(gravity)) {
                GLOG_ERROR("PhysicsSystem2D: failed to create 2D physics world (Box2D). "
                           "Pass -DGRYCE_FETCH_BOX2D=ON.");
                create_failed = true;
                return nullptr;
            }
            GLOG_INFO("PhysicsSystem2D: using backend '{}'", world->backend_name());
        }
        return world.get();
    }

    physics::BodyType determine_body_type(scene::Entity* entity) const {
        auto* rb = entity->get_component<components::RigidBody2D>();
        if (!rb) return physics::BodyType::Static;
        if (rb->is_kinematic) return physics::BodyType::Kinematic;
        return physics::BodyType::Dynamic;
    }

    float determine_mass(scene::Entity* entity) const {
        auto* rb = entity->get_component<components::RigidBody2D>();
        if (rb) return rb->mass;
        return 1.0f;
    }

    physics::MaterialDesc make_material(scene::Entity* entity) const {
        physics::MaterialDesc mat;
        auto* rb = entity->get_component<components::RigidBody2D>();
        if (rb) {
            mat.friction = rb->friction;
            mat.restitution = rb->restitution;
        } else {
            mat.friction = 0.5f;
            mat.restitution = 0.2f;
        }
        return mat;
    }

    void create_shapes(Slot& slot) {
        if (!world) return;
        scene::Entity* entity = slot.entity;
        if (!entity) return;

        // 先清理旧形状
        for (auto sh : slot.shapes) {
            world->destroy_shape(sh);
        }
        slot.shapes.clear();

        auto* t = entity->transform();
        math::Vector2f scale(t->scale.x, t->scale.y);
        float mass = determine_mass(entity);
        physics::MaterialDesc mat = make_material(entity);

        auto* box = entity->get_component<components::BoxCollider2D>();
        if (box) {
            math::Vector2f half_extents = math::Vector2f(box->size.x * scale.x * 0.5f,
                                                         box->size.y * scale.y * 0.5f);
            math::Vector2f offset(box->center.x * scale.x, box->center.y * scale.y);
            mat.density = compute_density_for_box(mass, half_extents);
            physics::ShapeHandle sh = world->add_box_shape(slot.body, half_extents, offset, 0.0f, mat);
            if (sh != physics::k_invalid_shape) {
                slot.shapes.push_back(sh);
            }
            slot.has_box = true;
            slot.last_box_size = box->size;
            slot.last_box_center = box->center;
        } else {
            slot.has_box = false;
        }

        auto* circle = entity->get_component<components::CircleCollider2D>();
        if (circle) {
            float radius = circle->radius * std::max(std::abs(scale.x), std::abs(scale.y));
            math::Vector2f offset(circle->center.x * scale.x, circle->center.y * scale.y);
            mat.density = compute_density_for_circle(mass, radius);
            physics::ShapeHandle sh = world->add_circle_shape(slot.body, radius, offset, mat);
            if (sh != physics::k_invalid_shape) {
                slot.shapes.push_back(sh);
            }
            slot.has_circle = true;
            slot.last_circle_radius = circle->radius;
            slot.last_circle_center = circle->center;
        } else {
            slot.has_circle = false;
        }
    }

    bool shapes_changed(const Slot& slot, scene::Entity* entity) const {
        auto* t = entity->transform();
        math::Vector2f scale(t->scale.x, t->scale.y);
        if (scale != slot.last_scale) return true;

        auto* box = entity->get_component<components::BoxCollider2D>();
        if (box) {
            if (!slot.has_box || box->size != slot.last_box_size || box->center != slot.last_box_center) {
                return true;
            }
        } else if (slot.has_box) {
            return true;
        }

        auto* circle = entity->get_component<components::CircleCollider2D>();
        if (circle) {
            if (!slot.has_circle || circle->radius != slot.last_circle_radius || circle->center != slot.last_circle_center) {
                return true;
            }
        } else if (slot.has_circle) {
            return true;
        }

        return false;
    }

    void create_body(scene::Entity* entity, const scene::UUID& uuid) {
        if (!world) return;
        auto* t = entity->transform();
        if (!t) return;

        math::Vector2f pos(t->position.x, t->position.y);
        float angle = quat_to_z(t->rotation);

        physics::BodyHandle body = world->create_body(determine_body_type(entity), pos, angle);
        if (body == physics::k_invalid_body) {
            GLOG_WARN("PhysicsSystem2D: failed to create body for entity '{}'", entity->name());
            return;
        }

        Slot& slot = slots[uuid];
        slot.entity = entity;
        slot.body = body;
        slot.last_position = pos;
        slot.last_angle = angle;
        slot.last_scale = math::Vector2f(t->scale.x, t->scale.y);

        auto* rb = entity->get_component<components::RigidBody2D>();
        if (rb) {
            world->set_linear_velocity(body, rb->velocity);
            world->set_linear_damping(body, rb->linear_damping);
            world->set_gravity_scale(body, rb->use_gravity ? 1.0f : 0.0f);
            world->set_fixed_rotation(body, rb->fixed_rotation);
        } else {
            world->set_gravity_scale(body, 0.0f);
            world->set_fixed_rotation(body, true);
        }

        create_shapes(slot);
    }

    void sync_to_backend(Slot& slot) {
        if (!world) return;
        scene::Entity* entity = slot.entity;
        if (!entity) return;
        auto* t = entity->transform();
        if (!t) return;

        math::Vector2f pos(t->position.x, t->position.y);
        float angle = quat_to_z(t->rotation);

        // 位置/角度变更较大时同步到后端（避免每帧都写）
        if (pos != slot.last_position || std::abs(angle - slot.last_angle) > 1e-4f) {
            world->set_transform(slot.body, pos, angle);
            slot.last_position = pos;
            slot.last_angle = angle;
        }

        auto* rb = entity->get_component<components::RigidBody2D>();
        if (rb) {
            world->set_linear_velocity(slot.body, rb->velocity);
            world->set_gravity_scale(slot.body, rb->use_gravity ? 1.0f : 0.0f);
            world->wake_up(slot.body);
        }

        if (shapes_changed(slot, entity)) {
            create_shapes(slot);
            slot.last_scale = math::Vector2f(t->scale.x, t->scale.y);
        }
    }

    void sync_from_backend(Slot& slot) {
        if (!world) return;
        scene::Entity* entity = slot.entity;
        if (!entity) return;
        auto* t = entity->transform();
        if (!t) return;

        auto* rb = entity->get_component<components::RigidBody2D>();
        if (!rb) return; // 静态/运动学不读回

        math::Vector2f pos;
        float angle = 0.0f;
        world->get_transform(slot.body, pos, angle);
        t->position.x = pos.x;
        t->position.y = pos.y;
        t->rotation = z_to_quat(angle);
        slot.last_position = pos;
        slot.last_angle = angle;

        rb->velocity = world->get_linear_velocity(slot.body);
        rb->is_sleeping = world->is_sleeping(slot.body);
    }

    void apply_acceleration_and_forces(Slot& slot) {
        if (!world) return;
        scene::Entity* entity = slot.entity;
        if (!entity) return;
        auto* rb = entity->get_component<components::RigidBody2D>();
        if (!rb || rb->is_kinematic || rb->is_sleeping) return;
        if (rb->acceleration == math::Vector2f::zero()) return;

        // acceleration 视为 m/s^2 的持续加速度，本帧每个物理子步都施加
        math::Vector2f force = rb->acceleration * rb->mass;
        world->apply_force_to_center(slot.body, force);
    }
};

PhysicsSystem2D::PhysicsSystem2D()
    : impl_(std::make_unique<Impl>()) {}

PhysicsSystem2D::~PhysicsSystem2D() = default;

void PhysicsSystem2D::on_update(scene::Scene& scene, float dt) {
    if (dt <= 0.0f) return;

    auto* world = impl_->ensure_world(gravity);
    if (!world) return;

    world->set_gravity(gravity);

    // 收集当前帧所有需要物理模拟的实体
    std::unordered_set<scene::UUID> current_uuids;
    scene.foreach([&](scene::Entity* entity) {
        if (!entity || !entity->enabled) return;
        auto* t = entity->transform();
        if (!t) return;
        bool has_rb = entity->get_component<components::RigidBody2D>() != nullptr;
        bool has_static = entity->get_component<components::StaticBody2D>() != nullptr;
        if (!has_rb && !has_static) return;
        bool has_collider = entity->get_component<components::BoxCollider2D>() != nullptr ||
                            entity->get_component<components::CircleCollider2D>() != nullptr;
        if (!has_collider) return;

        const scene::UUID& uuid = entity->uuid();
        current_uuids.insert(uuid);

        auto it = impl_->slots.find(uuid);
        if (it == impl_->slots.end()) {
            impl_->create_body(entity, uuid);
            it = impl_->slots.find(uuid);
            if (it == impl_->slots.end()) return;
        }
        it->second.seen_this_frame = true;
        impl_->sync_to_backend(it->second);
    });

    // Box2D 等外部后端要求每帧使用固定小步长，不能一次性 step 整个 dt
    impl_->time_accumulator += dt;
    int steps = static_cast<int>(impl_->time_accumulator / fixed_dt);
    if (steps > max_steps_per_frame) {
        GLOG_WARN("PhysicsSystem2D: clamping steps from {} to {} (dt={:.4f})", steps, max_steps_per_frame, dt);
        steps = max_steps_per_frame;
        impl_->time_accumulator = 0.0f;
    } else {
        impl_->time_accumulator -= static_cast<float>(steps) * fixed_dt;
    }

    for (int i = 0; i < steps; ++i) {
        // 每步施加组件声明的加速度（作为持续力）
        for (auto& [uuid, slot] : impl_->slots) {
            if (slot.seen_this_frame) {
                impl_->apply_acceleration_and_forces(slot);
            }
        }

        world->step(fixed_dt, substeps);

        // 把动态刚体的状态回写组件
        for (auto& [uuid, slot] : impl_->slots) {
            if (slot.seen_this_frame) {
                impl_->sync_from_backend(slot);
            }
        }

        // 中间步骤把组件状态同步回后端，供下一步使用
        if (i + 1 < steps) {
            for (auto& [uuid, slot] : impl_->slots) {
                if (slot.seen_this_frame) {
                    impl_->sync_to_backend(slot);
                }
            }
        }
    }

    // 本帧的 acceleration 已用完，清零
    scene.foreach([&](scene::Entity* entity) {
        if (!entity || !entity->enabled) return;
        auto* rb = entity->get_component<components::RigidBody2D>();
        if (rb) {
            rb->acceleration = math::Vector2f::zero();
        }
    });

    // 清理已销毁的实体对应的 body
    for (auto it = impl_->slots.begin(); it != impl_->slots.end();) {
        if (!it->second.seen_this_frame) {
            if (it->second.body != physics::k_invalid_body) {
                world->destroy_body(it->second.body);
            }
            it = impl_->slots.erase(it);
        } else {
            it->second.seen_this_frame = false;
            ++it;
        }
    }
}

} // namespace gryce_engine::ecs
