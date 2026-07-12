#include "ecs/systems/physics_system_3d.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "components/rigid_body.h"
#include "components/static_body.h"
#include "components/box_collider.h"
#include "components/sphere_collider.h"
#include "components/plane_collider.h"
#include "components/transform.h"
#include "components/physical_material.h"
#include "ecs/query.h"
#include "physics/physics_factory.h"
#include "scene/entity.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

namespace {

constexpr float k_pi = 3.14159265358979323846f;
constexpr float k_mass_scale = 0.001f;
constexpr float k_sleep_threshold = 0.05f;

math::Vector3f mul_per_component(const math::Vector3f& a, const math::Vector3f& b) {
    return math::Vector3f(a.x * b.x, a.y * b.y, a.z * b.z);
}

float compute_collider_volume(scene::Entity* entity) {
    auto* t = entity->transform();
    if (!t) return 1.0f;
    if (auto* box = entity->get_component<components::BoxCollider>()) {
        math::Vector3f size = mul_per_component(box->size, t->scale);
        return std::abs(size.x * size.y * size.z);
    }
    if (auto* sphere = entity->get_component<components::SphereCollider>()) {
        float scale_max = std::max({std::abs(t->scale.x), std::abs(t->scale.y), std::abs(t->scale.z)});
        float r = sphere->radius * scale_max;
        return (4.0f / 3.0f) * k_pi * r * r * r;
    }
    return 1.0f;
}

void apply_physical_material(scene::Entity* entity, components::RigidBody* rb) {
    if (!entity || !rb) return;
    auto* pm = entity->get_component<components::PhysicalMaterial>();
    if (!pm) return;

    float volume = compute_collider_volume(entity);
    rb->mass = std::max(0.001f, pm->density * volume * k_mass_scale);
    rb->restitution = math::clamp(1.0f - pm->softness, 0.0f, 1.0f);
    rb->friction = math::clamp(pm->friction, 0.0f, 1.0f);
    // 将风阻系数映射为线性阻尼（简化模型）
    rb->linear_damping = math::clamp(pm->drag_coefficient, 0.0f, 1.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// PhysicsSystem3D 内部实现
// ---------------------------------------------------------------------------
struct PhysicsSystem3D::Impl {
    std::unique_ptr<physics::IPhysicsWorld3D> world;

    struct Slot {
        scene::Entity* entity = nullptr;
        physics::BodyHandle body = physics::k_invalid_body;
        physics::ShapeHandle shape = physics::k_invalid_shape;

        math::Vector3f last_position;
        math::Quaternionf last_rotation = math::Quaternionf::identity();
        math::Vector3f last_scale{1.0f, 1.0f, 1.0f};

        // collider 缓存
        math::Vector3f last_box_size;
        math::Vector3f last_box_center;
        bool has_box = false;

        float last_sphere_radius = 0.0f;
        math::Vector3f last_sphere_center;
        bool has_sphere = false;

        math::Vector3f last_plane_normal;
        float last_plane_offset = 0.0f;
        bool has_plane = false;

        // 材质/质量缓存，用于检测是否需要重建 body
        float last_mass = 0.0f;
        float last_friction = 0.5f;
        float last_restitution = 0.2f;

        bool seen_this_frame = false;
    };

    std::unordered_map<scene::UUID, Slot> slots;
    bool initialized = false;
    bool create_failed = false;
    float time_accumulator = 0.0f;

    physics::IPhysicsWorld3D* ensure_world(const math::Vector3f& gravity) {
        if (create_failed) return nullptr;
        if (!world) {
            world = physics::create_physics_world_3d("jolt");
            if (!world || !world->init(gravity)) {
                GLOG_ERROR("PhysicsSystem3D: failed to create 3D physics world (Jolt). "
                           "Pass -DGRYCE_FETCH_JOLT=ON.");
                create_failed = true;
                return nullptr;
            }
            GLOG_INFO("PhysicsSystem3D: using backend '{}'", world->backend_name());
        }
        return world.get();
    }

    physics::BodyType determine_body_type(scene::Entity* entity) const {
        if (entity->get_component<components::StaticBody>()) {
            return physics::BodyType::Static;
        }
        auto* rb = entity->get_component<components::RigidBody>();
        if (rb) {
            return rb->is_kinematic ? physics::BodyType::Kinematic : physics::BodyType::Dynamic;
        }
        return physics::BodyType::Static;
    }

    physics::MaterialDesc make_material(scene::Entity* entity) const {
        physics::MaterialDesc mat;
        auto* pm = entity->get_component<components::PhysicalMaterial>();
        if (pm) {
            mat.friction = pm->friction;
            mat.restitution = 1.0f - pm->softness;
        } else {
            auto* rb = entity->get_component<components::RigidBody>();
            if (rb) {
                mat.friction = rb->friction;
                mat.restitution = rb->restitution;
            } else {
                mat.friction = 0.5f;
                mat.restitution = 0.2f;
            }
        }
        mat.friction = math::clamp(mat.friction, 0.0f, 1.0f);
        mat.restitution = math::clamp(mat.restitution, 0.0f, 1.0f);
        return mat;
    }

    void compute_mass_and_density(scene::Entity* entity, float& out_mass, float& out_density) const {
        auto* rb = entity->get_component<components::RigidBody>();
        auto* sb = entity->get_component<components::StaticBody>();
        if (sb || !rb) {
            out_mass = 0.0f;
            out_density = 1.0f;
            return;
        }

        float volume = compute_collider_volume(entity);
        float mass = rb->mass;
        if (volume > 1e-9f) {
            out_mass = mass;
            out_density = mass / volume;
        } else {
            out_mass = mass;
            out_density = 1.0f;
        }
    }

    bool shapes_changed(const Slot& slot, scene::Entity* entity) const {
        auto* t = entity->transform();
        if (!t) return false;
        if (t->scale != slot.last_scale) return true;

        auto* box = entity->get_component<components::BoxCollider>();
        if (box) {
            if (!slot.has_box || box->size != slot.last_box_size || box->center != slot.last_box_center) {
                return true;
            }
        } else if (slot.has_box) {
            return true;
        }

        auto* sphere = entity->get_component<components::SphereCollider>();
        if (sphere) {
            if (!slot.has_sphere || sphere->radius != slot.last_sphere_radius || sphere->center != slot.last_sphere_center) {
                return true;
            }
        } else if (slot.has_sphere) {
            return true;
        }

        auto* plane = entity->get_component<components::PlaneCollider>();
        if (plane) {
            if (!slot.has_plane || plane->normal != slot.last_plane_normal || plane->offset != slot.last_plane_offset) {
                return true;
            }
        } else if (slot.has_plane) {
            return true;
        }

        return false;
    }

    bool material_changed(const Slot& slot, scene::Entity* entity) const {
        auto* rb = entity->get_component<components::RigidBody>();
        if (!rb) return false;
        if (std::abs(rb->mass - slot.last_mass) > 1e-4f) return true;
        if (std::abs(rb->friction - slot.last_friction) > 1e-4f) return true;
        if (std::abs(rb->restitution - slot.last_restitution) > 1e-4f) return true;
        return false;
    }

    void destroy_slot_body(Slot& slot) {
        if (!world) return;
        if (slot.body != physics::k_invalid_body) {
            world->destroy_body(slot.body);
            slot.body = physics::k_invalid_body;
        }
        if (slot.shape != physics::k_invalid_shape) {
            world->destroy_shape(slot.shape);
            slot.shape = physics::k_invalid_shape;
        }
    }

    void create_shape_for_slot(Slot& slot) {
        if (!world) return;
        scene::Entity* entity = slot.entity;
        if (!entity) return;
        auto* t = entity->transform();
        if (!t) return;

        // 清理旧 shape
        if (slot.shape != physics::k_invalid_shape) {
            world->destroy_shape(slot.shape);
            slot.shape = physics::k_invalid_shape;
        }

        float mass = 0.0f;
        float density = 1.0f;
        compute_mass_and_density(entity, mass, density);

        physics::ShapeDesc desc;
        desc.density = density;

        auto* box = entity->get_component<components::BoxCollider>();
        if (box) {
            math::Vector3f world_size = mul_per_component(box->size, t->scale);
            desc.type = physics::ShapeType::Box;
            desc.size = world_size * 0.5f;
            desc.offset = mul_per_component(box->center, t->scale);
            slot.has_box = true;
            slot.last_box_size = box->size;
            slot.last_box_center = box->center;
        } else {
            slot.has_box = false;
        }

        auto* sphere = entity->get_component<components::SphereCollider>();
        if (sphere) {
            float scale_max = std::max({std::abs(t->scale.x), std::abs(t->scale.y), std::abs(t->scale.z)});
            float radius = sphere->radius * scale_max;
            desc.type = physics::ShapeType::Sphere;
            desc.size = math::Vector3f(radius, 0.0f, 0.0f);
            desc.offset = mul_per_component(sphere->center, t->scale);
            slot.has_sphere = true;
            slot.last_sphere_radius = sphere->radius;
            slot.last_sphere_center = sphere->center;
        } else {
            slot.has_sphere = false;
        }

        auto* plane = entity->get_component<components::PlaneCollider>();
        if (plane) {
            desc.type = physics::ShapeType::Plane;
            slot.has_plane = true;
            slot.last_plane_normal = plane->normal;
            slot.last_plane_offset = plane->offset;
        } else {
            slot.has_plane = false;
        }

        if (!box && !sphere && !plane) return;

        slot.shape = world->create_shape(desc);
    }

    void create_body(scene::Entity* entity, const scene::UUID& uuid) {
        if (!world) return;
        auto* t = entity->transform();
        if (!t) return;

        auto* rb = entity->get_component<components::RigidBody>();
        auto* pm = entity->get_component<components::PhysicalMaterial>();
        if (pm && rb) {
            apply_physical_material(entity, rb);
        }

        Slot& slot = slots[uuid];
        slot.entity = entity;
        slot.last_scale = t->scale;
        create_shape_for_slot(slot);

        if (slot.shape == physics::k_invalid_shape) {
            GLOG_WARN("PhysicsSystem3D: failed to create shape for entity '{}'", entity->name());
            return;
        }

        physics::BodyDesc desc;
        desc.type = determine_body_type(entity);
        desc.position = t->position;
        desc.rotation = t->rotation;
        desc.shape = slot.shape;

        if (rb) {
            desc.linear_velocity = rb->velocity;
            desc.angular_velocity = rb->angular_velocity;
            desc.mass = rb->mass;
            desc.linear_damping = rb->linear_damping;
            desc.angular_damping = rb->angular_damping;
            desc.allow_sleep = true;
        }

        slot.body = world->create_body(desc);
        if (slot.body == physics::k_invalid_body) {
            GLOG_WARN("PhysicsSystem3D: failed to create body for entity '{}'", entity->name());
            return;
        }

        physics::MaterialDesc mat = make_material(entity);
        world->attach_shape(slot.body, slot.shape, mat);

        if (rb) {
            world->set_gravity_scale(slot.body, rb->use_gravity ? 1.0f : 0.0f);
            world->set_linear_damping(slot.body, rb->linear_damping);
            world->set_angular_damping(slot.body, rb->angular_damping);
        } else {
            world->set_gravity_scale(slot.body, 0.0f);
        }

        slot.last_position = t->position;
        slot.last_rotation = t->rotation;
        slot.last_mass = rb ? rb->mass : 0.0f;
        slot.last_friction = mat.friction;
        slot.last_restitution = mat.restitution;
    }

    void sync_to_backend(Slot& slot) {
        if (!world || slot.body == physics::k_invalid_body) return;
        scene::Entity* entity = slot.entity;
        if (!entity) return;
        auto* t = entity->transform();
        if (!t) return;

        auto* rb = entity->get_component<components::RigidBody>();
        auto* pm = entity->get_component<components::PhysicalMaterial>();
        if (pm && rb) {
            apply_physical_material(entity, rb);
        }

        // 如果质量或材质发生变化，直接重建 body（Jolt 不支持运行时修改质量/形状）
        if (material_changed(slot, entity) || shapes_changed(slot, entity)) {
            destroy_slot_body(slot);
            create_body(entity, entity->uuid());
            return;
        }

        // 位置/旋转变化较大时同步到后端
        bool pos_changed = (t->position != slot.last_position);
        float rot_dot = std::abs(t->rotation.x * slot.last_rotation.x +
                                t->rotation.y * slot.last_rotation.y +
                                t->rotation.z * slot.last_rotation.z +
                                t->rotation.w * slot.last_rotation.w);
        bool rot_changed = rot_dot < 0.9999f;
        if (pos_changed || rot_changed) {
            world->set_transform(slot.body, t->position, t->rotation);
            slot.last_position = t->position;
            slot.last_rotation = t->rotation;
        }

        if (rb) {
            world->set_linear_velocity(slot.body, rb->velocity);
            world->set_gravity_scale(slot.body, rb->use_gravity ? 1.0f : 0.0f);
            world->wake_up(slot.body);
        }
    }

    void sync_from_backend(Slot& slot) {
        if (!world || slot.body == physics::k_invalid_body) return;
        scene::Entity* entity = slot.entity;
        if (!entity) return;
        auto* t = entity->transform();
        if (!t) return;

        auto* rb = entity->get_component<components::RigidBody>();
        if (!rb) return; // 静态/运动学不读回

        math::Vector3f pos;
        math::Quaternionf rot;
        world->get_transform(slot.body, pos, rot);
        t->position = pos;
        t->rotation = rot;
        slot.last_position = pos;
        slot.last_rotation = rot;

        rb->velocity = world->get_linear_velocity(slot.body);
        rb->angular_velocity = world->get_angular_velocity(slot.body);
        rb->is_sleeping = world->is_sleeping(slot.body);
    }

    void apply_acceleration_and_forces(Slot& slot) {
        if (!world || slot.body == physics::k_invalid_body) return;
        scene::Entity* entity = slot.entity;
        if (!entity) return;
        auto* rb = entity->get_component<components::RigidBody>();
        if (!rb || rb->is_kinematic || rb->is_sleeping) return;
        if (rb->acceleration == math::Vector3f::zero()) return;

        // acceleration 视为 m/s^2 的持续加速度，本帧每个物理子步都施加
        math::Vector3f force = rb->acceleration * rb->mass;
        world->apply_force(slot.body, force, math::Vector3f::zero());
    }
};

PhysicsSystem3D::PhysicsSystem3D()
    : impl_(std::make_unique<Impl>()) {}

PhysicsSystem3D::~PhysicsSystem3D() = default;

void PhysicsSystem3D::on_init(scene::Scene& /*scene*/) {
    impl_->ensure_world(gravity);
}

void PhysicsSystem3D::on_shutdown(scene::Scene& /*scene*/) {
    for (auto& [uuid, slot] : impl_->slots) {
        impl_->destroy_slot_body(slot);
    }
    impl_->slots.clear();
    if (impl_->world) {
        impl_->world->shutdown();
        impl_->world.reset();
    }
}

void PhysicsSystem3D::on_update(scene::Scene& scene, float dt) {
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
        bool has_rb = entity->get_component<components::RigidBody>() != nullptr;
        bool has_static = entity->get_component<components::StaticBody>() != nullptr;
        if (!has_rb && !has_static) return;
        bool has_collider = entity->get_component<components::BoxCollider>() != nullptr ||
                            entity->get_component<components::SphereCollider>() != nullptr ||
                            entity->get_component<components::PlaneCollider>() != nullptr;
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

    // 固定步长积分
    impl_->time_accumulator += dt;
    int steps = static_cast<int>(impl_->time_accumulator / fixed_dt);
    if (steps > max_steps_per_frame) {
        GLOG_WARN("PhysicsSystem3D: clamping steps from {} to {} (dt={:.4f})", steps, max_steps_per_frame, dt);
        steps = max_steps_per_frame;
        impl_->time_accumulator = 0.0f;
    } else {
        impl_->time_accumulator -= static_cast<float>(steps) * fixed_dt;
    }

    for (int i = 0; i < steps; ++i) {
        for (auto& [uuid, slot] : impl_->slots) {
            if (slot.seen_this_frame) {
                impl_->apply_acceleration_and_forces(slot);
            }
        }

        world->step(fixed_dt, substeps);

        for (auto& [uuid, slot] : impl_->slots) {
            if (slot.seen_this_frame) {
                impl_->sync_from_backend(slot);
            }
        }

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
        auto* rb = entity->get_component<components::RigidBody>();
        if (rb) {
            rb->acceleration = math::Vector3f::zero();
        }
    });

    // 清理已销毁实体对应的 body
    for (auto it = impl_->slots.begin(); it != impl_->slots.end();) {
        if (!it->second.seen_this_frame) {
            impl_->destroy_slot_body(it->second);
            it = impl_->slots.erase(it);
        } else {
            it->second.seen_this_frame = false;
            ++it;
        }
    }
}

} // namespace gryce_engine::ecs
