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
// 使用独立 pipeline / descriptor set / vertex buffer，支持矩形与文字
// ---------------------------------------------------------------------------
class VulkanRenderer2D : public IRenderer2D {
public:
    VulkanRenderer2D();
    ~VulkanRenderer2D() override;

    void init(RenderContext* ctx) override;
    void shutdown() override;

    void begin_frame(float screen_width, float screen_height) override;
    void end_frame() override;

    void draw_rect(float x, float y, float w, float h, const Color& color) override;
    void draw_polygon(const std::vector<math::Vector2f>& points, const Color& color) override;
    void draw_circle(float cx, float cy, float r, int segments, const Color& color) override;
    void draw_text(float x, float y, const std::string& text, float font_size, const Color& color) override;
    void draw_sprite(float x, float y, float w, float h,
                      ITexture* texture, const Color& tint = Color::white()) override;
    void draw_sprite_region(float x, float y, float w, float h,
                             float u0, float v0, float u1, float v1,
                             ITexture* texture, const Color& tint = Color::white()) override;

    // 2D 光照接口（Forward Lighting 简化版：ambient + 1 个点光源）
    void set_ambient_light(const Color& color) override;
    void add_point_light(const math::Vector2f& pos, float radius,
                          const Color& color, float intensity) override;
    void reset_lights() override;
    void draw_lit_sprite(float x, float y, float w, float h,
                          ITexture* albedo, ITexture* normal_map,
                          const Color& tint = Color::white()) override;
    void draw_lit_sprite_region(float x, float y, float w, float h,
                                 float u0, float v0, float u1, float v1,
                                 ITexture* albedo, ITexture* normal_map,
                                 const Color& tint = Color::white()) override;

    ITexture* resolve_texture(RHITextureHandle handle) const override;

    // 调试用：在屏幕 (x,y) 处绘制字体图集预览
    void draw_font_atlas_debug(float x, float y, float size);

private:
    bool context_alive() const;

    void push_vertex(float x, float y, const Color& color, float u, float v);
    void push_text_vertex(float x, float y, const Color& color, float u, float v);
    void push_sprite_vertex(float x, float y, const Color& color, float u, float v);
    void flush_batches();

    bool create_shader_modules();
    bool create_descriptor_layout();
    bool create_pipeline_layout();
    VkPipeline create_pipeline(VkShaderModule vert_module, VkShaderModule frag_module, VkPipelineLayout layout);
    bool create_descriptor_sets();
    bool create_vertex_buffer();

    void flush_sprite_batch();
    void flush_lit_sprite_batch();
    void flush_batch(std::vector<Vertex2D>&& verts, bool is_text, ITexture* texture,
                     VkPipeline pipeline, VkPipelineLayout layout, bool is_lit);

    RenderContext* ctx_ = nullptr;
    std::shared_ptr<RenderContext::Lifetime> ctx_lifetime_;
    VulkanBackend* vk_backend_ = nullptr;
    VulkanDevice* vk_device_ = nullptr;
    VulkanSwapchain* vk_swapchain_ = nullptr;

    VkShaderModule vert_module_ = VK_NULL_HANDLE;
    VkShaderModule vert_lit_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_rect_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_text_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_sprite_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_lit_sprite_module_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout lit_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_rect_ = VK_NULL_HANDLE;
    VkPipeline pipeline_text_ = VK_NULL_HANDLE;
    VkPipeline pipeline_sprite_ = VK_NULL_HANDLE;
    VkPipeline pipeline_lit_sprite_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> text_descriptor_sets_;
    std::vector<VkDescriptorSet> sprite_descriptor_sets_;

    std::vector<VulkanBuffer> vertex_buffer_rect_;
    std::vector<VulkanBuffer> vertex_buffer_text_;
    std::vector<VulkanBuffer> vertex_buffer_sprite_;
    std::vector<VulkanBuffer> vertex_buffer_lit_sprite_;
    std::vector<VkDeviceSize> vertex_buffer_rect_capacity_;
    std::vector<VkDeviceSize> vertex_buffer_text_capacity_;
    std::vector<VkDeviceSize> vertex_buffer_sprite_capacity_;
    std::vector<VkDeviceSize> vertex_buffer_lit_sprite_capacity_;

    FontAtlas font_atlas_;
    bool using_fallback_font_ = false;

    std::vector<Vertex2D> vertices_;
    std::vector<Vertex2D> text_vertices_;
    std::vector<Vertex2D> sprite_vertices_;
    ITexture* sprite_texture_ = nullptr;
    math::Matrix4f ortho_;

    // 2D 光照状态（Forward Lighting）
    Color ambient_light_ = Color::black();
    struct PointLight {
        math::Vector2f pos;
        float radius = 0.0f;
        Color color;
        float intensity = 0.0f;
    };
    PointLight point_light_;
    bool has_point_light_ = false;
    std::vector<Vertex2D> lit_sprite_vertices_;
    ITexture* lit_sprite_texture_ = nullptr;

    float screen_width_ = 0.0f;
    float screen_height_ = 0.0f;
    bool initialized_ = false;
};

} // namespace gryce_engine::render
