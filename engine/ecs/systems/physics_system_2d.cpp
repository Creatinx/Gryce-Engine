#include "ecs/systems/physics_system_2d.h"

#include "components/physics_2d.h"
#include "components/transform.h"
#include "scene/entity.h"
#include "scene/query.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

#include <box2d/box2d.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gryce_engine::ecs {

namespace {

static_assert(sizeof(b2BodyId) <= sizeof(uint64_t), "b2BodyId must fit into uint64_t");

uint64_t pack_body(b2BodyId id) {
    uint64_t v = 0;
    std::memcpy(&v, &id, sizeof(b2BodyId));
    return v;
}

b2BodyId unpack_body(uint64_t v) {
    b2BodyId id;
    std::memcpy(&id, &v, sizeof(b2BodyId));
    return id;
}

// 2D 旋转只取绕 Z 轴的分量；纯 Z 旋转下 2*atan2(z, w) 即旋转角。
float z_angle_of(const math::Quaternionf& q) {
    return 2.0f * std::atan2(q.z, q.w);
}

math::Quaternionf quat_from_z(float angle) {
    const float half = angle * 0.5f;
    return math::Quaternionf(0.0f, 0.0f, std::sin(half), std::cos(half));
}

inline b2Vec2 to_b2(const math::Vector2f& v) { return b2Vec2{ v.x, v.y }; }
inline math::Vector2f to_v2(const b2Vec2& v) { return math::Vector2f(v.x, v.y); }

// 风区/浮力区：世界位置取实体局部变换（2D 场景通常为扁平层级）。
math::Vector2f zone_center(const scene::Entity* e, const math::Vector2f& offset) {
    const auto* tr = e->transform();
    const math::Vector3f p = tr ? tr->position : math::Vector3f::zero();
    return math::Vector2f(p.x + offset.x, p.y + offset.y);
}

} // namespace

struct PhysicsSystem2D::Impl {
    b2WorldId world = b2_nullWorldId;
    std::unordered_map<EntityID, b2BodyId> bodies;
    math::Vector2f gravity = math::Vector2f(0.0f, 980.0f); // 引擎单位/秒²
    float accumulator = 0.0f;
};

PhysicsSystem2D::PhysicsSystem2D() : impl_(std::make_unique<Impl>()) {}
PhysicsSystem2D::~PhysicsSystem2D() = default;

void PhysicsSystem2D::set_gravity(const math::Vector2f& g) {
    impl_->gravity = g;
    if (b2World_IsValid(impl_->world)) {
        b2World_SetGravity(impl_->world, to_b2(g / pixels_per_meter));
    }
}

math::Vector2f PhysicsSystem2D::get_gravity() const { return impl_->gravity; }

void PhysicsSystem2D::on_init(scene::Scene& scene) {
    (void)scene;
    if (b2World_IsValid(impl_->world)) {
        b2DestroyWorld(impl_->world);
        impl_->world = b2_nullWorldId;
    }
    impl_->bodies.clear();
    impl_->accumulator = 0.0f;

    b2WorldDef def = b2DefaultWorldDef();
    def.gravity = to_b2(impl_->gravity / std::max(1.0f, pixels_per_meter));
    impl_->world = b2CreateWorld(&def);
    if (!b2World_IsValid(impl_->world)) {
        GLOG_ERROR("PhysicsSystem2D: failed to create Box2D world");
    }
}

void PhysicsSystem2D::on_shutdown(scene::Scene& scene) {
    (void)scene;
    if (b2World_IsValid(impl_->world)) {
        b2DestroyWorld(impl_->world); // 销毁世界会一并销毁所有 body/shape
        impl_->world = b2_nullWorldId;
    }
    impl_->bodies.clear();
    impl_->accumulator = 0.0f;
}

