#include "component_2d.h"

#include <cmath>

#include "components/node2d.h"
#include "components/transform.h"
#include "scene/entity.h"

namespace gryce_engine::components::d2 {

namespace {

// 从四元数提取绕 Z 轴的旋转角（与 PhysicsSystem2D 的 quat_to_z 保持一致）
float quat_to_z(const math::Quaternionf& q) {
    return std::atan2(2.0f * (q.w * q.z + q.x * q.y),
                      1.0f - 2.0f * (q.y * q.y + q.z * q.z));
}

math::Vector2f rotate_2d(const math::Vector2f& v, float angle) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    return math::Vector2f(v.x * c - v.y * s, v.x * s + v.y * c);
}

// 读取实体本地 Transform 的 2D 部分
Transform2D local_transform_2d(const scene::Entity* entity) {
    Transform2D out;
    auto* t = entity ? entity->transform() : nullptr;
    if (!t) return out;
    out.position = math::Vector2f(t->position.x, t->position.y);
    out.rotation = quat_to_z(t->rotation);
    out.scale = math::Vector2f(t->scale.x, t->scale.y);
    return out;
}

} // namespace

Transform2D world_transform_2d(const scene::Entity* entity) {
    Transform2D local = local_transform_2d(entity);
    if (!entity) return local;

    // Godot top_level 语义：忽略整条父链
    if (auto* n2d = entity->get_component<components::Node2D>(); n2d && n2d->top_level) {
        return local;
    }

    const scene::Entity* parent = entity->parent();
    if (!parent) return local;

    // 递归组合父级世界变换（top_level 祖先在递归中自然截断其父链）
    const Transform2D pw = world_transform_2d(parent);
    Transform2D world;
    world.position = pw.position + rotate_2d(
        math::Vector2f(local.position.x * pw.scale.x, local.position.y * pw.scale.y),
        pw.rotation);
    world.rotation = pw.rotation + local.rotation;
    world.scale = math::Vector2f(pw.scale.x * local.scale.x, pw.scale.y * local.scale.y);
    return world;
}

uint64_t Component2D::render_hash() const {
    uint64_t h = hash_string(type());
    hash_combine(h, static_cast<uint64_t>(enabled));
    hash_combine(h, static_cast<uint64_t>(canvas_layer));
    hash_combine(h, static_cast<uint64_t>(render_order));
    math::Vector2f p = position();
    hash_combine(h, hash_float(p.x));
    hash_combine(h, hash_float(p.y));
    math::Vector2f s = scale();
    hash_combine(h, hash_float(s.x));
    hash_combine(h, hash_float(s.y));
    hash_combine(h, hash_float(rotation()));
    return h;
}

math::Vector2f Component2D::position() const {
    return world_transform_2d(owner_).position;
}

float Component2D::rotation() const {
    return world_transform_2d(owner_).rotation;
}

math::Vector2f Component2D::scale() const {
    return world_transform_2d(owner_).scale;
}

math::Vector2f Component2D::transform_point(const math::Vector2f& local) const {
    math::Vector2f world = position();
    math::Vector2f s = scale();
    return math::Vector2f(
        world.x + local.x * s.x,
        world.y + local.y * s.y
    );
}

} // namespace gryce_engine::components::d2
