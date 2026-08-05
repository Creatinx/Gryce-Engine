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
#include "components/character_controller_2d.h"
#include "components/joint_2d.h"
#include "components/2d/component_2d.h"
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

math::Quaternionf z_to_quat(float angle) {
    return math::Quaternionf::from_axis_angle(math::Vector3f(0.0f, 0.0f, 1.0f), angle);
}

// 实体的 2D 世界变换（沿父链组合，top_level 截止；见 components::d2::world_transform_2d）。
// 物理后端统一在世界空间模拟：body 的创建/同步都用世界位置与旋转。
components::d2::Transform2D world_2d(const scene::Entity* entity) {
    return components::d2::world_transform_2d(entity);
}

// 把 2D 世界位置/旋转写回本地 Transform：对父级世界变换求逆
// （平移 + 旋转 + 缩放全补偿；父级缩放接近 0 的分量不做缩放补偿）。
void set_local_from_world_2d(scene::Entity* entity, const math::Vector2f& wpos, float wangle) {
    auto* t = entity ? entity->transform() : nullptr;
    if (!t) return;
    const components::d2::Transform2D pw = components::d2::world_transform_2d(entity->parent());
    const float c = std::cos(-pw.rotation);
    const float s = std::sin(-pw.rotation);
    const float dx = wpos.x - pw.position.x;
    const float dy = wpos.y - pw.position.y;
    math::Vector2f local(dx * c - dy * s, dx * s + dy * c);
    if (std::abs(pw.scale.x) > 1e-6f) local.x /= pw.scale.x;
    if (std::abs(pw.scale.y) > 1e-6f) local.y /= pw.scale.y;
    t->position.x = local.x;
    t->position.y = local.y;
    t->rotation = z_to_quat(wangle - pw.rotation);
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

        // 速度/重力缓存：仅在变化时唤醒，避免每帧 wake_up 使睡眠机制失效
        math::Vector2f last_velocity;
        float last_gravity_scale = 1.0f;

        bool seen_this_frame = false;
    };

    struct JointSlot {
        physics::JointHandle handle = physics::k_invalid_joint;
        scene::UUID body_a_uuid;
        scene::UUID body_b_uuid;
        bool seen_this_frame = false;
    };

    std::unordered_map<scene::UUID, Slot> slots;
    std::unordered_map<scene::UUID, JointSlot> joints;
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

        math::Vector2f scale = world_2d(entity).scale; // 碰撞体尺寸随父链缩放
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
        math::Vector2f scale = world_2d(entity).scale;
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

        // body 使用 2D 世界空间（沿父链组合，top_level 截止）
        const auto wt = world_2d(entity);
        math::Vector2f pos = wt.position;
        float angle = wt.rotation;

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
        slot.last_scale = world_2d(entity).scale;

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

        // 与渲染一致，同步 2D 世界空间位置/旋转到后端
        const auto wt = world_2d(entity);
        math::Vector2f pos = wt.position;
        float angle = wt.rotation;

        // 位置/角度变更较大时同步到后端（避免每帧都写）
        if (pos != slot.last_position || std::abs(angle - slot.last_angle) > 1e-4f) {
            world->set_transform(slot.body, pos, angle);
            slot.last_position = pos;
            slot.last_angle = angle;
        }

        auto* rb = entity->get_component<components::RigidBody2D>();
        if (rb) {
            auto* cc = entity->get_component<components::CharacterController2D>();
            if (!cc) {
                world->set_linear_velocity(slot.body, rb->velocity);
            }
            const float gravity_scale = rb->use_gravity ? 1.0f : 0.0f;
            world->set_gravity_scale(slot.body, gravity_scale);
            // 仅当速度/重力设置或变换实际变化时唤醒，避免每帧 wake_up 让睡眠机制失效
            const bool vel_changed = rb->velocity != slot.last_velocity;
            const bool grav_changed = gravity_scale != slot.last_gravity_scale;
            if (vel_changed || grav_changed || pos != slot.last_position || std::abs(angle - slot.last_angle) > 1e-4f) {
                world->wake_up(slot.body);
            }
            slot.last_velocity = rb->velocity;
            slot.last_gravity_scale = gravity_scale;
        }

        if (shapes_changed(slot, entity)) {
            create_shapes(slot);
            slot.last_scale = world_2d(entity).scale;
        }
    }

    struct GroundInfo2D {
        bool hit = false;
        math::Vector2f normal = math::Vector2f(0.0f, 1.0f);
        float distance = 0.0f;
    };

    GroundInfo2D check_ground_2d(const math::Vector2f& pos, const components::CharacterController2D* cc) const {
        GroundInfo2D best;
        math::Vector2f down(0.0f, -1.0f);
        math::Vector2f base = pos + cc->ground_check_offset;
        float span = cc->ground_check_span * 0.5f;

        math::Vector2f origins[3] = {
            base + math::Vector2f(-span, 0.0f),
            base,
            base + math::Vector2f(span, 0.0f)
        };

        for (const auto& origin : origins) {
            auto hit = world->raycast(origin, down, cc->ground_check_distance);
            if (hit.has_value()) {
                float dist = hit->fraction * cc->ground_check_distance;
                // 取最低命中点（最大距离），避免把台阶顶当作地面
                if (!best.hit || dist > best.distance) {
                    best.hit = true;
                    best.normal = hit->normal;
                    best.distance = dist;
                }
            }
        }
        return best;
    }

    void apply_character_controller(Slot& slot, float dt) {
        if (!world || slot.body == physics::k_invalid_body) return;
        scene::Entity* entity = slot.entity;
        if (!entity) return;
        auto* rb = entity->get_component<components::RigidBody2D>();
        auto* cc = entity->get_component<components::CharacterController2D>();
        if (!rb || !cc) return;

        if (cc->fixed_rotation) {
            world->set_fixed_rotation(slot.body, true);
        }

        // 接地/台阶探测在世界空间进行（与 body 同步空间一致）
        math::Vector2f pos = world_2d(entity).position;

        // 1. 多射线接地检测
        GroundInfo2D ground = check_ground_2d(pos, cc);
        bool grounded_now = ground.hit;
        cc->is_grounded = cc->is_grounded || grounded_now;
        if (grounded_now) {
            cc->ground_normal = ground.normal;
        }

        // 2. 输入目标速度
        math::Vector2f target_vel = math::Vector2f::zero();
        if (cc->move_input.length_sq() > 1e-6f) {
            target_vel = cc->move_input.normalized() * cc->speed;
        }

        // 3. 坡度处理：投影到地面切线，陡坡限制向上移动
        if (grounded_now) {
            float normal_y = math::clamp(ground.normal.y, -1.0f, 1.0f);
            float slope_limit_cos = std::cos(cc->slope_limit_degrees * 3.14159265f / 180.0f);
            if (normal_y < slope_limit_cos) {
                // 陡坡：禁止向上分量
                if (target_vel.y > 0.0f) target_vel.y = 0.0f;
            } else {
                // 投影到坡面切线
                math::Vector2f tangent(-ground.normal.y, ground.normal.x);
                if (tangent.length_sq() < 1e-6f) tangent = math::Vector2f(1.0f, 0.0f);
                tangent = tangent.normalized();
                float proj = target_vel.dot(tangent);
                target_vel = tangent * proj;
            }
        }

        // 4. 台阶：从角色前方向下探测地面高度，若出现低矮台阶则抬升角色
        if (grounded_now && target_vel.length_sq() > 1e-6f && cc->step_height > 0.0f) {
            math::Vector2f dir = target_vel.normalized();
            math::Vector2f base = pos + cc->ground_check_offset;
            float current_ground_y = base.y - ground.distance;

            // 在角色前方（略超碰撞体边缘）向下探测，找到前方地面/台阶顶高度
            float forward_offset = cc->ground_check_span * 0.5f + 0.52f;
            math::Vector2f probe_origin = base + dir * forward_offset;
            probe_origin.y += cc->step_height + 0.02f;
            auto step_hit = world->raycast(probe_origin, math::Vector2f(0.0f, -1.0f), cc->step_height * 4.0f);
            if (step_hit.has_value()) {
                float height_delta = step_hit->point.y - current_ground_y;
                if (height_delta > 0.01f && height_delta <= cc->step_height) {
                    // 检查头顶到新位置是否通畅
                    math::Vector2f head_origin = base + math::Vector2f(0.0f, height_delta + 0.1f);
                    auto head_hit = world->raycast(head_origin, dir, forward_offset);
                    if (!head_hit.has_value()) {
                        math::Vector2f new_pos = pos + dir * 0.05f + math::Vector2f(0.0f, height_delta + 0.02f);
                        // new_pos 为世界空间，写回本地 Transform
                        set_local_from_world_2d(entity, new_pos, world_2d(entity).rotation);
                        world->set_transform(slot.body, new_pos, world_2d(entity).rotation);
                        slot.last_position = new_pos;
                    }
                }
            }
        }

        // 5. 推撞保留：无输入时防下滑，有输入时向目标速度混合
        math::Vector2f current_vel = rb->velocity;
        if (cc->move_input.length_sq() < 1e-6f && grounded_now) {
            if (current_vel.length() < 0.2f) {
                target_vel = math::Vector2f::zero();
            } else {
                target_vel = current_vel * std::max(0.0f, 1.0f - dt * cc->push_recovery_speed);
            }
        } else {
            float blend = math::clamp(dt * cc->push_recovery_speed, 0.0f, 1.0f);
            target_vel = current_vel.lerp(target_vel, blend);
        }

        // 6. 跳跃
        if (cc->jump_requested && grounded_now) {
            target_vel.y = cc->jump_force;
            cc->jump_requested = false;
        }

        rb->velocity = target_vel;
        world->set_linear_velocity(slot.body, target_vel);
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
        // 后端输出的是世界空间，写回本地 Transform 需按父级世界变换求逆
        set_local_from_world_2d(entity, pos, angle);
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

    void update_joints(scene::Scene& scene) {
        if (!world) return;

        for (auto& [uuid, jslot] : joints) {
            jslot.seen_this_frame = false;
        }

        scene.foreach([&](scene::Entity* entity) {
            if (!entity || !entity->enabled) return;
            auto* joint = entity->get_component<components::Joint2D>();
            if (!joint) return;
            if (!joint->body_a_uuid.is_valid() || !joint->body_b_uuid.is_valid()) return;

            scene::Entity* body_a = scene.find_entity_by_uuid(joint->body_a_uuid);
            scene::Entity* body_b = scene.find_entity_by_uuid(joint->body_b_uuid);
            if (!body_a || !body_b) return;

            auto it_a = slots.find(body_a->uuid());
            auto it_b = slots.find(body_b->uuid());
            if (it_a == slots.end() || it_b == slots.end()) return;

            physics::BodyHandle handle_a = it_a->second.body;
            physics::BodyHandle handle_b = it_b->second.body;
            if (handle_a == physics::k_invalid_body || handle_b == physics::k_invalid_body) return;

            const scene::UUID& uuid = entity->uuid();
            auto it = joints.find(uuid);
            if (it == joints.end()) {
                physics::JointDesc2D desc;
                desc.type = joint->joint_type;
                desc.body_a = handle_a;
                desc.body_b = handle_b;
                desc.anchor_a = joint->anchor_a;
                desc.anchor_b = joint->anchor_b;
                desc.length = joint->length;
                desc.frequency = joint->frequency;
                desc.damping = joint->damping;
                desc.collide_connected = joint->collide_connected;
                physics::JointHandle jh = world->create_joint(desc);
                if (jh != physics::k_invalid_joint) {
                    JointSlot jslot;
                    jslot.handle = jh;
                    jslot.body_a_uuid = body_a->uuid();
                    jslot.body_b_uuid = body_b->uuid();
                    jslot.seen_this_frame = true;
                    joints[uuid] = jslot;
                }
            } else {
                it->second.seen_this_frame = true;
            }
        });

        for (auto it = joints.begin(); it != joints.end();) {
            if (!it->second.seen_this_frame) {
                world->destroy_joint(it->second.handle);
                it = joints.erase(it);
            } else {
                ++it;
            }
        }
    }
};

