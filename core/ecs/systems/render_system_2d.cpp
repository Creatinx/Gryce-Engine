#include "ecs/systems/render_system_2d.h"

#include <algorithm>
#include <vector>

#include "components/2d/camera_2d.h"
#include "components/2d/skybox_2d.h"
#include "components/2d/ambient_light_2d.h"
#include "components/2d/component_2d.h"
#include "components/2d/light_2d.h"
#include "ecs/query.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

void RenderSystem2D::on_render(scene::Scene& scene, render::RenderContext& /*ctx*/) {
    if (!renderer_) return;

    // 每一帧都必须重绘 2D：swapchain 图像每帧都会被 clear 并重新合成
    // （3D 场景每帧重画），若跳过“画面未变化”的帧，该帧将完全没有 2D
    // 内容，表现为 2D 元素（HUD、文字、面板）闪烁。
    rendered_last_frame_ = true;

    // 查找并设置活动摄像机；场景没有 Camera2D 时保留应用层通过
    // renderer2d->set_camera() 设置的摄像机，不要重置回原点——否则
    // 像素坐标 HUD（如 FPS 标签）会被绘制到屏幕中心附近。
    components::d2::camera::Camera2D* active_camera = nullptr;
    foreach_with_component<components::d2::camera::Camera2D>(scene, [&](scene::Entity* /*e*/, components::d2::camera::Camera2D* cam) {
        if (cam && cam->enabled && cam->is_active && !active_camera) {
            active_camera = cam;
        }
    });
    if (active_camera) {
        renderer_->set_camera(active_camera->center(), active_camera->zoom);
    }

    // 重置光照状态（环境光 + 点光源），避免上一帧数据残留
    renderer_->reset_lights();

    // 收集环境光：优先使用 AmbientLight2D 组件，否则使用 renderer 默认值
    {
        render::Color ambient = render::Color::black();
        bool has_ambient = false;
        foreach_with_component<components::d2::light::AmbientLight2D>(scene, [&](scene::Entity* /*e*/, components::d2::light::AmbientLight2D* al) {
            if (!al || !al->enabled || has_ambient) return;
            ambient = render::Color(
                al->color.r * al->intensity,
                al->color.g * al->intensity,
                al->color.b * al->intensity,
                al->color.a);
            has_ambient = true;
        });
        renderer_->set_ambient_light(ambient);
    }

    // 收集 2D 光源，统一使用世界空间描述（Shader 内部处理坐标系）
    foreach_with_component<components::d2::light::Light2D>(scene, [&](scene::Entity* entity, components::d2::light::Light2D* light) {
        if (!light || !light->enabled) return;

        render::Light2D l;
        l.color = light->color;
        l.intensity = light->intensity;
        l.radius = light->radius;
        l.range = light->range;
        l.spot_angle = light->spot_angle;
        l.spot_softness = light->spot_softness;

        auto p = entity->transform()->position;
        l.position = math::Vector2f(p.x, p.y);
        l.direction = light->direction;
        if (l.direction.length_sq() < 1e-6f) {
            l.direction = math::Vector2f(0.0f, -1.0f);
        } else {
            l.direction = l.direction.normalized();
        }

        switch (light->light_type) {
        case components::d2::light::Light2D::LightType::Point:
            l.type = render::LightType2D::Point;
            break;
        case components::d2::light::Light2D::LightType::Directional:
            l.type = render::LightType2D::Directional;
            break;
        case components::d2::light::Light2D::LightType::Spot:
            l.type = render::LightType2D::Spot;
            break;
        }

        renderer_->add_light(l);
    });

    // 先绘制天空盒（最底层背景）
    math::Vector2f saved_center = renderer_->camera_center();
    float saved_zoom = renderer_->camera_zoom();
    foreach_with_component<components::d2::skybox::Skybox2D>(scene, [&](scene::Entity* /*e*/, components::d2::skybox::Skybox2D* sky) {
        if (sky && sky->enabled) {
            renderer_->set_camera(saved_center, saved_zoom);
            sky->draw(renderer_);
        }
    });

    // 按 render_order 排序后再绘制，防止背景盖住文字/UI。
    std::vector<components::d2::Component2D*> comps;
    foreach_with_component<components::d2::Component2D>(scene, [&](scene::Entity* /*e*/, components::d2::Component2D* comp) {
        if (comp->enabled) {
            comps.push_back(comp);
        }
    });

    std::sort(comps.begin(), comps.end(), [](components::d2::Component2D* a, components::d2::Component2D* b) {
        return a->render_order < b->render_order;
    });

    constexpr int k_ui_layer = 1000;

    for (auto* comp : comps) {
        // UI 层（render_order >= 1000）使用屏幕空间，不受摄像机影响
        if (comp->render_order >= k_ui_layer) {
            renderer_->set_camera(math::Vector2f::zero(), 1.0f);
        } else {
            renderer_->set_camera(saved_center, saved_zoom);
        }
        comp->draw(renderer_);
    }

    // 恢复摄像机状态
    renderer_->set_camera(saved_center, saved_zoom);
}

} // namespace gryce_engine::ecs
