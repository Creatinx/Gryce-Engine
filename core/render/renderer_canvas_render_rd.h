#pragma once

#include <memory>

#include "render/export.h"
#include "render/renderer_canvas_render.h"

namespace gryce_engine::render {

class RenderContext;
class IRenderer2D;

// ---------------------------------------------------------------------------
// RendererCanvasRenderRD — 2D 画布渲染器 RD 实现
// 包装现有的 Renderer2D 实现，提供 Godot 风格的 Canvas 渲染接口。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererCanvasRenderRD : public RendererCanvasRender {
public:
    RendererCanvasRenderRD();
    ~RendererCanvasRenderRD() override;

    // RendererCanvasRender 接口
    bool init(RenderContext* ctx) override;
    void shutdown() override;

    void begin_frame(float screen_width, float screen_height) override;
    void end_frame() override;
    void set_blend_mode(BlendMode mode) override;

    void set_camera(const math::Vector2f& center, float zoom,
                    bool top_left_origin = false, float rotation = 0.0f) override;
    math::Vector2f camera_center() const override;
    float camera_zoom() const override;
    math::Vector2f world_to_screen(const math::Vector2f& world) const override;
    math::Vector2f screen_size() const override;

    void draw_rect(float x, float y, float w, float h, const Color& color) override;
    void draw_polygon(const std::vector<math::Vector2f>& points, const Color& color) override;
    void draw_circle(float cx, float cy, float r, int segments, const Color& color) override;
    void draw_text(float x, float y, const std::string& text, float font_size, const Color& color) override;

    void draw_sprite(float x, float y, float w, float h,
                     RHITextureHandle texture, const Color& tint = Color::white()) override;
    void draw_sprite_region(float x, float y, float w, float h,
                            float u0, float v0, float u1, float v1,
                            RHITextureHandle texture, const Color& tint = Color::white()) override;
    void draw_sprite_rotated(float x, float y, float w, float h, float rotation,
                             RHITextureHandle texture, const Color& tint = Color::white()) override;

    void set_ambient_light(const Color& color) override;
    void add_light(const Light2D& light) override;
    void reset_lights() override;
    void draw_lit_sprite(float x, float y, float w, float h,
                         RHITextureHandle albedo, RHITextureHandle normal_map,
                         const Color& tint = Color::white()) override;

    void draw_shadow_caster(float x, float y, float w, float h) override;
    void set_bloom(const BloomParams& params) override;

    RHITextureHandle create_texture_from_data(const class assets::TextureData* data) override;
    class ITexture* resolve_texture(RHITextureHandle handle) const override;

    IRenderer2D* native_renderer2d() override { return renderer2d_.get(); }

private:
    std::unique_ptr<IRenderer2D> renderer2d_;
    RenderContext* ctx_ = nullptr;
    bool initialized_ = false;
};

} // namespace gryce_engine::render