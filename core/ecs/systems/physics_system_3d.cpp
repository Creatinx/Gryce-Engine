#include "ecs/systems/physics_system_3d.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "components/rigid_body.h"
#include "components/static_body.h"
#include "components/mesh_renderer.h"
#include "components/box_collider.h"
#include "components/sphere_collider.h"
#include "components/plane_collider.h"
#include "components/character_controller_3d.h"
#include "components/joint_3d.h"
#include "components/transform.h"
#include "components/physical_material.h"
#include "assets/asset_manager.h"
#include "assets/mesh_data.h"
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

// 实体碰撞体积（世界空间）：优先用原始碰撞体组件，无则回退到网格包围盒。
// 用于把 rb->mass 换算成 shape 密度，保证 Jolt 计算出的刚体质量 ≈ 声明质量。
float compute_body_volume(scene::Entity* entity) {
    auto* t = entity->transform();
    if (!t) return 1.0f;

    math::Vector3f scale(std::abs(t->scale.x), std::abs(t->scale.y), std::abs(t->scale.z));

    if (auto* box = entity->get_component<components::BoxCollider>()) {
        math::Vector3f s = mul_per_component(box->size, scale);
        return s.x * s.y * s.z;
    }
    if (auto* sphere = entity->get_component<components::SphereCollider>()) {
        const float r = sphere->radius * std::max(scale.x, std::max(scale.y, scale.z));
        return (4.0f / 3.0f) * k_pi * r * r * r;
    }
    // 无限平面视为零体积

    auto* mr = entity->get_component<components::MeshRenderer>();
    if (!mr || mr->mesh_path.empty()) return 1.0f;
    auto mesh = assets::AssetManager::instance().load_mesh(mr->mesh_path);
    if (!mesh || mesh->vertices.empty()) return 1.0f;

    math::Vector3f min_p = mesh->vertices[0].position;
    math::Vector3f max_p = mesh->vertices[0].position;
    for (const auto& v : mesh->vertices) {
        min_p = math::Vector3f(std::min(min_p.x, v.position.x),
                               std::min(min_p.y, v.position.y),
                               std::min(min_p.z, v.position.z));
        max_p = math::Vector3f(std::max(max_p.x, v.position.x),
                               std::max(max_p.y, v.position.y),
                               std::max(max_p.z, v.position.z));
    }
    math::Vector3f local_size = max_p - min_p;
    math::Vector3f world_size = mul_per_component(local_size, scale);
    return world_size.x * world_size.y * world_size.z;
}

void apply_physical_material(scene::Entity* entity, components::RigidBody* rb) {
    if (!entity || !rb) return;
    auto* pm = entity->get_component<components::PhysicalMaterial>();
    if (!pm) return;

    float volume = compute_body_volume(entity);
    rb->mass = std::max(0.001f, pm->density * volume * k_mass_scale);
    rb->restitution = math::clamp(1.0f - pm->softness, 0.0f, 1.0f);
    rb->friction = math::clamp(pm->friction, 0.0f, 1.0f);
    // 将风阻系数映射为线性阻尼（简化模型）
    rb->linear_damping = math::clamp(pm->drag_coefficient, 0.0f, 1.0f);
}

// 实体是否具备可用于物理模拟的形状：网格（MeshRenderer）或任一原始碰撞体组件。
bool has_physics_shape(scene::Entity* entity) {
    if (!entity) return false;
    auto* mr = entity->get_component<components::MeshRenderer>();
    if (mr && !mr->mesh_path.empty()) return true;
    return entity->get_component<components::BoxCollider>() != nullptr ||
           entity->get_component<components::SphereCollider>() != nullptr ||
           entity->get_component<components::PlaneCollider>() != nullptr;
}

