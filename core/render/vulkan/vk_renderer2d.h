#pragma once

#include <memory>
#include <vector>

#include "render/render2d.h"
#include "render/renderer2d_impl.h"
#include "render/font_atlas.h"
#include "render/vulkan/vk_buffer.h"

namespace gryce_engine::render {

class RenderContext;
class VulkanBackend;
class VulkanDevice;
class VulkanSwapchain;
class VulkanTexture;

// ---------------------------------------------------------------------------
// VulkanRenderer2D — Vulkan 2D 渲染器实现
// 支持矩形、文字、精灵、2D 光照、硬阴影与 Bloom 后处理
// ---------------------------------------------------------------------------
class VulkanRenderer2D : public IRenderer2D {
public:
    VulkanRenderer2D();
    ~VulkanRenderer2D() override;

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
    void draw_sprite(float x, float y, float w, float h,
                      ITexture* texture, const Color& tint = Color::white()) override;
    void draw_sprite_region(float x, float y, float w, float h,
                             float u0, float v0, float u1, float v1,
                             ITexture* texture, const Color& tint = Color::white()) override;

    // 2D 光照接口
    void set_ambient_light(const Color& color) override;
    void add_light(const Light2D& light) override;
    void add_point_light(const math::Vector2f& pos, float radius,
                          const Color& color, float intensity) override;
    void reset_lights() override;
    void draw_lit_sprite(float x, float y, float w, float h,
                          ITexture* albedo, ITexture* normal_map,
                          const Color& tint = Color::white()) override;
    void draw_lit_sprite_region(float x, float y, float w, float h,
                                 float u0, float v0, float u1, float v1,
                                 ITexture* albedo, ITexture* normal_map,
                                 const Color& tint = Color::white(),
                                 float nu0 = 0.0f, float nv0 = 0.0f,
                                 float nu1 = 1.0f, float nv1 = 1.0f) override;

    // 阴影遮挡物与 Bloom
    void draw_shadow_caster(float x, float y, float w, float h) override;
    void set_bloom(const BloomParams& params) override;

    RHITextureHandle create_texture_from_data(const assets::TextureData* data) override;
    ITexture* resolve_texture(RHITextureHandle handle) const override;

    // 调试用：在屏幕 (x,y) 处绘制字体图集预览
    void draw_font_atlas_debug(float x, float y, float size);

private:
    bool context_alive() const;

    void push_vertex(float x, float y, const Color& color, float u, float v);
    void push_text_vertex(float x, float y, const Color& color, float u, float v);
    void push_sprite_vertex(float x, float y, const Color& color, float u, float v);
    void push_lit_vertex(ITexture* albedo, ITexture* normal,
                         float x, float y, const Color& color,
                         float u, float v, float nu, float nv);
    void push_shadow_caster_vertex(float x, float y);

    // 受光照精灵批次：按 (albedo, normal) 纹理分组
    struct LitBatch {
        ITexture* albedo = nullptr;
        ITexture* normal = nullptr;
        std::vector<LitVertex2D> verts;
    };
    std::vector<LitBatch>::iterator find_lit_batch(ITexture* albedo, ITexture* normal);

    void flush_batches();
    void flush_sprite_batch();
    void render_lit_sprites_forward(bool offscreen);
    void flush_batch(std::vector<Vertex2D>&& verts, bool is_text, ITexture* texture,
                     VkPipeline pipeline, VkPipelineLayout layout);

    void render_shadow_pass();
    void render_bloom_pass();

    bool create_shader_modules();
    bool create_descriptor_layouts();
    bool create_pipeline_layouts();
    bool create_swapchain_pipelines();
    bool create_descriptor_sets();
    VkDescriptorSet allocate_descriptor_set(VkDescriptorSetLayout layout);
    bool create_vertex_buffers();
    bool create_uniform_buffers();
    bool create_fallback_textures();
    bool create_shadow_map();
    bool create_bloom_targets();
    void destroy_shadow_map();
    void destroy_bloom_targets();
    void destroy_bloom_pipelines();

    VkPipeline create_pipeline(VkShaderModule vert_module, VkShaderModule frag_module,
                               VkPipelineLayout layout, VkRenderPass render_pass,
                               uint32_t vertex_stride,
                               const VkVertexInputAttributeDescription* attrs,
                               uint32_t attr_count,
                               bool depth_test, bool depth_write, bool blend,
                               bool color_output = true);

    void write_lit_descriptor_set(VkDescriptorSet set, int frame_index,
                                  VulkanTexture* albedo, VulkanTexture* normal,
                                  bool use_shadow);
    void draw_fullscreen_pass(RHIFramebufferHandle fb,
                              VkPipeline pipeline, VkPipelineLayout layout,
                              bool use_lit_descriptor_set,
                              const std::vector<std::pair<RHITextureHandle, uint32_t>>& bindings,
                              const void* push_constants, size_t push_size);

