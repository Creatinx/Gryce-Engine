#include "render/renderer_canvas_render_rd.h"
#include "render/render_context.h"
#include "render/render2d.h"

namespace gryce_engine::render {

RendererCanvasRenderRD::RendererCanvasRenderRD() = default;
RendererCanvasRenderRD::~RendererCanvasRenderRD() {
    shutdown();
}

bool RendererCanvasRenderRD::init(RenderContext* ctx) {
    if (initialized_) return true;
    ctx_ = ctx;
    renderer2d_ = ctx->create_renderer2d();
    if (!renderer2d_) {
        return false;
    }
    renderer2d_->init(ctx);
    initialized_ = true;
    return true;
}

void RendererCanvasRenderRD::shutdown() {
    if (initialized_) {
        if (renderer2d_) {
            renderer2d_->shutdown();
        }
        initialized_ = false;
    }
}

void RendererCanvasRenderRD::begin_frame(float screen_width, float screen_height) {
    if (renderer2d_) {
        renderer2d_->begin_frame(screen_width, screen_height);
    }
}

void RendererCanvasRenderRD::end_frame() {
    if (renderer2d_) {
        renderer2d_->end_frame();
    }
}

void RendererCanvasRenderRD::set_blend_mode(BlendMode mode) {
    if (renderer2d_) {
        renderer2d_->set_blend_mode(mode);
    }
}

void RendererCanvasRenderRD::set_camera(const math::Vector2f& center, float zoom,
                                         bool top_left_origin, float rotation) {
    if (renderer2d_) {
        renderer2d_->set_camera(center, zoom, top_left_origin, rotation);
    }
}

math::Vector2f RendererCanvasRenderRD::camera_center() const {
    return renderer2d_ ? renderer2d_->camera_center() : math::Vector2f::zero();
}

float RendererCanvasRenderRD::camera_zoom() const {
    return renderer2d_ ? renderer2d_->camera_zoom() : 1.0f;
}

math::Vector2f RendererCanvasRenderRD::world_to_screen(const math::Vector2f& world) const {
    return renderer2d_ ? renderer2d_->world_to_screen(world) : world;
}

math::Vector2f RendererCanvasRenderRD::screen_size() const {
    return renderer2d_ ? renderer2d_->screen_size() : math::Vector2f::zero();
}

void RendererCanvasRenderRD::draw_rect(float x, float y, float w, float h, const Color& color) {
    if (renderer2d_) {
        renderer2d_->draw_rect(x, y, w, h, color);
    }
}

void RendererCanvasRenderRD::draw_polygon(const std::vector<math::Vector2f>& points, const Color& color) {
    if (renderer2d_) {
        renderer2d_->draw_polygon(points, color);
    }
}

void RendererCanvasRenderRD::draw_circle(float cx, float cy, float r, int segments, const Color& color) {
    if (renderer2d_) {
        renderer2d_->draw_circle(cx, cy, r, segments, color);
    }
}

void RendererCanvasRenderRD::draw_text(float x, float y, const std::string& text, float font_size, const Color& color) {
    if (renderer2d_) {
        renderer2d_->draw_text(x, y, text, font_size, color);
    }
}

void RendererCanvasRenderRD::draw_sprite(float x, float y, float w, float h,
                                          RHITextureHandle texture, const Color& tint) {
    if (renderer2d_) {
        renderer2d_->draw_sprite(x, y, w, h, texture, tint);
    }
}

void RendererCanvasRenderRD::draw_sprite_region(float x, float y, float w, float h,
                                                 float u0, float v0, float u1, float v1,
                                                 RHITextureHandle texture, const Color& tint) {
    if (renderer2d_) {
        renderer2d_->draw_sprite_region(x, y, w, h, u0, v0, u1, v1, texture, tint);
    }
}

void RendererCanvasRenderRD::draw_sprite_rotated(float x, float y, float w, float h, float rotation,
                                                   RHITextureHandle texture, const Color& tint) {
    if (renderer2d_) {
        renderer2d_->draw_sprite_rotated(x, y, w, h, rotation, texture, tint);
    }
}

void RendererCanvasRenderRD::set_ambient_light(const Color& color) {
    if (renderer2d_) {
        renderer2d_->set_ambient_light(color);
    }
}

void RendererCanvasRenderRD::add_light(const Light2D& light) {
    if (renderer2d_) {
        renderer2d_->add_light(light);
    }
}

void RendererCanvasRenderRD::reset_lights() {
    if (renderer2d_) {
        renderer2d_->reset_lights();
    }
}

void RendererCanvasRenderRD::draw_lit_sprite(float x, float y, float w, float h,
                                              RHITextureHandle albedo, RHITextureHandle normal_map,
                                              const Color& tint) {
    if (renderer2d_) {
        renderer2d_->draw_lit_sprite(x, y, w, h, albedo, normal_map, tint);
    }
}

void RendererCanvasRenderRD::draw_shadow_caster(float x, float y, float w, float h) {
    if (renderer2d_) {
        renderer2d_->draw_shadow_caster(x, y, w, h);
    }
}

void RendererCanvasRenderRD::set_bloom(const BloomParams& params) {
    if (renderer2d_) {
        renderer2d_->set_bloom(params);
    }
}

RHITextureHandle RendererCanvasRenderRD::create_texture_from_data(const assets::TextureData* data) {
    return renderer2d_ ? renderer2d_->create_texture_from_data(data) : RHITextureHandle{};
}

ITexture* RendererCanvasRenderRD::resolve_texture(RHITextureHandle handle) const {
    return renderer2d_ ? renderer2d_->resolve_texture(handle) : nullptr;
}

} // namespace gryce_engine::render