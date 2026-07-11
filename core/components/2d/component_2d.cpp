#include "component_2d.h"

#include "components/transform.h"
#include "scene/entity.h"

namespace gryce_engine::components::d2 {

uint64_t Component2D::render_hash() const {
    uint64_t h = hash_string(type());
    hash_combine(h, static_cast<uint64_t>(enabled));
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
    if (!owner_) return math::Vector2f::zero();
    auto* t = owner_->transform();
    if (!t) return math::Vector2f::zero();
    return math::Vector2f(t->position.x, t->position.y);
}

float Component2D::rotation() const {
    if (!owner_) return 0.0f;
    auto* t = owner_->transform();
    if (!t) return 0.0f;
    return 0.0f; // TODO: 从 Quaternion 提取 Z 轴旋转
}

math::Vector2f Component2D::scale() const {
    if (!owner_) return math::Vector2f::one();
    auto* t = owner_->transform();
    if (!t) return math::Vector2f::one();
    return math::Vector2f(t->scale.x, t->scale.y);
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
