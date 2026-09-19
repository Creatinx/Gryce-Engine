#include "ecs/systems/render_system_2d.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "components/2d/camera_2d.h"
#include "components/2d/skybox_2d.h"
#include "components/2d/ambient_light_2d.h"
#include "components/2d/component_2d.h"
#include "components/2d/light_2d.h"
#include "components/node2d.h"
#include "scene/query.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

void RenderSystem2D::on_render(scene::Scene& scene, render::RenderContext& /*ctx*/) {
    if (!renderer_) return;

    // 每一帧都必须重绘 2D：swapchain 图像每帧都会被 clear 并重新合成
    // （3D 场景每帧重画），若跳过“画面未变化”的帧，该帧将完全没有 2D
    // 内容，表现为 2D 元素（HUD、文字、面板）闪烁。
    rendered_last_frame_ = true;

    constexpr int k_ui_layer = 1000;

    // 收集所有启用的 2D 组件
    std::vector<components::d2::Component2D*> comps;
    foreach_with_component<components::d2::Component2D>(scene, [&](scene::Entity* /*e*/, components::d2::Component2D* comp) {
        if (comp->enabled) {
            comps.push_back(comp);
        }
    });

    // 有效层（Godot CanvasLayer 语义）：canvas_layer 升序分组绘制；
    // 兼容旧场景：canvas_layer 默认 0，render_order >= 1000 的旧 UI 组件
    // 仍归入屏幕空间 UI 层。
    auto effective_layer = [&](components::d2::Component2D* c) -> int {
        int layer = c->canvas_layer;
        if (layer == 0 && c->render_order >= k_ui_layer) layer = k_ui_layer;
        return layer;
    };

    // 查找某层的活动摄像机（Camera2D 的 canvas_layer 决定它控制哪一层）
    auto find_camera = [&](int layer) -> components::d2::camera::Camera2D* {
        components::d2::camera::Camera2D* result = nullptr;
        foreach_with_component<components::d2::camera::Camera2D>(scene, [&](scene::Entity* /*e*/, components::d2::camera::Camera2D* cam) {
            if (!result && cam && cam->enabled && cam->is_active && effective_layer(cam) == layer) {
                result = cam;
            }
        });
        return result;
    };

    // 按层分组（层号升序，小号先画/靠底）
    std::map<int, std::vector<components::d2::Component2D*>> layers;
    for (auto* c : comps) {
        layers[effective_layer(c)].push_back(c);
    }

    // 重置光照状态（环境光 + 点光源），避免上一帧数据残留
    renderer_->reset_lights();

    // 2D 光照只作用于世界层（canvas_layer 0），UI/HUD 层不受光照影响
    {
        // 无 AmbientLight2D 时默认全亮（等效未受光，Sprite2D 立即可见）；
        // 显式放置环境光/点光源后由组件控制明暗。
        render::Color ambient = render::Color::white();
        bool has_ambient = false;
        foreach_with_component<components::d2::light::AmbientLight2D>(scene, [&](scene::Entity* /*e*/, components::d2::light::AmbientLight2D* al) {
            if (!al || !al->enabled || has_ambient || effective_layer(al) != 0) return;
            ambient = render::Color(
                al->color.r * al->intensity,
                al->color.g * al->intensity,
                al->color.b * al->intensity,
                al->color.a);
            has_ambient = true;
        });
        renderer_->set_ambient_light(ambient);
    }

    foreach_with_component<components::d2::light::Light2D>(scene, [&](scene::Entity* entity, components::d2::light::Light2D* light) {
        if (!light || !light->enabled || effective_layer(light) != 0) return;

        render::Light2D l;
        l.color = light->color;
        l.intensity = light->intensity;
        l.radius = light->radius;
        l.range = light->range;
        l.spot_angle = light->spot_angle;
        l.spot_softness = light->spot_softness;

        // 使用 2D 世界位置（沿父链组合，top_level 截止）
        auto p = components::d2::world_transform_2d(entity).position;
        l.position = p;
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

    math::Vector2f saved_center = renderer_->camera_center();
    float saved_zoom = renderer_->camera_zoom();
    float saved_rotation = renderer_->camera_rotation();

    // 天空盒属于世界层背景：以世界层摄像机先画
    if (layers.count(0) > 0) {
        auto* world_cam = find_camera(0);
        if (world_cam) {
            renderer_->set_camera(world_cam->center(), world_cam->zoom, false, world_cam->rotation);
        }
        foreach_with_component<components::d2::skybox::Skybox2D>(scene, [&](scene::Entity* /*e*/, components::d2::skybox::Skybox2D* sky) {
            if (sky && sky->enabled && effective_layer(sky) == 0) {
                sky->draw(renderer_);
            }
        });
    }

    // 逐层绘制：层内排序规则（升序，越小越先画/越靠底）
    //   1. Component2D::render_order；
    //   2. owner 挂有 Node2D 时的 z_index（无 Node2D 视为 0）；
    //   3. stable_sort 保持收集顺序（同层级按场景遍历顺序）。
    auto z_index_of = [](components::d2::Component2D* c) {
        auto* owner = c->owner();
        auto* n2d = owner ? owner->get_component<components::Node2D>() : nullptr;
        return n2d ? n2d->z_index : 0;
    };

    for (auto& [layer, layer_comps] : layers) {
        std::stable_sort(layer_comps.begin(), layer_comps.end(),
            [&](components::d2::Component2D* a, components::d2::Component2D* b) {
                if (a->render_order != b->render_order) {
                    return a->render_order < b->render_order;
                }
                return z_index_of(a) < z_index_of(b);
            });

        auto* cam = find_camera(layer);
        if (cam) {
            renderer_->set_camera(cam->center(), cam->zoom, false, cam->rotation);
        } else if (layer != 0) {
            // 无相机的非世界层：屏幕空间（UI/HUD），左上角原点
            renderer_->set_camera(math::Vector2f::zero(), 1.0f, true, 0.0f);
        }
    // 世界层（layer 0）无 Camera2D 时保留应用层通过
    // renderer2d->set_camera() 设置的摄像机，不重置回原点。

        for (auto* comp : layer_comps) {
            // 天空盒已在世界层背景统一绘制，跳过避免重复
            if (comp->type() == std::string("Skybox2D")) continue;
            comp->draw(renderer_);
        }
    }

    // 恢复摄像机状态
    renderer_->set_camera(saved_center, saved_zoom, false, saved_rotation);
}

} // namespace gryce_engine::ecs
