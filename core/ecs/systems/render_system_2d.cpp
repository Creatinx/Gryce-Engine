#include "ecs/systems/render_system_2d.h"

#include <algorithm>
#include <vector>

#include "components/2d/camera_2d.h"

using gryce_engine::components::d2::hash_combine;
#include "components/2d/component_2d.h"
#include "components/2d/light_2d.h"
#include "ecs/query.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

uint64_t RenderSystem2D::compute_scene_hash(scene::Scene& scene) {
    uint64_t h = 0;

    // 活动摄像机
    foreach_with_component<components::d2::camera::Camera2D>(scene, [&](scene::Entity* /*e*/, components::d2::camera::Camera2D* cam) {
        if (cam && cam->enabled) {
            hash_combine(h, cam->render_hash());
        }
    });

    // 光源
    foreach_with_component<components::d2::light::Light2D>(scene, [&](scene::Entity* /*e*/, components::d2::light::Light2D* light) {
        if (light && light->enabled) {
            hash_combine(h, light->render_hash());
        }
    });

    // 所有可见 2D 组件
    foreach_with_component<components::d2::Component2D>(scene, [&](scene::Entity* /*e*/, components::d2::Component2D* comp) {
        if (comp && comp->enabled) {
            hash_combine(h, comp->render_hash());
        }
    });

    return h;
}

void RenderSystem2D::on_render(scene::Scene& scene, render::RenderContext& /*ctx*/) {
    if (!renderer_) return;

    // Dirty-Frame 优化：画面状态未变化时直接跳过本帧渲染
    uint64_t hash = compute_scene_hash(scene);
    if (!first_frame_ && hash == last_hash_) {
        rendered_last_frame_ = false;
        return;
    }
    last_hash_ = hash;
    first_frame_ = false;
    rendered_last_frame_ = true;

    // 查找并设置活动摄像机
    components::d2::camera::Camera2D* active_camera = nullptr;
    foreach_with_component<components::d2::camera::Camera2D>(scene, [&](scene::Entity* /*e*/, components::d2::camera::Camera2D* cam) {
        if (cam && cam->enabled && cam->is_active && !active_camera) {
            active_camera = cam;
        }
    });
    if (active_camera) {
        renderer_->set_camera(active_camera->center(), active_camera->zoom);
    } else {
        renderer_->set_camera(math::Vector2f::zero(), 1.0f);
    }

    // 收集 2D 光源，point light 需要从世界坐标转换到屏幕坐标
    renderer_->reset_lights();
    foreach_with_component<components::d2::light::Light2D>(scene, [&](scene::Entity* entity, components::d2::light::Light2D* light) {
        if (!light || !light->enabled || light->light_type != components::d2::light::Light2D::LightType::Point) return;
        auto p = entity->transform()->position;
        math::Vector2f screen_pos = renderer_->world_to_screen(math::Vector2f(p.x, p.y));
        renderer_->add_point_light(screen_pos, light->radius * renderer_->camera_zoom(), light->color, light->intensity);
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
    math::Vector2f saved_center = renderer_->camera_center();
    float saved_zoom = renderer_->camera_zoom();

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