// 实体任一碰撞体标记为触发器（is_trigger）即为传感器。
bool entity_has_trigger(scene::Entity* entity) {
    if (!entity) return false;
    if (auto* box = entity->get_component<components::BoxCollider>()) {
        if (box->is_trigger) return true;
    }
    if (auto* sphere = entity->get_component<components::SphereCollider>()) {
        if (sphere->is_trigger) return true;
    }
    if (auto* plane = entity->get_component<components::PlaneCollider>()) {
        if (plane->is_trigger) return true;
    }
    return false;
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

        // 碰撞体缓存
        std::string last_mesh_path;

        // 原始碰撞体缓存（检测组件增删/参数变化，触发 body 重建）
        bool has_box = false;
        math::Vector3f last_box_size;
        math::Vector3f last_box_center;
        bool has_sphere = false;
        float last_sphere_radius = 0.0f;
        math::Vector3f last_sphere_center;
        bool has_plane = false;
        math::Vector3f last_plane_normal;
        float last_plane_offset = 0.0f;

        // 材质/质量缓存，用于检测是否需要重建 body
        float last_mass = 0.0f;
        float last_friction = 0.5f;
        float last_restitution = 0.2f;
        bool last_is_trigger = false;

        // 速度/重力缓存：仅在变化时唤醒，避免每帧 wake_up 使睡眠机制失效
        math::Vector3f last_velocity;
        float last_gravity_scale = 1.0f;

        bool seen_this_frame = false;
    };

    struct JointSlot {
        physics::JointHandle handle = physics::k_invalid_joint;
        scene::UUID body_a_uuid;
        scene::UUID body_b_uuid;
        // 创建时的 body 句柄：body 因 mass/shape 变化重建后句柄失效，需重建关节
        physics::BodyHandle body_a_handle = physics::k_invalid_body;
        physics::BodyHandle body_b_handle = physics::k_invalid_body;
        bool seen_this_frame = false;
    };

    std::unordered_map<scene::UUID, Slot> slots;
    std::unordered_map<scene::UUID, JointSlot> joints;
    bool initialized = false;
    bool create_failed = false;
    float time_accumulator = 0.0f;

    // 本帧碰撞/触发事件缓冲
    std::vector<physics::CollisionEvent> physics_events_;
    std::vector<EntityCollisionEvent> frame_events_;

    // 取回后端事件 → 映射为实体 UUID 级别事件，并更新 RigidBody::last_collision_impulse。
    void drain_collision_events_internal() {
        frame_events_.clear();
        if (!world) return;

        physics_events_.clear();
        world->drain_collision_events(physics_events_);

        std::unordered_map<physics::BodyHandle, float> impulses;
        world->drain_collision_impulses(impulses);

        // 每帧重置 last_collision_impulse，避免陈旧冲量持续触发碎裂。
        // 只处理本帧仍存活（seen_this_frame）的槽位：实体可能已被销毁，
        // slot.entity 是悬垂指针，不能再解引用。
        for (auto& [uuid, slot] : slots) {
            if (!slot.seen_this_frame) continue;
            auto* rb = slot.entity ? slot.entity->get_component<components::RigidBody>() : nullptr;
            if (rb) rb->last_collision_impulse = 0.0f;
        }
        if (!impulses.empty()) {
            for (auto& [uuid, slot] : slots) {
                if (!slot.seen_this_frame || slot.body == physics::k_invalid_body) continue;
                auto it = impulses.find(slot.body);
                if (it != impulses.end()) {
                    auto* rb = slot.entity ? slot.entity->get_component<components::RigidBody>() : nullptr;
                    if (rb) rb->last_collision_impulse = it->second;
                }
            }
        }

        if (physics_events_.empty()) return;

        // body 句柄 → 实体（仅本帧存活实体，避免映射到悬垂指针）
        std::unordered_map<physics::BodyHandle, scene::Entity*> handle_to_entity;
        handle_to_entity.reserve(slots.size());
        for (const auto& [uuid, slot] : slots) {
            if (slot.seen_this_frame && slot.body != physics::k_invalid_body && slot.entity) {
                handle_to_entity[slot.body] = slot.entity;
            }
        }

        frame_events_.reserve(physics_events_.size());
        for (const auto& ev : physics_events_) {
            auto ita = handle_to_entity.find(ev.body_a);
            auto itb = handle_to_entity.find(ev.body_b);
            if (ita == handle_to_entity.end() || itb == handle_to_entity.end()) continue;

            EntityCollisionEvent out;
            out.type = ev.type;
            out.body_a = ita->second->uuid();
            out.body_b = itb->second->uuid();
            out.point = ev.point;
            out.normal = ev.normal;
            out.impulse = ev.impulse;
            // EndContact 后端不提供 body 引用，这里按实体碰撞体状态补全
            out.is_trigger = ev.is_trigger ||
                             entity_has_trigger(ita->second) ||
                             entity_has_trigger(itb->second);
            frame_events_.push_back(out);
        }
    }

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

        float volume = compute_body_volume(entity);
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

        auto* mr = entity->get_component<components::MeshRenderer>();
        const bool has_mesh = mr && !mr->mesh_path.empty();
        const std::string current_mesh = mr ? mr->mesh_path : std::string();

        // Mesh takes precedence when building the shape (see create_shape_for_slot).
        // If the entity also carries primitive collider components, those are NOT
        // used for the shape, so they must not drive the "changed" detection here;
        // otherwise every frame reports a change and rebuilds all bodies.
        if (has_mesh) {
            return current_mesh != slot.last_mesh_path ||
                   entity_has_trigger(entity) != slot.last_is_trigger;
        }

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
            if (!slot.has_sphere || sphere->radius != slot.last_sphere_radius ||
                sphere->center != slot.last_sphere_center) {
                return true;
            }
        } else if (slot.has_sphere) {
            return true;
        }

        auto* plane = entity->get_component<components::PlaneCollider>();
        if (plane) {
            if (!slot.has_plane || plane->normal != slot.last_plane_normal ||
                plane->offset != slot.last_plane_offset) {
                return true;
            }
        } else if (slot.has_plane) {
            return true;
        }

        if (current_mesh != slot.last_mesh_path) return true;

        if (entity_has_trigger(entity) != slot.last_is_trigger) return true;

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

        const math::Vector3f scale(std::abs(t->scale.x), std::abs(t->scale.y), std::abs(t->scale.z));

        // 先清空全部碰撞体缓存，实际使用的会重新写入
        slot.has_box = false;
        slot.has_sphere = false;
        slot.has_plane = false;

        auto* mr = entity->get_component<components::MeshRenderer>();
        if (mr && !mr->mesh_path.empty()) {
            auto mesh_data = assets::AssetManager::instance().load_mesh(mr->mesh_path);
            if (!mesh_data || mesh_data->vertices.empty()) {
                GLOG_WARN("PhysicsSystem3D: no mesh data for entity '{}'", entity->name());
                return;
            }

            // 静态物体用精确三角网格，动态刚体用凸包（Jolt MeshShape 不能用于动态）
            // 顶点需乘 transform scale：Jolt body transform 只含位置/旋转（无 scale），
            // 若不缩放顶点，碰撞体尺寸会与渲染（模型矩阵含 scale）不一致，导致物体穿透地面。
            const bool is_static = entity->get_component<components::StaticBody>() != nullptr;
            const auto scale_pos = [&scale](const math::Vector3f& p) {
                return math::Vector3f(p.x * scale.x, p.y * scale.y, p.z * scale.z);
            };
            if (is_static) {
                desc.type = physics::ShapeType::Mesh;
                desc.points.reserve(mesh_data->vertices.size());
                for (const auto& v : mesh_data->vertices) {
                    desc.points.push_back(scale_pos(v.position));
                }
                desc.indices = mesh_data->indices;
                if (desc.indices.empty()) {
                    // 没有索引时从 vertices 顺序生成三角面（假设每 3 个顶点一组）
                    desc.indices.reserve((mesh_data->vertices.size() / 3) * 3);
                    for (uint32_t i = 0; i + 2 < mesh_data->vertices.size(); i += 3) {
                        desc.indices.push_back(i);
                        desc.indices.push_back(i + 1);
                        desc.indices.push_back(i + 2);
                    }
                }
            } else {
                desc.type = physics::ShapeType::ConvexHull;
                desc.points.reserve(mesh_data->vertices.size());
                for (const auto& v : mesh_data->vertices) {
                    desc.points.push_back(scale_pos(v.position));
                }
            }
            slot.last_mesh_path = mr->mesh_path;
        } else {
            // 无网格：从原始碰撞体组件构建基本形状
            slot.last_mesh_path.clear();
            auto* box = entity->get_component<components::BoxCollider>();
            auto* sphere = entity->get_component<components::SphereCollider>();
            auto* plane = entity->get_component<components::PlaneCollider>();
            if (box) {
                desc.type = physics::ShapeType::Box;
                // BoxCollider.size 是完整尺寸，Jolt BoxShape 需要半宽
                desc.size = mul_per_component(box->size, scale) * 0.5f;
                desc.offset = mul_per_component(box->center, scale);
                slot.has_box = true;
                slot.last_box_size = box->size;
                slot.last_box_center = box->center;
            } else if (sphere) {
                desc.type = physics::ShapeType::Sphere;
                desc.size = math::Vector3f(sphere->radius * std::max(scale.x, std::max(scale.y, scale.z)), 0.0f, 0.0f);
                desc.offset = mul_per_component(sphere->center, scale);
                slot.has_sphere = true;
                slot.last_sphere_radius = sphere->radius;
                slot.last_sphere_center = sphere->center;
            } else if (plane) {
                desc.type = physics::ShapeType::Plane;
                slot.has_plane = true;
                slot.last_plane_normal = plane->normal;
                slot.last_plane_offset = plane->offset;
            } else {
                GLOG_WARN("PhysicsSystem3D: entity '{}' has no collider or mesh", entity->name());
                return;
            }
        }

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
        desc.is_sensor = entity_has_trigger(entity);

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
        slot.last_is_trigger = desc.is_sensor;
    }

    void sync_to_backend(Slot& slot, bool allow_body_rebuild = true) {
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
        // Rebuild only during the frame-start collection pass. Destroying and
        // recreating bodies between substeps (step1 -> step2) leaves Jolt's
        // contact/constraint caches dangling; worker threads of the next substep
        // dereference freed bodies and crash at a fixed offset (0xc0000005).
        if (allow_body_rebuild) {
            if (material_changed(slot, entity) || shapes_changed(slot, entity)) {
                GLOG_INFO("Phys3D: REBUILD body for entity '{}' (material/shape changed)",
                          entity->name());
                destroy_slot_body(slot);
                create_body(entity, entity->uuid());
                return;
            }
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
            auto* cc = entity->get_component<components::CharacterController3D>();
            if (!cc) {
                world->set_linear_velocity(slot.body, rb->velocity);
            }
            const float gravity_scale = rb->use_gravity ? 1.0f : 0.0f;
            world->set_gravity_scale(slot.body, gravity_scale);
            // 仅当速度/重力设置或变换实际变化时唤醒，避免每帧 wake_up 让睡眠机制失效
            const bool vel_changed = rb->velocity != slot.last_velocity;
            const bool grav_changed = gravity_scale != slot.last_gravity_scale;
            if (vel_changed || grav_changed || pos_changed || rot_changed) {
                world->wake_up(slot.body);
            }
            slot.last_velocity = rb->velocity;
            slot.last_gravity_scale = gravity_scale;
        }
    }

    struct GroundInfo3D {
        bool hit = false;
        math::Vector3f normal = math::Vector3f(0.0f, 1.0f, 0.0f);
        float distance = 0.0f;
    };

    // 辅助：raycast 并忽略自身碰撞体（解决射线起点在角色内部时命中自己的问题）
    // 辅助：raycast 并忽略自身碰撞体（解决射线起点在角色内部时命中自己的问题）
    std::optional<physics::RaycastHit> raycast_ignore_self(const math::Vector3f& origin,
                                                            const math::Vector3f& direction,
                                                            float max_distance,
                                                            physics::BodyHandle self_body) const {
        math::Vector3f o = origin;
        float traveled = 0.0f;
        for (int i = 0; i < 8; ++i) {
            float remaining = max_distance - traveled;
            if (remaining <= 1e-4f) return std::nullopt;
            auto hit = world->raycast(o, direction, remaining);
            if (!hit.has_value()) return std::nullopt;
            if (hit->body != self_body) {
                hit->distance += traveled;
                hit->point = origin + direction * hit->distance;
                return hit;
            }
            // 命中自己：向前跳跃一段，避免在薄壳上反复命中
            float step = std::max(hit->distance + 0.1f, 0.1f);
            o = origin + direction * (traveled + step);
            traveled += step;
        }
        return std::nullopt;
    }

    GroundInfo3D check_ground_3d(const math::Vector3f& pos, const components::CharacterController3D* cc, physics::BodyHandle self_body) const {
        GroundInfo3D best;
        math::Vector3f down(0.0f, -1.0f, 0.0f);
        math::Vector3f base = pos + cc->ground_check_offset;
        float r = cc->ground_check_radius;

        math::Vector3f origins[5] = {
            base,
            base + math::Vector3f(r, 0.0f, 0.0f),
            base + math::Vector3f(-r, 0.0f, 0.0f),
            base + math::Vector3f(0.0f, 0.0f, r),
            base + math::Vector3f(0.0f, 0.0f, -r)
        };

        for (const auto& origin : origins) {
            auto hit = raycast_ignore_self(origin, down, cc->ground_check_distance, self_body);
            if (hit.has_value()) {
                // 取最低命中点（最大距离），避免把台阶顶当作地面
                if (!best.hit || hit->distance > best.distance) {
                    best.hit = true;
                    best.normal = hit->normal;
                    best.distance = hit->distance;
                }
            }
        }
        return best;
    }

    void apply_character_controller(Slot& slot, float dt) {
        if (!world || slot.body == physics::k_invalid_body) return;
        scene::Entity* entity = slot.entity;
        if (!entity) return;
        auto* rb = entity->get_component<components::RigidBody>();
        auto* cc = entity->get_component<components::CharacterController3D>();
        if (!rb || !cc) return;

        if (cc->fixed_rotation) {
            world->set_angular_velocity(slot.body, math::Vector3f::zero());
        }

        auto* t = entity->transform();
        math::Vector3f pos = t ? t->position : math::Vector3f::zero();

        // 1. 多射线接地检测
        GroundInfo3D ground = check_ground_3d(pos, cc, slot.body);
        bool grounded_now = ground.hit;
        cc->is_grounded = cc->is_grounded || grounded_now;
        if (grounded_now) {
            cc->ground_normal = ground.normal;
        }

        // 2. 输入目标速度（仅水平面），垂直分量保留当前速度（避免每子步覆盖重力/跳跃）
        math::Vector3f target_vel = math::Vector3f::zero();
        math::Vector2f input_h(cc->move_input.x, cc->move_input.z);
        if (input_h.length_sq() > 1e-6f) {
            input_h = input_h.normalized() * cc->speed;
            target_vel.x = input_h.x;
            target_vel.z = input_h.y;
        }
        target_vel.y = rb->velocity.y;

        // 3. 坡度处理
        if (grounded_now) {
            float normal_y = math::clamp(ground.normal.y, -1.0f, 1.0f);
            float slope_limit_cos = std::cos(cc->slope_limit_degrees * 3.14159265f / 180.0f);
            if (normal_y < slope_limit_cos) {
                // 陡坡：移除向上的水平移动分量
                math::Vector3f horizontal = target_vel;
                horizontal.y = 0.0f;
                if (horizontal.dot(ground.normal) < 0.0f) {
                    target_vel -= ground.normal * target_vel.dot(ground.normal);
                    target_vel.y = 0.0f;
                }
            } else {
                // 投影到地面平面
                math::Vector3f projected = target_vel - ground.normal * target_vel.dot(ground.normal);
                target_vel = projected;
                target_vel.y = 0.0f; // 保持水平速度，Y 由重力/跳跃单独控制
            }
        }

        // 4. 台阶：从角色前方向下探测地面高度，若出现低矮台阶则抬升角色
        if (grounded_now && input_h.length_sq() > 1e-6f && cc->step_height > 0.0f) {
            math::Vector3f dir = target_vel.normalized();
            math::Vector3f base = pos + cc->ground_check_offset;
            float current_ground_y = base.y - ground.distance;

            // 在角色前方（略超碰撞体边缘）向下探测，找到前方地面/台阶顶高度
            float forward_offset = cc->ground_check_radius + 0.52f;
            math::Vector3f probe_origin = base + dir * forward_offset;
            probe_origin.y += cc->step_height + 0.02f;
            auto step_hit = raycast_ignore_self(probe_origin, math::Vector3f(0.0f, -1.0f, 0.0f), cc->step_height * 4.0f, slot.body);
            if (step_hit.has_value()) {
                float height_delta = step_hit->point.y - current_ground_y;
                if (height_delta > 0.01f && height_delta <= cc->step_height) {
                    // 检查头顶到新位置是否通畅
                    math::Vector3f head_origin = base + math::Vector3f(0.0f, height_delta + 0.1f, 0.0f);
                    auto head_hit = raycast_ignore_self(head_origin, dir, forward_offset, slot.body);
                    if (!head_hit.has_value()) {
                        math::Vector3f new_pos = pos + dir * 0.15f + math::Vector3f(0.0f, height_delta + 0.02f, 0.0f);
                        t->position = new_pos;
                        world->set_transform(slot.body, new_pos, t->rotation);
                        slot.last_position = new_pos;
                    }
                }
            }
        }

        // 5. 推撞保留
        math::Vector3f current_vel = rb->velocity;
        if (input_h.length_sq() < 1e-6f && grounded_now) {
            math::Vector3f horizontal_vel = current_vel;
            horizontal_vel.y = 0.0f;
            if (horizontal_vel.length() < 0.2f) {
                target_vel.x = 0.0f;
                target_vel.z = 0.0f;
            } else {
                math::Vector3f decayed = current_vel * std::max(0.0f, 1.0f - dt * cc->push_recovery_speed);
                target_vel.x = decayed.x;
                target_vel.z = decayed.z;
            }
        } else {
            float blend = math::clamp(dt * cc->push_recovery_speed, 0.0f, 1.0f);
            math::Vector3f blended = current_vel.lerp(target_vel, blend);
            target_vel.x = blended.x;
            target_vel.z = blended.z;
            // 垂直速度只在空中时由推撞保留轻微影响，主要交给重力/跳跃
            if (!grounded_now) {
                target_vel.y = blended.y;
            }
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

    void update_joints(scene::Scene& scene) {
        if (!world) return;

        for (auto& [uuid, jslot] : joints) {
            jslot.seen_this_frame = false;
        }

        scene.foreach([&](scene::Entity* entity) {
            if (!entity || !entity->enabled) return;
            auto* joint = entity->get_component<components::Joint3D>();
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
            const bool handles_changed =
                it != joints.end() &&
                (it->second.body_a_handle != handle_a || it->second.body_b_handle != handle_b);
            if (it == joints.end() || handles_changed) {
                // 关节不存在，或连接的 body 因 mass/shape 变化被重建（旧句柄失效），
                // 需要销毁旧关节并基于当前句柄重新创建。
                if (it != joints.end()) {
                    world->destroy_joint(it->second.handle);
                    joints.erase(it);
                }
                physics::JointDesc3D desc;
                desc.type = joint->joint_type;
                desc.body_a = handle_a;
                desc.body_b = handle_b;
                desc.anchor_a = joint->anchor_a;
                desc.anchor_b = joint->anchor_b;
                desc.axis_a = joint->axis_a;
                desc.axis_b = joint->axis_b;
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
                    jslot.body_a_handle = handle_a;
                    jslot.body_b_handle = handle_b;
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

void PhysicsSystem3D::rebuild_body_for_entity(scene::Entity* entity) {
    if (!entity || !impl_->world) return;

    const scene::UUID& uuid = entity->uuid();
    auto it = impl_->slots.find(uuid);
    if (it != impl_->slots.end()) {
        impl_->destroy_slot_body(it->second);
        impl_->slots.erase(it);
    }

    auto* t = entity->transform();
    if (!t) return;
    bool has_rb = entity->get_component<components::RigidBody>() != nullptr;
    bool has_static = entity->get_component<components::StaticBody>() != nullptr;
    if (!has_rb && !has_static) return;
    if (!has_physics_shape(entity)) return;

    impl_->create_body(entity, uuid);
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
        if (!has_physics_shape(entity)) return;

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
        if (auto* cc = entity->get_component<components::CharacterController3D>()) {
            cc->is_grounded = false;
        }
        impl_->sync_to_backend(it->second);
    });

    // 创建/更新关节（必须在 body 创建之后、step 之前）
    impl_->update_joints(scene);

    // 固定步长积分。fixed_dt<=0 时除法为 UB/无穷：跳过本帧步进，但仍执行后续清理。
    impl_->time_accumulator += dt;
    int steps = 0;
    if (fixed_dt > 0.0f) {
        steps = static_cast<int>(impl_->time_accumulator / fixed_dt);
    } else {
        GLOG_WARN("PhysicsSystem3D: fixed_dt={} <= 0, skipping physics steps", fixed_dt);
        impl_->time_accumulator = 0.0f;
    }
    if (steps > max_steps_per_frame) {
        GLOG_WARN("PhysicsSystem3D: clamping steps from {} to {} (dt={:.4f})", steps, max_steps_per_frame, dt);
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
                    // Between substeps: sync transform/velocity only, never rebuild.
                    impl_->sync_to_backend(slot, /*allow_body_rebuild=*/false);
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

    // 取回本帧碰撞/触发事件并更新 last_collision_impulse
    impl_->drain_collision_events_internal();

    // 清理已销毁实体对应的 body，同时级联销毁关联的关节
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
            impl_->destroy_slot_body(it->second);
            it = impl_->slots.erase(it);
        } else {
            it->second.seen_this_frame = false;
            ++it;
        }
    }
}

std::optional<physics::RaycastHit> PhysicsSystem3D::raycast(const math::Vector3f& origin,
                                                            const math::Vector3f& direction,
                                                            float max_distance) const {
    if (!impl_->world) return std::nullopt;
    return impl_->world->raycast(origin, direction, max_distance);
}

const std::vector<PhysicsSystem3D::EntityCollisionEvent>& PhysicsSystem3D::collision_events() const {
    return impl_->frame_events_;
}

} // namespace gryce_engine::ecs