void PhysicsSystem2D::on_update(scene::Scene& scene, float dt) {
    if (!b2World_IsValid(impl_->world)) return;

    const float ppm = std::max(1.0f, pixels_per_meter);
    const float inv_ppm = 1.0f / ppm;

    // --- 1. 生命周期同步：创建缺失刚体、销毁已失效刚体 -------------------
    std::unordered_set<EntityID> live;
    auto rb_pool = scene.component_store().pool<components::RigidBody2D>();
    live.reserve(rb_pool.size());

    for (components::RigidBody2D* rb : rb_pool) {
        if (!rb) continue;
        scene::Entity* e = rb->owner();
        if (!e) continue;
        live.insert(e->id());

        const b2BodyId existing = unpack_body(rb->runtime_body);
        if (b2Body_IsValid(existing) && b2Body_GetType(existing) ==
                static_cast<b2BodyType>(static_cast<int>(rb->body_type))) {
            continue;
        }
        if (b2Body_IsValid(existing)) b2DestroyBody(existing);

        b2BodyDef bd = b2DefaultBodyDef();
        bd.type = static_cast<b2BodyType>(static_cast<int>(rb->body_type));
        const auto* tr = e->transform();
        const math::Vector3f pos = tr ? tr->position : math::Vector3f::zero();
        bd.position = b2Vec2{ pos.x * inv_ppm, pos.y * inv_ppm };
        bd.rotation = b2MakeRot(tr ? z_angle_of(tr->rotation) : 0.0f);
        bd.linearVelocity = b2Vec2{ rb->velocity.x * inv_ppm, rb->velocity.y * inv_ppm };
        bd.angularVelocity = rb->angular_velocity;
        bd.linearDamping = rb->linear_damping;
        bd.angularDamping = rb->angular_damping;
        bd.gravityScale = rb->gravity_scale;
        bd.fixedRotation = rb->fixed_rotation;
        bd.isBullet = rb->continuous;
        bd.isAwake = rb->awake;
        bd.userData = e;

        b2BodyId id = b2CreateBody(impl_->world, &bd);
        if (!b2Body_IsValid(id)) {
            GLOG_WARN("PhysicsSystem2D: failed to create body for '{}'", e->name());
            rb->runtime_body = 0;
            continue;
        }

        // 碰撞体：优先取 Box/Circle；两者皆无则补 1x1 米默认方形，保证有质量。
        bool has_shape = false;
        if (auto* box = e->get_component<components::BoxCollider2D>()) {
            b2ShapeDef sd = b2DefaultShapeDef();
            sd.density = std::max(0.0f, box->density);
            sd.material.friction = box->friction;
            sd.material.restitution = box->restitution;
            sd.isSensor = box->is_sensor;
            const float hx = std::max(0.001f, box->size.x * 0.5f) * inv_ppm;
            const float hy = std::max(0.001f, box->size.y * 0.5f) * inv_ppm;
            const b2Polygon poly = b2MakeOffsetBox(
                hx, hy, b2Vec2{ box->offset.x * inv_ppm, box->offset.y * inv_ppm },
                b2MakeRot(box->rotation));
            b2CreatePolygonShape(id, &sd, &poly);
            has_shape = true;
        }
        if (auto* circle = e->get_component<components::CircleCollider2D>()) {
            b2ShapeDef sd = b2DefaultShapeDef();
            sd.density = std::max(0.0f, circle->density);
            sd.material.friction = circle->friction;
            sd.material.restitution = circle->restitution;
            sd.isSensor = circle->is_sensor;
            b2Circle shape{};
            shape.center = b2Vec2{ circle->offset.x * inv_ppm, circle->offset.y * inv_ppm };
            shape.radius = std::max(0.001f, circle->radius) * inv_ppm;
            b2CreateCircleShape(id, &sd, &shape);
            has_shape = true;
        }
        if (!has_shape) {
            b2ShapeDef sd = b2DefaultShapeDef();
            sd.density = 1.0f;
            const b2Polygon poly = b2MakeBox(0.5f, 0.5f);
            b2CreatePolygonShape(id, &sd, &poly);
        }

        impl_->bodies[e->id()] = id;
        rb->runtime_body = pack_body(id);
    }

    // 销毁已失效（实体被删或组件被移除）的刚体
    for (auto it = impl_->bodies.begin(); it != impl_->bodies.end();) {
        if (!live.contains(it->first)) {
            if (b2Body_IsValid(it->second)) b2DestroyBody(it->second);
            it = impl_->bodies.erase(it);
        } else {
            ++it;
        }
    }

    // --- 2. 推进前同步：静态/运动学刚体跟随 Transform；动力学应用脚本速度 --
    for (components::RigidBody2D* rb : rb_pool) {
        if (!rb || !rb->enabled) continue;
        scene::Entity* e = rb->owner();
        if (!e || !e->enabled) continue;
        const b2BodyId id = unpack_body(rb->runtime_body);
        if (!b2Body_IsValid(id)) continue;

        const auto* tr = e->transform();
        const math::Vector3f pos = tr ? tr->position : math::Vector3f::zero();
        const float angle = tr ? z_angle_of(tr->rotation) : 0.0f;

        if (rb->body_type == components::Body2DType::Dynamic) {
            // 脚本改写 velocity 时下发；回写阶段会把物理结果同步回组件，
            // 因此只有"被外部改动"的值才会与刚体当前速度不一致。
            const b2Vec2 bv = b2Body_GetLinearVelocity(id);
            const math::Vector2f want = rb->velocity * inv_ppm;
            if (std::fabs(want.x - bv.x) > 1e-3f || std::fabs(want.y - bv.y) > 1e-3f) {
                b2Body_SetLinearVelocity(id, to_b2(want));
            }
            const float w = b2Body_GetAngularVelocity(id);
            if (std::fabs(rb->angular_velocity - w) > 1e-3f) {
                b2Body_SetAngularVelocity(id, rb->angular_velocity);
            }
        } else {
            b2Body_SetTransform(id, b2Vec2{ pos.x * inv_ppm, pos.y * inv_ppm },
                                b2MakeRot(angle));
            if (rb->body_type == components::Body2DType::Kinematic) {
                b2Body_SetLinearVelocity(id, to_b2(rb->velocity * inv_ppm));
                b2Body_SetAngularVelocity(id, rb->angular_velocity);
            }
        }
    }

    // --- 3. 固定步长推进（每步先施加风力与浮力） -------------------------
    impl_->accumulator += std::min(dt, 0.25f);
    const float step = std::max(1.0f / 240.0f, fixed_timestep);
    int steps = 0;
    while (impl_->accumulator >= step && steps < std::max(1, max_steps_per_frame)) {
        apply_wind(scene, step, ppm);
        apply_buoyancy(scene, step, ppm);
        b2World_Step(impl_->world, step, std::max(1, sub_step_count));
        impl_->accumulator -= step;
        ++steps;
    }
    if (steps >= std::max(1, max_steps_per_frame)) {
        impl_->accumulator = 0.0f; // 丢弃积压，避免卡顿后死亡螺旋
    }

    // --- 4. 回写：位姿 -> Transform，速度 -> RigidBody2D ------------------
    for (components::RigidBody2D* rb : rb_pool) {
        if (!rb) continue;
        scene::Entity* e = rb->owner();
        if (!e) continue;
        const b2BodyId id = unpack_body(rb->runtime_body);
        if (!b2Body_IsValid(id)) continue;

        auto* tr = e->transform();
        if (tr) {
            const b2Vec2 p = b2Body_GetPosition(id);
            tr->position.x = p.x * ppm;
            tr->position.y = p.y * ppm;
            tr->rotation = quat_from_z(b2Rot_GetAngle(b2Body_GetRotation(id)));
        }
        rb->velocity = to_v2(b2Body_GetLinearVelocity(id)) * ppm;
        rb->angular_velocity = b2Body_GetAngularVelocity(id);
        rb->awake = b2Body_IsAwake(id);
    }
}

