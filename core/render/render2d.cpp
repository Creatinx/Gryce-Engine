#include "render2d.h"

namespace gryce_engine::render {

void IRenderer2D::add_point_light(const math::Vector2f& pos, float radius,
                                   const Color& color, float intensity) {
    Light2D light;
    light.type = LightType2D::Point;
    light.position = pos;
    light.radius = radius;
    light.color = color;
    light.intensity = intensity;
    add_light(light);
}

} // namespace gryce_engine::render
