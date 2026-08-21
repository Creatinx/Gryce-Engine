#pragma once

#include <memory>
#include <vector>

#include "render/export.h"
#include "render/render2d.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// RendererCanvasRender — 2D 画布渲染器抽象接口
// 类似于 Godot 的 RendererCanvasRender，所有 2D 渲染器实现此接口。
// 包装现有 IRenderer2D 接口并添加 Godot 风格的 Canvas 功能。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererCanvasRender {
public:
    virtual ~RendererCanvasRender() = default;

    // 生命周期
    virtual bool init(RenderContext* ctx) = 0;
    virtual void shutdown() = 0;

    // 每帧生命周期
    virtual void begin_frame(float screen_width, float screen_height) = 0;
    virtual void end_frame() = 0;

    // 混合模式
    virtual void set_blend_mode(BlendMode mode) = 0;

    // 摄像机
    virtual void set_camera(const math::Vector2f& center, float zoom,
                            bool top_left_origin = false, float rotation = 0.0f) = 0;
    virtual math::Vector2f camera_center() const = 0;
    virtual float camera_zoom() const = 0;
    virtual math::Vector2f world_to_screen(const math::Vector2f& world) const = 0;
    virtual math::Vector2f screen_size() const = 0;

    // 2D 图元绘制
    virtual void draw_rect(float x, float y, float w, float h, const Color& color) = 0;
    virtual void draw_polygon(const std::vector<math::Vector2f>& points, const Color& color) = 0;
    virtual void draw_circle(float cx, float cy, float r, int segments, const Color& color) = 0;
    virtual void draw_text(float x, float y, const std::string& text, float font_size, const Color& color) = 0;

    // 2D 精灵
    virtual void draw_sprite(float x, float y, float w, float h,
                             RHITextureHandle texture, const Color& tint = Color::white()) = 0;
    virtual void draw_sprite_region(float x, float y, float w, float h,
                                    float u0, float v0, float u1, float v1,
                                    RHITextureHandle texture, const Color& tint = Color::white()) = 0;
    virtual void draw_sprite_rotated(float x, float y, float w, float h, float rotation,
                                     RHITextureHandle texture, const Color& tint = Color::white()) = 0;

    // 2D 光照
    virtual void set_ambient_light(const Color& color) = 0;
    virtual void add_light(const Light2D& light) = 0;
    virtual void reset_lights() = 0;
    virtual void draw_lit_sprite(float x, float y, float w, float h,
                                 RHITextureHandle albedo, RHITextureHandle normal_map,
                                 const Color& tint = Color::white()) = 0;

    // 2D 阴影
    virtual void draw_shadow_caster(float x, float y, float w, float h) = 0;

    // Bloom
    virtual void set_bloom(const BloomParams& params) = 0;

    // 纹理
    virtual RHITextureHandle create_texture_from_data(const class assets::TextureData* data) = 0;
    virtual class ITexture* resolve_texture(RHITextureHandle handle) const = 0;

    // 访问底层 IRenderer2D（兼容现有代码）
    virtual IRenderer2D* native_renderer2d() = 0;
};

} // namespace gryce_engine::render