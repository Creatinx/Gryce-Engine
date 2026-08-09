#include "ecs/systems/subviewport_system.h"

#include <cmath>
#include <vector>

#include "components/camera.h"
#include "components/light.h"
#include "components/subviewport.h"
#include "components/2d/sprite_2d.h"
#include "components/transform.h"
#include "ecs/query.h"
#include "math/camera.h"
#include "math/math.h"
#include "render/render_context.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::ecs {

namespace {

constexpr float k_rad_to_deg = 180.0f / 3.14159265358979323846f;

// 由实体 Transform + Camera 组件推导 math::Camera（与 render_api 一致）
bool build_camera(scene::Entity* entity, int vp_w, int vp_h, math::Camera& out) {
    if (!entity) return false;
    auto* cam = entity->get_component<components::Camera>();
    auto* t = entity->transform();
    if (!cam || !cam->enabled || !t) return false;

    const math::Vector3f fwd =
        t->rotation.rotate_vector(math::Vector3f(0.0f, 0.0f, -1.0f)).normalized();
    const float pitch = std::asin(math::clamp(fwd.y, -1.0f, 1.0f));
    const float yaw = std::atan2(fwd.z, fwd.x);

    out.set_position(t->position);
    out.set_yaw(yaw * k_rad_to_deg);
    out.set_pitch(pitch * k_rad_to_deg);
    out.set_fov(cam->fov);
    out.set_near_far(cam->near_plane, cam->far_plane);
    out.set_aspect(vp_h > 0 ? static_cast<float>(vp_w) / static_cast<float>(vp_h) : 16.0f / 9.0f);
    return true;
}

// 收集场景光源（最多 8 盏，MVP 简化版）
void collect_lights(scene::Scene& scene, std::vector<render::RenderPipeline::Light>& out) {
    out.clear();
    scene.foreach([&](scene::Entity* entity) {
        if (!entity || out.size() >= render::RenderPipeline::k_max_lights) return;
        auto* light = entity->get_component<components::Light>();
        if (!light || !light->enabled) return;
        render::RenderPipeline::Light l;
        l.type = static_cast<render::RenderPipeline::LightType>(light->light_type);
        l.direction = light->direction;
        l.color = light->color;
        l.intensity = light->intensity;
        l.range = light->range;
        l.spot_angle = light->spot_angle;
        l.spot_softness = light->spot_softness;
        auto* t = entity->transform();
        l.position = t ? t->position : math::Vector3f::zero();
        out.push_back(l);
    });
}

} // namespace

void SubViewportSystem::on_render(scene::Scene& scene, render::RenderContext& ctx) {
    components::SubViewport* viewport = nullptr;
    foreach_with_component<components::SubViewport>(scene, [&](scene::Entity*, components::SubViewport* v) {
        if (!viewport && v && v->enabled) viewport = v;
    });
    if (!viewport) return;

    // 渲染线程运行时无法从主线程创建 GL 资源（离屏输出当前仅 GL 支持）
    if (ctx.is_running()) {
        GLOG_DEBUG("SubViewportSystem: skipped (render thread running)");
        return;
    }

    if (!pipeline_ && !init_attempted_) {
        init_attempted_ = true;
        auto p = std::make_unique<render::RenderPipeline>();
        p->set_viewport_output_enabled(true);
        p->set_hdr_enabled(true);
        p->set_viewport(viewport->width, viewport->height);
        if (!p->init(&ctx, "res:/shaders")) {
            GLOG_WARN("SubViewportSystem: offscreen pipeline init failed (OpenGL-only)");
            return;
        }
        pipeline_ = std::move(p);
        last_width_ = viewport->width;
        last_height_ = viewport->height;
    }
    if (!pipeline_) return;

    // 尺寸变化：整体重建管线（MVP）
    if (viewport->width != last_width_ || viewport->height != last_height_) {
        pipeline_->shutdown();
        pipeline_ = std::make_unique<render::RenderPipeline>();
        pipeline_->set_viewport_output_enabled(true);
        pipeline_->set_hdr_enabled(true);
        pipeline_->set_viewport(viewport->width, viewport->height);
        if (!pipeline_->init(&ctx, "res:/shaders")) {
            pipeline_.reset();
            GLOG_WARN("SubViewportSystem: offscreen pipeline resize failed");
            return;
        }
        last_width_ = viewport->width;
        last_height_ = viewport->height;
    }

    // 选择相机：指定名字 > 场景第一个启用的 Camera
    scene::Entity* cam_entity = nullptr;
    if (!viewport->camera_name.empty()) {
        cam_entity = scene.find_entity_by_name(viewport->camera_name);
    }
    if (!cam_entity) {
        scene.foreach([&](scene::Entity* e) {
            if (!cam_entity && e) {
                auto* c = e->get_component<components::Camera>();
                if (c && c->enabled) cam_entity = e;
            }
        });
    }

    math::Camera cam;
    if (!build_camera(cam_entity, viewport->width, viewport->height, cam)) return;

    std::vector<render::RenderPipeline::Light> lights;
    collect_lights(scene, lights);
    pipeline_->set_viewport(viewport->width, viewport->height);
    pipeline_->set_camera(cam);
    pipeline_->set_lights(lights);
    pipeline_->set_ambient(math::Vector3f(0.15f, 0.15f, 0.15f));
    pipeline_->render_scene(scene, ctx);

    // 离屏管线结束时 framebuffer 停在 viewport_fbo_，必须切回默认目标，
    // 否则后续 RenderSystem2D 会画到子视口纹理上。
    ctx.set_framebuffer(render::RHIFramebufferHandle{});

    viewport->texture_handle = pipeline_->viewport_color_handle();
    if (!viewport->sprite_entity_name.empty() && viewport->texture_handle.is_valid()) {
        if (scene::Entity* spr = scene.find_entity_by_name(viewport->sprite_entity_name)) {
            if (auto* sprite = spr->get_component<components::d2::sprite::Sprite2D>()) {
                sprite->runtime_texture = viewport->texture_handle;
            }
        }
    }
}

} // namespace gryce_engine::ecs
