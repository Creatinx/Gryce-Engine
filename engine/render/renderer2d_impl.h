#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
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
    void set_blend_mode(BlendMode mode) override;

    void set_camera(const math::Vector2f& center, float zoom, bool top_left_origin = false,
                    float rotation = 0.0f) override;
    math::Vector2f camera_center() const override { return camera_center_; }
    float camera_zoom() const override { return camera_zoom_; }
    float camera_rotation() const override { return camera_rotation_; }
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
                      RHITextureHandle texture, const Color& tint = Color::white()) override;
    void draw_sprite_region(float x, float y, float w, float h,
                             float u0, float v0, float u1, float v1,
                             RHITextureHandle texture, const Color& tint = Color::white()) override;
    void draw_sprite_rotated(float x, float y, float w, float h, float rotation,
                              RHITextureHandle texture, const Color& tint = Color::white()) override;
    void draw_lit_sprite(float x, float y, float w, float h,
                          RHITextureHandle albedo, RHITextureHandle normal_map,
                          const Color& tint = Color::white()) override;
    void draw_lit_sprite_region(float x, float y, float w, float h,
                                 float u0, float v0, float u1, float v1,
                                 RHITextureHandle albedo, RHITextureHandle normal_map,
                                 const Color& tint = Color::white(),
                                 float nu0 = 0.0f, float nv0 = 0.0f,
                                 float nu1 = 1.0f, float nv1 = 1.0f) override;

    // 阴影遮挡物（用于 2D 硬阴影）
    void draw_shadow_caster(float x, float y, float w, float h) override;

    // Bloom 后处理
    void set_bloom(const BloomParams& params) override;

    void set_scissor(int x, int y, int w, int h) override;
    void reset_scissor() override;

    float measure_text(const char* text, float font_size) const override;
    void  begin_overlay_pass(int screen_width, int screen_height) override;
    void  end_overlay_pass() override;

    RHITextureHandle create_texture_from_data(const assets::TextureData* data) override;
    ITexture* resolve_texture(RHITextureHandle handle) const override;

protected:
    // ---------------------------------------------------------------------
    // 后端适配钩子（默认实现 = OpenGL 行为，子类可覆盖）
    // Renderer2D 的绝大多数逻辑是后端无关的（批处理、矩阵、光照参数、
    // bloom ping-pong 编排），只有少数几处需要直接触碰后端设备：
    //   * 深度纹理的比较模式（GL 需要关闭 TEXTURE_COMPARE_MODE）
    //   * 创建 1x1 默认 albedo/normal 纹理（GL 用 glGenTextures）
    //   * 仅有深度附件的 FBO 需要禁用颜色读写（GL 的 glDrawBuffer）
    // 这些点在 GL 后端下由 renderer2d_impl.cpp 直接完成；其它后端
    // （D3D12）继承本类并覆盖下列钩子即可复用全部逻辑。
    // ---------------------------------------------------------------------

    // 关闭给定深度纹理的「采样器比较模式」（默认：GL 实现）。
    virtual void configure_depth_texture_for_sampling(ITexture* shadow_map);

    // 为一个仅有深度附件的离屏目标禁用颜色写入（默认：GL 的 glDrawBuffer）。
    virtual void disable_color_writes_for_depth_only_target();

    // 创建 1x1 默认纹理（albedo = 白 / normal = 平面法线）。
    // 返回后端纹理对象 id（GL 为 GLuint）；0 表示创建失败。
    // 默认实现走 GL。参数是 RGBA 四字节。
    virtual uint32_t create_default_1x1_texture(const unsigned char rgba[4]);

    // 删除 create_default_1x1_texture 创建的纹理（默认：glDeleteTextures）。
    virtual void destroy_default_texture(uint32_t id);

    // 把后端纹理 id 绑定到给定纹理槽位（默认：glBindTextureUnit）。
    virtual void bind_default_texture_slot(int slot, uint32_t id);

    // 2D 的离屏 pass（阴影 / bloom）是否直接由本渲染器切换渲染目标。
    // GL：是（GL 的 RT 切换是即时状态，无需 barrier）。
    // D3D12 / Vulkan：否 —— 离屏 RT 由引擎的 pass 负责创建与 barrier，
    //   2D 渲染器只在引擎已经绑好的目标上绘制（避免与命令缓冲的状态跟踪打架）。
    virtual bool use_backend_framebuffers() const { return true; }

    // 子类钩子实现里需要访问的上下文（其余成员仍为 private）。
    RenderContext* ctx() const { return ctx_; }

