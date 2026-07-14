#pragma once

#include <memory>
#include <vector>

#include "render/render2d.h"
#include "render/render_context.h"
#include "render/font_atlas.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// Vertex2D — 2D 顶点格式（位置 + 颜色 + 纹理坐标）
// ---------------------------------------------------------------------------
struct Vertex2D {
    float x, y;
    float r, g, b, a;
    float u, v;
};

// ---------------------------------------------------------------------------
// LitVertex2D — 受光照 2D 顶点格式（位置 + 颜色 + albedo UV + normal UV）
// ---------------------------------------------------------------------------
struct LitVertex2D {
    float x, y;
    float r, g, b, a;
    float u, v;    // albedo UV
    float nu, nv;  // normal UV
};

// ---------------------------------------------------------------------------
// ShadowCasterVertex — 阴影遮挡物顶点（仅位置）
// ---------------------------------------------------------------------------
struct ShadowCasterVertex {
    float x, y;
};

// ---------------------------------------------------------------------------
// Renderer2D — 2D 渲染器实现
// 使用动态顶点 buffer，每帧收集所有 2D 图形并一次性提交
// ---------------------------------------------------------------------------
class Renderer2D : public IRenderer2D {
public:
    Renderer2D();
    ~Renderer2D() override;

    void init(RenderContext* ctx) override;
    void shutdown() override;

    void begin_frame(float screen_width, float screen_height) override;
    void end_frame() override;

    void set_camera(const math::Vector2f& center, float zoom) override;
    math::Vector2f camera_center() const override { return camera_center_; }
    float camera_zoom() const override { return camera_zoom_; }
    math::Vector2f world_to_screen(const math::Vector2f& world) const override;
    math::Vector2f screen_size() const override { return math::Vector2f(screen_width_, screen_height_); }

    void draw_rect(float x, float y, float w, float h, const Color& color) override;
    void draw_polygon(const std::vector<math::Vector2f>& points, const Color& color) override;
    void draw_circle(float cx, float cy, float r, int segments, const Color& color) override;
    void draw_text(float x, float y, const std::string& text, float font_size, const Color& color) override;

    // 2D 光照接口
    void set_ambient_light(const Color& color) override;
    void add_light(const Light2D& light) override;
    void reset_lights() override;
    void draw_sprite(float x, float y, float w, float h,
                      ITexture* texture, const Color& tint = Color::white()) override;
    void draw_sprite_region(float x, float y, float w, float h,
                             float u0, float v0, float u1, float v1,
                             ITexture* texture, const Color& tint = Color::white()) override;
    void draw_lit_sprite(float x, float y, float w, float h,
                          ITexture* albedo, ITexture* normal_map,
                          const Color& tint = Color::white()) override;
    void draw_lit_sprite_region(float x, float y, float w, float h,
                                 float u0, float v0, float u1, float v1,
                                 ITexture* albedo, ITexture* normal_map,
                                 const Color& tint = Color::white(),
                                 float nu0 = 0.0f, float nv0 = 0.0f,
                                 float nu1 = 1.0f, float nv1 = 1.0f) override;

    // 阴影遮挡物（用于 2D 硬阴影）
    void draw_shadow_caster(float x, float y, float w, float h) override;

    // Bloom 后处理
    void set_bloom(const BloomParams& params) override;

    RHITextureHandle create_texture_from_data(const assets::TextureData* data) override;
    ITexture* resolve_texture(RHITextureHandle handle) const override;

private:
    bool context_alive() const;

    void push_vertex(float x, float y, const Color& color, float u, float v);
    void push_text_vertex(float x, float y, const Color& color, float u, float v);
    void push_lit_vertex(ITexture* albedo, ITexture* normal,
                         float x, float y, const Color& color,
                         float u, float v, float nu, float nv);
    void push_shadow_caster_vertex(float x, float y);
    void flush_batches();
    void flush_batch(std::vector<Vertex2D>&& verts, bool is_text);

    // 受光照精灵批次：按 (albedo, normal) 纹理分组，减少状态切换
    struct LitBatch {
        ITexture* albedo = nullptr;
        ITexture* normal = nullptr;
        std::vector<LitVertex2D> verts;
    };
    std::vector<LitBatch>::iterator find_lit_batch(ITexture* albedo, ITexture* normal);

    void render_lit_sprites_forward(bool target_is_scene_fbo);
    void render_shadow_pass();
    void render_bloom_pass();

    bool create_shadow_map();
    bool create_bloom_targets();
    void destroy_shadow_map();
    void destroy_bloom_targets();

    RenderContext* ctx_ = nullptr;
    std::shared_ptr<RenderContext::Lifetime> ctx_lifetime_;
    RHIShaderHandle shader_;
    RHIShaderHandle lit_sprite_shader_;
    RHIShaderHandle shadow_shader_;
    RHIShaderHandle bloom_threshold_shader_;
    RHIShaderHandle bloom_blur_shader_;
    RHIShaderHandle bloom_compose_shader_;
    RHIMeshHandle mesh_;
    FontAtlas font_atlas_;
    std::vector<Vertex2D> vertices_;       // 普通图形顶点（无纹理）
    std::vector<Vertex2D> text_vertices_;  // 文字顶点（使用字体图集）
    std::vector<LitBatch> lit_batches_;    // 受光照精灵批次（按纹理分组）
    std::vector<ShadowCasterVertex> shadow_caster_vertices_; // 阴影遮挡物顶点
    math::Matrix4f ortho_;
    math::Matrix4f view_proj_;
    math::Vector2f camera_center_ = math::Vector2f::zero();
    float camera_zoom_ = 1.0f;
    float screen_width_ = 0.0f;
    float screen_height_ = 0.0f;
    bool initialized_ = false;
    bool using_fallback_font_ = false;

    // 2D 光照
    Color ambient_light_ = Color::black();
    std::vector<Light2D> lights_;

    // 2D 阴影
    RHIFramebufferHandle shadow_fbo_;
    RHITextureHandle shadow_map_;
    static constexpr int k_shadow_map_size = 1024;

    // 2D Bloom
    BloomParams bloom_params_;
    bool bloom_initialized_ = false;
    RHIFramebufferHandle bloom_fbo_a_;
    RHIFramebufferHandle bloom_fbo_b_;
    RHITextureHandle bloom_texture_a_;
    RHITextureHandle bloom_texture_b_;
    RHITextureHandle scene_texture_;
    RHIFramebufferHandle scene_fbo_;
};

} // namespace gryce_engine::render