PhysicsSystem2D::PhysicsSystem2D()
    : impl_(std::make_unique<Impl>()) {}

PhysicsSystem2D::~PhysicsSystem2D() = default;

void PhysicsSystem2D::on_init(scene::Scene& /*scene*/) {
    impl_->ensure_world(gravity);
}

void PhysicsSystem2D::on_shutdown(scene::Scene& /*scene*/) {
    // 场景分离时销毁全部物理资源，避免旧场景的 body/关节残留到下一个场景
    for (auto& [uuid, jslot] : impl_->joints) {
        if (jslot.handle != physics::k_invalid_joint && impl_->world) {
            impl_->world->destroy_joint(jslot.handle);
        }
    }
    impl_->joints.clear();
    for (auto& [uuid, slot] : impl_->slots) {
        if (slot.body != physics::k_invalid_body && impl_->world) {
            impl_->world->destroy_body(slot.body);
        }
    }
    impl_->slots.clear();
    if (impl_->world) {
        impl_->world->shutdown();
        impl_->world.reset();
    }
    impl_->time_accumulator = 0.0f;
    impl_->create_failed = false; // 允许下次场景重新创建（此前失败可能只是环境问题）
}

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
        // 每帧开始时重置接地状态，随后各子步会累积“本帧是否曾接地”
        if (auto* cc = entity->get_component<components::CharacterController2D>()) {
            cc->is_grounded = false;
        }
        impl_->sync_to_backend(it->second);
    });

    // 创建/更新关节（必须在 body 创建之后、step 之前）
    impl_->update_joints(scene);

    // Box2D 等外部后端要求每帧使用固定小步长，不能一次性 step 整个 dt。
    // fixed_dt<=0 时除法为 UB/无穷：跳过本帧步进，但仍执行后续清理。
    impl_->time_accumulator += dt;
    int steps = 0;
    if (fixed_dt > 0.0f) {
        steps = static_cast<int>(impl_->time_accumulator / fixed_dt);
    } else {
        GLOG_WARN("PhysicsSystem2D: fixed_dt={} <= 0, skipping physics steps", fixed_dt);
        impl_->time_accumulator = 0.0f;
    }
    if (steps > max_steps_per_frame) {
        GLOG_WARN("PhysicsSystem2D: clamping steps from {} to {} (dt={:.4f})", steps, max_steps_per_frame, dt);
        steps = max_steps_per_frame;
        impl_->time_accumulator = 0.0f;
    } else {
        impl_->time_accumulator -= static_cast<float>(steps) * fixed_dt;
    }

    for (int i = 0; i < steps; ++i) {
        // 角色控制器每物理子步更新一次（台阶探测需要紧跟碰撞位置）
        for (auto& [uuid, slot] : impl_->slots) {
            if (slot.seen_this_frame) {
                impl_->apply_character_controller(slot, fixed_dt);
            }
        }

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

    // 清理已销毁的实体对应的 body，同时级联销毁关联的关节
    for (auto it = impl_->slots.begin(); it != impl_->slots.end();) {
        if (!it->second.seen_this_frame) {
            const scene::UUID& body_uuid = it->first;
            for (auto jit = impl_->joints.begin(); jit != impl_->joints.end();) {
                if (jit->second.body_a_uuid == body_uuid || jit->second.body_b_uuid == body_uuid) {
                    world->destroy_joint(jit->second.handle);
                    jit = impl_->joints.erase(jit);
                } else {
                    ++jit;
                }
            }
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

std::optional<physics::IPhysicsWorld2D::RaycastHit2D> PhysicsSystem2D::raycast(
    const math::Vector2f& origin, const math::Vector2f& direction, float max_distance) const {
    if (!impl_->world) return std::nullopt;
    return impl_->world->raycast(origin, direction, max_distance);
}

} // namespace gryce_engine::ecs