private:
    bool context_alive() const;

    void push_vertex(float x, float y, const Color& color, float u, float v);
    void push_text_vertex(float x, float y, const Color& color, float u, float v);
    void push_lit_vertex(RHITextureHandle albedo, RHITextureHandle normal,
                         float x, float y, const Color& color,
                         float u, float v, float nu, float nv);
    void push_shadow_caster_vertex(float x, float y);
    void flush_batches();
    void flush_batch(std::vector<Vertex2D>&& verts, bool is_text);

    // 受光照精灵批次：按 (albedo, normal) 纹理句柄分组，减少状态切换。
    // 批内只存句柄（按值拷贝安全），渲染线程执行时经 generation 校验解析，
    // 纹理已销毁则回退默认纹理，杜绝悬垂 ITexture*。
    struct LitBatch {
        RHITextureHandle albedo;
        RHITextureHandle normal;
        std::vector<LitVertex2D> verts;
        // 绘制时捕获的视图矩阵：受光精灵在 end_frame 的 lit pass 才提交，
        // 此时 RenderSystem2D 已把相机恢复为上一帧状态；按批次记录绘制时刻
        // 的矩阵，否则受光内容会画到错误的屏幕位置。
        math::Matrix4f view_proj;
    };
    using LitBatchKey = std::pair<RHITextureHandle, RHITextureHandle>;
    struct LitBatchKeyHash {
        std::size_t operator()(const LitBatchKey& key) const noexcept {
            auto h1 = std::hash<uint32_t>{}(key.first.index) ^ (std::hash<uint32_t>{}(key.first.generation) << 1);
            auto h2 = std::hash<uint32_t>{}(key.second.index) ^ (std::hash<uint32_t>{}(key.second.generation) << 1);
            return h1 ^ (h2 << 1);
        }
    };
    LitBatch* find_lit_batch(RHITextureHandle albedo, RHITextureHandle normal);

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
    std::unordered_map<LitBatchKey, LitBatch, LitBatchKeyHash> lit_batches_; // 受光照精灵批次（按纹理分组）
    std::vector<ShadowCasterVertex> shadow_caster_vertices_; // 阴影遮挡物顶点
    math::Matrix4f ortho_;
    math::Matrix4f view_proj_;
    math::Vector2f camera_center_ = math::Vector2f::zero();
    float camera_zoom_ = 1.0f;
    float camera_rotation_ = 0.0f;
    bool camera_top_left_origin_ = false;
    float screen_width_ = 0.0f;
    float screen_height_ = 0.0f;

    // 覆盖层 pass 的相机快照（begin_overlay_pass 存、end_overlay_pass 还）。
    // 覆盖层把相机切成"左上角原点 1:1"，结束后必须原样还回去——同一帧里
    // 之后还可能再有别的 2D 绘制（GameView）用到场景相机。
    struct OverlaySavedCamera {
        math::Vector2f center    = math::Vector2f::zero();
        float          zoom      = 1.0f;
        float          rotation  = 0.0f;
        float          width     = 0.0f;
        float          height    = 0.0f;
        bool           top_left  = false;
        bool           active    = false;
    };
    OverlaySavedCamera overlay_saved_;
    bool initialized_ = false;
    bool using_fallback_font_ = false;
    // 无可用字体时只告警一次（避免每帧刷日志）；回退图集的实心色块不再绘制。
    bool warned_no_font_ = false;
    BlendMode blend_mode_ = BlendMode::Alpha;

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

    // 受光照 pass 的默认 albedo/normal 纹理（GL object id）。
    // 仅渲染线程访问：在 lit 前向命令 lambda 内惰性创建，
    // shutdown 时通过命令在同一线程删除（避免函数级 static 的进程级泄漏）。
    uint32_t default_albedo_tex_ = 0;
    uint32_t default_normal_tex_ = 0;
};

} // namespace gryce_engine::render