// ---------------------------------------------------------------------------
// 风力：对区域内动态刚体施加加速度 = force + 湍流 - drag * 物体速度
// 边缘衰减 falloff 让区域边界平滑过渡，避免物体穿越边界时力突变。
// ---------------------------------------------------------------------------
void PhysicsSystem2D::apply_wind(scene::Scene& scene, float dt, float ppm) {
    struct Zone {
        math::Vector2f center;
        math::Vector2f half;
        math::Vector2f force;
        float turbulence;
        float falloff;
        float drag;
        float phase;
        bool wake;
    };
    std::vector<Zone> zones;
    foreach_with_component<components::WindZone2D>(
        scene, [&](scene::Entity* e, components::WindZone2D* w) {
            Zone z;
            z.center = zone_center(e, w->offset);
            z.half = math::Vector2f(std::max(0.0f, w->size.x) * 0.5f,
                                    std::max(0.0f, w->size.y) * 0.5f);
            z.force = w->force;
            z.turbulence = std::max(0.0f, w->turbulence);
            z.falloff = math::clamp(w->falloff, 0.0f, 1.0f);
            z.drag = std::max(0.0f, w->drag);
            z.phase = w->runtime_phase;
            z.wake = w->affect_sleeping;
            w->runtime_phase += dt * w->turbulence_frequency;
            zones.push_back(z);
        });
    if (zones.empty()) return;

    const float inv_ppm = 1.0f / ppm;
    auto rb_pool = scene.component_store().pool<components::RigidBody2D>();
    for (components::RigidBody2D* rb : rb_pool) {
        if (!rb || !rb->enabled) continue;
        if (rb->body_type != components::Body2DType::Dynamic) continue;
        scene::Entity* e = rb->owner();
        if (!e || !e->enabled) continue;
        const b2BodyId id = unpack_body(rb->runtime_body);
        if (!b2Body_IsValid(id)) continue;

        const b2Vec2 bp = b2Body_GetPosition(id);
        const math::Vector2f pos(bp.x * ppm, bp.y * ppm);
        const float mass = b2Body_GetMass(id);
        if (mass <= 0.0f) continue;

        math::Vector2f accel = math::Vector2f::zero();
        float drag = 0.0f;
        bool wake = false;
        bool in_any = false;
        for (const Zone& z : zones) {
            if (z.half.x <= 0.0f || z.half.y <= 0.0f) continue;
            const math::Vector2f d = pos - z.center;
            if (std::fabs(d.x) > z.half.x || std::fabs(d.y) > z.half.y) continue;

            // 边缘衰减：t 为归一化到边界的距离（0 = 中心，1 = 边界）
            float weight = 1.0f;
            if (z.falloff > 0.0f) {
                const float t = std::max(std::fabs(d.x) / z.half.x,
                                         std::fabs(d.y) / z.half.y);
                weight = math::clamp(1.0f - z.falloff * t, 0.0f, 1.0f);
            }
            if (weight <= 0.0f) continue;

            accel += z.force * weight;
            if (z.turbulence > 0.0f) {
                // 两相错频正弦扰动：随时间连续变化，同区域内物体受力一致
                accel.x += std::sin(z.phase) * z.turbulence * weight;
                accel.y += std::cos(z.phase * 1.37f) * z.turbulence * weight;
            }
            drag = std::max(drag, z.drag);
            wake = wake || z.wake;
            in_any = true;
        }
        if (!in_any) continue;

        // 速度阻尼：-drag * v（引擎单位/秒²）
        if (drag > 0.0f) {
            const b2Vec2 bv = b2Body_GetLinearVelocity(id);
            accel.x -= bv.x * ppm * drag;
            accel.y -= bv.y * ppm * drag;
        }

        // 加速度 -> 牛顿力：F = m * a，a 由引擎单位换算为米制
        const b2Vec2 force{ mass * accel.x * inv_ppm, mass * accel.y * inv_ppm };
        b2Body_ApplyForceToCenter(id, force, wake);
    }
}

