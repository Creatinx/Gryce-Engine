#include "render/canvas2d_pipeline.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>

#include "components/ui_root.h"
#include "ecs/systems/render_system_2d.h"
#include "math/camera.h"
#include "render/render2d.h"
#include "render/render_context.h"
#include "render/ui_pipeline.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "scene/query.h"
#include "ui/ui_drawlist.h"

namespace gryce_engine::render {

Canvas2DPipeline::Canvas2DPipeline() = default;
Canvas2DPipeline::~Canvas2DPipeline() {
    shutdown();
}

bool Canvas2DPipeline::initialize(RenderContext* ctx) {
    shutdown();
    if (!ctx) return false;
    ctx_ = ctx;
    renderer_ = ctx->create_renderer2d();
    if (!renderer_) return false;
    renderer_->init(ctx);
    return true;
}

void Canvas2DPipeline::shutdown() {
    destroy_viewport_target();
    if (renderer_) renderer_->shutdown();
    renderer_.reset();
    ctx_ = nullptr;
}

void Canvas2DPipeline::render(scene::Scene* scene,
                              int screen_w, int screen_h,
                              bool draw_fps_fallback,
                              float fps, float frame_ms,
                              const ui::UIInputFrame& ui_input,
                              const math::Camera* camera) {
    if (!renderer_ || !ctx_ || screen_w <= 0 || screen_h <= 0) return;

    bool offscreen = false;
    if (viewport_output_enabled_) {
        offscreen = ensure_viewport_target(screen_w, screen_h);
        if (offscreen) ctx_->set_framebuffer(viewport_framebuffer_);
    }

    renderer_->begin_frame(static_cast<float>(screen_w), static_cast<float>(screen_h));
    std::vector<components::UIRoot*> roots;
    if (scene) {
        ecs::RenderSystem2D system(renderer_.get());
        system.on_render(*scene, *ctx_);

        ecs::foreach_with_component<components::UIRoot>(
            *scene, [&](scene::Entity*, components::UIRoot* root) {
                if (root && root->enabled && root->active && root->is_visible_in_tree()) {
                    roots.push_back(root);
                }
            });
        std::stable_sort(roots.begin(), roots.end(),
                         [](const components::UIRoot* a, const components::UIRoot* b) {
                             return a->z_index < b->z_index;
                         });
    }

    // Finish the Canvas2D scene before submitting UI. In the non-offscreen path
    // the old order ran UI before Renderer2D::end_frame(); D3D12's 2D finalize
    // could then rebind/overwrite the screen target, dropping the whole overlay.
    renderer_->end_frame();
    if (offscreen) {
        // Return to the backbuffer after finishing the Canvas2D scene. UI overlay
        // is drawn afterward and must not be cleared by this transition.
        ctx_->set_framebuffer(RHIFramebufferHandle{});
        ctx_->set_viewport(0, 0, screen_w, screen_h);
        ctx_->clear(0.025f, 0.03f, 0.04f, 1.0f);
    }

    ui::UIInputFrame no_input;
    no_input.delta_time = std::clamp(frame_ms * 0.001f, 0.0001f, 0.1f);
    ui::UIInputFrame ui_frame = ui_input;
    ui_frame.delta_time = no_input.delta_time;
    for (components::UIRoot* root : roots) {
            ui::UIRect viewport{0.0f, 0.0f,
                                static_cast<float>(screen_w),
                                static_cast<float>(screen_h)};
            float layout_scale = 1.0f;
            if (root->space == components::UIRootSpace::World) {
                auto* owner = root->owner();
                if (!camera || !owner) continue;
                const math::Matrix4f view_projection =
                    camera->get_projection_matrix() * camera->get_view_matrix();
                const math::Vector3f world =
                    owner->world_transform().transform_point(math::Vector3f::zero());
                const math::Vector4f clip =
                    view_projection * math::Vector4f(world, 1.0f);
                if (clip.w <= 0.001f) continue;
                const float ndc_x = clip.x / clip.w;
                const float ndc_y = clip.y / clip.w;
                const float screen_x = (ndc_x * 0.5f + 0.5f) * screen_w;
                const float screen_y = (1.0f - (ndc_y * 0.5f + 0.5f)) * screen_h;
                const float half_fov = math::to_radians(camera->fov()) * 0.5f;
                layout_scale = std::max(
                    0.01f,
                    root->world_scale * screen_h /
                        (2.0f * std::tan(half_fov) * clip.w));
                viewport = {screen_x, screen_y,
                            static_cast<float>(screen_w),
                            static_cast<float>(screen_h)};
            }
            root->runtime().set_layout_scale(layout_scale);
            root->runtime().set_viewport(viewport);
            root->runtime().update(root->accepts_input ? ui_frame : no_input);
            ui::UIDrawList draw_list;
            root->runtime().build_draw_list(draw_list);
            if (std::getenv("GRYCE_UI_DIAG")) {
                std::printf("[ui-diag] roots=%zu commands=%zu screen=%dx%d\n",
                            roots.size(), draw_list.size(), screen_w, screen_h);
            }
            render_ui_draw_list(*renderer_, draw_list, screen_w, screen_h);
    }
    if (draw_fps_fallback) {
        renderer_->begin_overlay_pass(screen_w, screen_h);
        char text[96];
        std::snprintf(text, sizeof(text), "FPS %.1f   frame %.2f ms",
                      static_cast<double>(fps), static_cast<double>(frame_ms));
        renderer_->draw_text(12.0f, 30.0f, text, 22.0f,
                             Color(1.0f, 1.0f, 1.0f, 1.0f));
        renderer_->end_overlay_pass();
    }
}

void Canvas2DPipeline::ensure_text_raster_size(float physical_px) {
    if (renderer_) renderer_->ensure_text_raster_size(physical_px);
}

bool Canvas2DPipeline::ensure_viewport_target(int width, int height) {
    if (!ctx_ || width <= 0 || height <= 0) return false;
    if (viewport_framebuffer_.is_valid() &&
        viewport_width_ == width && viewport_height_ == height) {
        return true;
    }

    const bool paused = ctx_->is_running();
    if (paused) ctx_->pause_render_thread();
    destroy_viewport_target();

    viewport_texture_ = ctx_->create_texture();
    ITexture* texture = ctx_->texture(viewport_texture_);
    bool ok = texture &&
              texture->create(TextureFormat::RGBA8, width, height, nullptr);
    if (ok) {
        texture->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        texture->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
        viewport_framebuffer_ = ctx_->create_framebuffer();
        IFramebuffer* framebuffer = ctx_->framebuffer(viewport_framebuffer_);
        ok = framebuffer && framebuffer->create(width, height);
        if (ok) {
            framebuffer->attach_color_texture(texture);
            ok = framebuffer->is_complete();
        }
    }

    if (ok) {
        viewport_width_ = width;
        viewport_height_ = height;
    } else {
        destroy_viewport_target();
    }
    if (paused) ctx_->resume_render_thread();
    return ok;
}

void Canvas2DPipeline::destroy_viewport_target() {
    if (ctx_) {
        const bool paused = ctx_->is_running();
        if (paused) ctx_->pause_render_thread();
        if (viewport_framebuffer_.is_valid()) {
            ctx_->destroy_framebuffer(viewport_framebuffer_);
            viewport_framebuffer_ = RHIFramebufferHandle{};
        }
        if (viewport_texture_.is_valid()) {
            ctx_->destroy_texture(viewport_texture_);
            viewport_texture_ = RHITextureHandle{};
        }
        if (paused) ctx_->resume_render_thread();
    }
    viewport_width_ = 0;
    viewport_height_ = 0;
}

} // namespace gryce_engine::render