    RenderContext* ctx_ = nullptr;
    std::shared_ptr<RenderContext::Lifetime> ctx_lifetime_;
    VulkanBackend* vk_backend_ = nullptr;
    VulkanDevice* vk_device_ = nullptr;
    VulkanSwapchain* vk_swapchain_ = nullptr;

    // shader modules
    VkShaderModule vert_module_ = VK_NULL_HANDLE;
    VkShaderModule vert_lit_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_rect_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_text_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_sprite_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_lit_sprite_module_ = VK_NULL_HANDLE;
    VkShaderModule vert_shadow_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_shadow_module_ = VK_NULL_HANDLE;
    VkShaderModule vert_bloom_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_bloom_threshold_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_bloom_blur_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_bloom_compose_module_ = VK_NULL_HANDLE;

    // descriptor / pipeline layouts
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout lit_descriptor_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout lit_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout shadow_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout bloom_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout bloom_compose_pipeline_layout_ = VK_NULL_HANDLE;

    // pipelines (swapchain render pass)
    VkPipeline pipeline_rect_ = VK_NULL_HANDLE;
    VkPipeline pipeline_text_ = VK_NULL_HANDLE;
    VkPipeline pipeline_sprite_ = VK_NULL_HANDLE;
    VkPipeline pipeline_lit_sprite_ = VK_NULL_HANDLE;

    // pipelines (shadow / offscreen / bloom)
    VkPipeline pipeline_shadow_ = VK_NULL_HANDLE;
    VkPipeline pipeline_lit_sprite_scene_ = VK_NULL_HANDLE;
    VkPipeline pipeline_rect_scene_ = VK_NULL_HANDLE;
    VkPipeline pipeline_text_scene_ = VK_NULL_HANDLE;
    VkPipeline pipeline_sprite_scene_ = VK_NULL_HANDLE;
    VkPipeline pipeline_bloom_threshold_ = VK_NULL_HANDLE;
    VkPipeline pipeline_bloom_blur_ = VK_NULL_HANDLE;
    VkPipeline pipeline_bloom_compose_ = VK_NULL_HANDLE;

    std::vector<VkDescriptorPool> descriptor_pools_;

    std::vector<VulkanBuffer> vertex_buffer_rect_;
    std::vector<VulkanBuffer> vertex_buffer_text_;
    std::vector<VulkanBuffer> vertex_buffer_sprite_;
    std::vector<VulkanBuffer> vertex_buffer_lit_sprite_;
    std::vector<VulkanBuffer> vertex_buffer_shadow_caster_;
    std::vector<VulkanBuffer> fs_vertex_buffer_;
    std::vector<VulkanBuffer> light_ubo_;
    std::vector<VkDeviceSize> vertex_buffer_rect_capacity_;
    std::vector<VkDeviceSize> vertex_buffer_text_capacity_;
    std::vector<VkDeviceSize> vertex_buffer_sprite_capacity_;
    std::vector<VkDeviceSize> vertex_buffer_lit_sprite_capacity_;
    std::vector<VkDeviceSize> vertex_buffer_shadow_caster_capacity_;
    std::vector<VkDeviceSize> fs_vertex_buffer_capacity_;

    FontAtlas font_atlas_;
    bool using_fallback_font_ = false;

    std::vector<Vertex2D> vertices_;
    std::vector<Vertex2D> text_vertices_;
    std::vector<Vertex2D> sprite_vertices_;
    ITexture* sprite_texture_ = nullptr;
    std::vector<LitBatch> lit_batches_;
    std::vector<ShadowCasterVertex> shadow_caster_vertices_;

    math::Matrix4f ortho_;
    math::Matrix4f view_proj_;
    math::Vector2f camera_center_ = math::Vector2f::zero();
    float camera_zoom_ = 1.0f;
    float screen_width_ = 0.0f;
    float screen_height_ = 0.0f;
    bool initialized_ = false;
    bool use_bloom_this_frame_ = false;

    // 2D 光照
    Color ambient_light_ = Color::black();
    std::vector<Light2D> lights_;
    static constexpr int k_max_lights = 32;
    static constexpr int k_shadow_map_size = 1024;

    // Bloom
    BloomParams bloom_params_;
    bool bloom_initialized_ = false;

    // GPU 资源句柄
    RHITextureHandle fallback_albedo_tex_;
    RHITextureHandle fallback_normal_tex_;
    RHITextureHandle shadow_map_tex_;
    RHITextureHandle scene_texture_;
    RHITextureHandle bloom_texture_a_;
    RHITextureHandle bloom_texture_b_;
    RHIFramebufferHandle shadow_fb_;
    RHIFramebufferHandle scene_fb_;
    RHIFramebufferHandle bloom_fb_a_;
    RHIFramebufferHandle bloom_fb_b_;

    VulkanTexture* fallback_albedo_ = nullptr;
    VulkanTexture* fallback_normal_ = nullptr;
    VulkanTexture* shadow_map_ = nullptr;
};

} // namespace gryce_engine::render