// ---------------------------------------------------------------------------
// 浮力：阿基米德原理。浸没比例 f 由水面与刚体包围盒沿重力方向的投影求得，
// 浮力方向与重力相反、大小 = fluid_density * 浸没面积 * |g|；作用点取浸没
// 部分形心，天然产生扶正力矩。另叠加相对流速的线性阻力与角阻力。
// ---------------------------------------------------------------------------
void PhysicsSystem2D::apply_buoyancy(scene::Scene& scene, float dt, float ppm) {
    (void)dt;
    struct Fluid {
        math::Vector2f center;
        math::Vector2f half;
        float surface_y;      // 世界坐标（引擎单位）
        float density;
        float linear_drag;
        float angular_drag;
        math::Vector2f flow;
        bool wake;
    };
    std::vector<Fluid> fluids;
    foreach_with_component<components::BuoyancyArea2D>(
        scene, [&](scene::Entity* e, components::BuoyancyArea2D* b) {
            Fluid f;
            f.center = zone_center(e, b->offset);
            f.half = math::Vector2f(std::max(0.0f, b->size.x) * 0.5f,
                                    std::max(0.0f, b->size.y) * 0.5f);
            f.surface_y = b->surface_height;
            f.density = std::max(0.0f, b->fluid_density);
            f.linear_drag = std::max(0.0f, b->linear_drag);
            f.angular_drag = std::max(0.0f, b->angular_drag);
            f.flow = b->flow_velocity;
            f.wake = b->affects_sleeping;
            fluids.push_back(f);
        });
    if (fluids.empty()) return;

    const float inv_ppm = 1.0f / ppm;
    const math::Vector2f g = impl_->gravity;
    const float g_len = g.length();
    if (g_len <= 1e-6f) return;
    const math::Vector2f g_dir = g / g_len;   // 单位向量，指向重力方向
    const float g_m = g_len * inv_ppm;        // 米制重力加速度

    auto rb_pool = scene.component_store().pool<components::RigidBody2D>();
    for (components::RigidBody2D* rb : rb_pool) {
        if (!rb || !rb->enabled) continue;
        if (rb->body_type != components::Body2DType::Dynamic) continue;
        scene::Entity* e = rb->owner();
        if (!e || !e->enabled) continue;
        const b2BodyId id = unpack_body(rb->runtime_body);
        if (!b2Body_IsValid(id)) continue;

        const b2AABB aabb = b2Body_ComputeAABB(id);
        const float mass = b2Body_GetMass(id);
        if (mass <= 0.0f) continue;

        for (const Fluid& f : fluids) {
            // 水平方向须落在流体区域矩形内
            const float aabb_cx_px = (aabb.lowerBound.x + aabb.upperBound.x) * 0.5f * ppm;
            const float aabb_cy_px = (aabb.lowerBound.y + aabb.upperBound.y) * 0.5f * ppm;
            if (std::fabs(aabb_cx_px - f.center.x) > f.half.x ||
                std::fabs(aabb_cy_px - f.center.y) > f.half.y) {
                continue;
            }

            // 包围盒四角沿重力方向的"浸没深度"（>0 表示在水面下方）
            const b2Vec2 corners[4] = {
                { aabb.lowerBound.x, aabb.lowerBound.y },
                { aabb.upperBound.x, aabb.lowerBound.y },
                { aabb.lowerBound.x, aabb.upperBound.y },
                { aabb.upperBound.x, aabb.upperBound.y },
            };
            const float surface_y_m = f.surface_y * inv_ppm;
            float min_d = 1e30f, max_d = -1e30f;
            float sx = 0.0f, sy = 0.0f;
            int submerged = 0;
            for (const b2Vec2& c : corners) {
                const b2Vec2 rel{ c.x, c.y - surface_y_m };
                const float d = rel.x * g_dir.x + rel.y * g_dir.y;
                min_d = std::min(min_d, d);
                max_d = std::max(max_d, d);
                if (d > 0.0f) { sx += c.x; sy += c.y; ++submerged; }
            }
            if (max_d <= 0.0f) continue; // 完全在水面之上

            float frac;
            if (min_d >= 0.0f) {
                frac = 1.0f; // 完全浸没
            } else {
                frac = max_d / (max_d - min_d);
            }
            frac = math::clamp(frac, 0.0f, 1.0f);
            if (frac <= 0.0f) continue;

            const float area_m2 = (aabb.upperBound.x - aabb.lowerBound.x) *
                                  (aabb.upperBound.y - aabb.lowerBound.y);
            if (area_m2 <= 0.0f) continue;

            // 浮力：方向与重力相反
            const float buoy_mag = f.density * area_m2 * frac * g_m;
            const b2Vec2 buoy{ -g_dir.x * buoy_mag, -g_dir.y * buoy_mag };
            b2Vec2 apply_at{ aabb_cx_px * inv_ppm, aabb_cy_px * inv_ppm };
            if (submerged > 0) {
                apply_at = b2Vec2{ sx / static_cast<float>(submerged),
                                   sy / static_cast<float>(submerged) };
            }
            b2Body_ApplyForce(id, buoy, apply_at, f.wake);

            // 线性阻力：与相对流速成正比（-k * m * Δv * frac）
            const b2Vec2 bv = b2Body_GetLinearVelocity(id);
            const math::Vector2f flow_m = f.flow * inv_ppm;
            const math::Vector2f dv(bv.x - flow_m.x, bv.y - flow_m.y);
            const b2Vec2 drag_force{
                -f.linear_drag * mass * dv.x * frac,
                -f.linear_drag * mass * dv.y * frac,
            };
            b2Body_ApplyForceToCenter(id, drag_force, f.wake);

            // 角阻力：抑制在水中翻滚
            const float inertia = b2Body_GetRotationalInertia(id);
            const float omega = b2Body_GetAngularVelocity(id);
            b2Body_ApplyTorque(id, -f.angular_drag * inertia * omega * frac, f.wake);
        }
    }
}

} // namespace gryce_engine::ecs
