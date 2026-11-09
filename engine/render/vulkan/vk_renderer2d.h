#pragma once

#include <memory>
#include <unordered_map>
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
    void flush_pending() override { flush_batches(); }
    void begin_overlay_pass(int screen_width, int screen_height) override;
    void end_overlay_pass() override;
    // 2D 裁剪。坐标是**逻辑像素**（draw list 的坐标系），实现里按 overlay_scale
    // 折成目标像素：覆盖层走 2x 超采样时，不缩放就等于把整幅画面裁到左上四分之一。
    void set_scissor(int x, int y, int w, int h) override;
    void reset_scissor() override;
    void set_blend_mode(BlendMode mode) override;

    void set_camera(const math::Vector2f& center, float zoom, bool top_left_origin = false,
                    float rotation = 0.0f) override;
    math::Vector2f camera_center() const override { return camera_center_; }
    float camera_zoom() const override { return camera_zoom_; }
    float camera_rotation() const override { return camera_rotation_; }
    math::Vector2f world_to_screen(const math::Vector2f& world) const override;
    math::Vector2f screen_size() const override { return math::Vector2f(screen_width_, screen_height_); }
    float overlay_scale() const override;

    void draw_rect(float x, float y, float w, float h, const Color& color) override;
    void draw_polygon(const std::vector<math::Vector2f>& points, const Color& color) override;
    void draw_circle(float cx, float cy, float r, int segments, const Color& color) override;
    // 圆角图元：几何与 GL/D3D12 共用 render/round_rect_geometry.h，只是把顶点
    // 压进本后端自己的批次（后者已按 overlay_scale 放大，所以半径不用另乘）。
    void draw_round_rect(float x, float y, float w, float h,
                         const float radii[4], const Color& color) override;
    void draw_round_rect_gradient(float x, float y, float w, float h,
                                  const float radii[4],
                                  const Color& top, const Color& bottom) override;
    void draw_round_rect_border(float x, float y, float w, float h,
                                const float radii[4], float thickness,
                                const Color& color) override;
    void draw_round_shadow(float x, float y, float w, float h,
                           const float radii[4], float spread,
                           const Color& color) override;
    void draw_text(float x, float y, const std::string& text, float font_size, const Color& color) override;
    // 多色文本（语法高亮）：与 draw_text 同一套字形推进，逐 run 换色但共用笔位。
    void draw_text_colored(float x, float y, float font_size,
                           const std::vector<ColoredRun>& runs) override;
    void draw_sprite(float x, float y, float w, float h,
                      RHITextureHandle texture, const Color& tint = Color::white()) override;
    void draw_sprite_region(float x, float y, float w, float h,
                             float u0, float v0, float u1, float v1,
                             RHITextureHandle texture, const Color& tint = Color::white()) override;
    void draw_sprite_rotated(float x, float y, float w, float h, float rotation,
                              RHITextureHandle texture, const Color& tint = Color::white()) override;

    // 2D 光照接口
    void set_ambient_light(const Color& color) override;
    void add_light(const Light2D& light) override;
    void add_point_light(const math::Vector2f& pos, float radius,
                          const Color& color, float intensity) override;
    void reset_lights() override;
    void draw_lit_sprite(float x, float y, float w, float h,
                          RHITextureHandle albedo, RHITextureHandle normal_map,
                          const Color& tint = Color::white()) override;
    void draw_lit_sprite_region(float x, float y, float w, float h,
                                 float u0, float v0, float u1, float v1,
                                 RHITextureHandle albedo, RHITextureHandle normal_map,
                                 const Color& tint = Color::white(),
                                 float nu0 = 0.0f, float nv0 = 0.0f,
                                 float nu1 = 1.0f, float nv1 = 1.0f) override;

    // 阴影遮挡物与 Bloom
    void draw_shadow_caster(float x, float y, float w, float h) override;
    void set_bloom(const BloomParams& params) override;

    RHITextureHandle create_texture_from_data(const assets::TextureData* data) override;
    ITexture* resolve_texture(RHITextureHandle handle) const override;

    // 文本度量与图集档位：必须与 draw_text 的取档方式一致，否则 measure 出来的
    // 宽度和实际笔位对不上（彩色 run 会互相对不齐）。
    float measure_text(const char* text, float font_size) const override;
    float text_cap_height(float font_size) const override;
    // 多尺寸阶梯：IRenderer2D 的默认实现是空操作，Vulkan 侧不实现的话图集永远
    // 只有初始那 32px 一档，UI 的 12~16px 文字全从 32px 缩下来。
    bool  ensure_text_raster_size(float px) override;
    bool  ensure_text_raster_sizes(const std::vector<float>& px) override;
    float text_raster_size() const override;

    // 在屏幕 (x,y) 处绘制字体图集预览（核对图集内容与 UV 用）
    void draw_font_atlas_debug(float x, float y, float size);

private:
    bool context_alive() const;

    void push_vertex(float x, float y, const Color& color, float u, float v);
    void push_text_vertex(float x, float y, const Color& color, float u, float v);
    void push_sprite_vertex(float x, float y, const Color& color, float u, float v);
    void push_lit_vertex(RHITextureHandle albedo, RHITextureHandle normal,
                         float x, float y, const Color& color,
                         float u, float v, float nu, float nv);
    void push_shadow_caster_vertex(float x, float y);

    // 受光照精灵批次：按 (albedo, normal) 纹理句柄分组。
    // 批内只存句柄（按值拷贝安全），命令在渲染线程执行时经 generation
    // 校验解析，纹理已销毁则回退默认纹理，杜绝悬垂 ITexture*。
    struct LitBatch {
        RHITextureHandle albedo;
        RHITextureHandle normal;
        std::vector<LitVertex2D> verts;
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

    void flush_batches();
    void flush_sprite_batch();
    // 把逻辑像素的裁剪矩形按 overlay_scale 折成目标像素后下发。
    void apply_scissor_rect(int x, int y, int w, int h);
    void render_lit_sprites_forward(bool offscreen);
    // first_vertex/draw_count 用于同一条顶点流的分段绘制：多次提交共用一次上传，
    // 避免后一次上传覆盖前一次的数据（每个帧槽只有一个顶点缓冲）。
    void flush_batch(std::vector<Vertex2D>&& verts, bool is_text, ITexture* texture,
                     VkPipeline pipeline, VkPipelineLayout layout,
                     std::uint32_t first_vertex = 0, std::uint32_t draw_count = 0,
                     std::uint32_t upload_first_vertex = 0);

    void render_shadow_pass();
    void render_bloom_pass();

    bool create_shader_modules();
    bool create_descriptor_layouts();
    bool create_pipeline_layouts();
    bool create_swapchain_pipelines();
    bool create_descriptor_sets();
    VkDescriptorSet allocate_descriptor_set(VkDescriptorSetLayout layout);
    // 顶点缓冲扩容：旧缓冲移入 retired_buffers_[frame_index] 延迟销毁，
    // 避免销毁仍被本帧已录制命令引用的缓冲（GPU 读已释放资源会 device lost）。
    bool grow_vertex_buffer(VulkanBuffer& buffer, VkDeviceSize& capacity,
                            VkDeviceSize required, int frame_index, const char* what);
    bool create_vertex_buffers();
    bool create_uniform_buffers();
    bool create_fallback_textures();
    bool create_shadow_map();
    bool create_bloom_targets();
    void destroy_shadow_map();
    void destroy_bloom_targets();
    void destroy_bloom_pipelines();

    // 覆盖层超采样（SSAA）：UI 先画到 2x 离屏目标，结束时双线性降采样回
    // back buffer。双线性在精确 2:1 上等价于 2x2 盒式平均，等于每个输出像素
    // 拿到 4 个样本——文字与所有 2D 图形边缘一并变平滑。
    bool ensure_ui_ssaa_target(int width, int height);
    bool create_ui_ssaa_target_locked(int width, int height);
    bool create_ui_ssaa_pipeline();
    void destroy_ui_ssaa_target();

    VkPipeline create_pipeline(VkShaderModule vert_module, VkShaderModule frag_module,
                               VkPipelineLayout layout, VkRenderPass render_pass,
                               uint32_t vertex_stride,
                               const VkVertexInputAttributeDescription* attrs,
                               uint32_t attr_count,
                               bool depth_test, bool depth_write, BlendMode blend_mode,
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
    VkPipeline pipeline_sprite_additive_ = VK_NULL_HANDLE;
    VkPipeline pipeline_lit_sprite_ = VK_NULL_HANDLE;

    // pipelines (shadow / offscreen / bloom)
    VkPipeline pipeline_shadow_ = VK_NULL_HANDLE;
    VkPipeline pipeline_rect_scene_ = VK_NULL_HANDLE;
    VkPipeline pipeline_text_scene_ = VK_NULL_HANDLE;
    VkPipeline pipeline_sprite_scene_ = VK_NULL_HANDLE;
    VkPipeline pipeline_sprite_scene_additive_ = VK_NULL_HANDLE;
    VkPipeline pipeline_lit_sprite_scene_ = VK_NULL_HANDLE;
    VkPipeline pipeline_bloom_threshold_ = VK_NULL_HANDLE;
    VkPipeline pipeline_bloom_blur_ = VK_NULL_HANDLE;
    VkPipeline pipeline_bloom_compose_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ui_resolve_ = VK_NULL_HANDLE;
    VkShaderModule frag_ui_resolve_module_ = VK_NULL_HANDLE;

    // 覆盖层超采样目标（2x）、对应帧缓冲与解析管线输入
    RHITextureHandle ui_ssaa_texture_;
    RHIFramebufferHandle ui_ssaa_fb_;
    int ui_ssaa_w_ = 0;
    int ui_ssaa_h_ = 0;
    bool ui_ssaa_ready_ = false;
    float overlay_scale_ = 1.0f;
    // 当前裁剪矩形（已是目标像素）。scissor 在 Vulkan 里是动态状态，而一个批次
    // 从累积到提交之间可能跨过多次状态变化，所以每次提交都要按"这批顶点积累时"
    // 的矩形重新下发一次——否则整批会被最后一个分支的矩形带跑。
    bool scissor_set_ = false;
    std::array<std::int32_t, 4> scissor_rect_{{0, 0, 0, 0}};

    std::vector<VkDescriptorPool> descriptor_pools_;
    // 每个池"本帧还没重置"的标记。重置动作放进 allocate_descriptor_set：主线程
    // 推的重置命令执行时拿到的帧槽可能还是上一帧的，重置不到真正要用的池，
    // 未重置的池会一路累积到最后分配失败。
    std::vector<std::uint8_t> descriptor_pool_dirty_;

    std::vector<VulkanBuffer> vertex_buffer_rect_;
    std::vector<VulkanBuffer> vertex_buffer_text_;
    std::vector<VulkanBuffer> vertex_buffer_sprite_;
    std::vector<VulkanBuffer> vertex_buffer_lit_sprite_;
    std::vector<VulkanBuffer> vertex_buffer_shadow_caster_;
    std::vector<VulkanBuffer> fs_vertex_buffer_;
    std::vector<VulkanBuffer> light_ubo_;
    // 顶点缓冲扩容时被替换下来的旧缓冲（按帧槽存放）。旧缓冲可能仍被
    // 本帧已录制的命令引用，立即销毁会让 GPU 读已释放资源导致 device
    // lost；延迟到该帧槽下一次 begin_frame（fence 等待之后）再销毁。
    std::vector<std::vector<VulkanBuffer>> retired_buffers_;
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
    // 当前精灵批次纹理（句柄按值持有，flush 时经 generation 校验解析）
    RHITextureHandle sprite_texture_;
    // 已封口的贴图批次。贴图按纹理分批，纹理一变就得换绑定，不能在绘制期直接
    // 提交——那样会插到矩形/文字批次之前，被后提交的底色盖住。这里记下该段开始
    // 时的矩形顶点数，提交时按这个切分把矩形还原回原来的绘制顺序。
    struct SpriteSegment {
        RHITextureHandle texture;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::vector<Vertex2D> vertices;
        std::size_t rect_vertex_split = 0;
    };
    std::vector<SpriteSegment> sprite_segments_;
    // 一帧可能提交多次（分层绘制），而每个帧槽只有一条矩形/贴图顶点缓冲，
    // 所以流在缓冲里的起点要跨批次累加，否则后一批会覆盖前一批的数据。
    std::uint32_t rect_stream_base_ = 0;
    std::uint32_t sprite_stream_base_ = 0;
    std::uint32_t text_stream_base_ = 0;
    void finalize_sprite_segment();
    VkPipeline current_sprite_pipeline() const;
    std::unordered_map<LitBatchKey, LitBatch, LitBatchKeyHash> lit_batches_;
    std::vector<ShadowCasterVertex> shadow_caster_vertices_;

    math::Matrix4f ortho_;
    math::Matrix4f view_proj_;
    math::Vector2f camera_center_ = math::Vector2f::zero();
    float camera_zoom_ = 1.0f;
    float camera_rotation_ = 0.0f;
    bool camera_top_left_origin_ = false;
    float screen_width_ = 0.0f;
    float screen_height_ = 0.0f;
    bool overlay_saved_active_ = false;
    bool overlay_pass_ = false;
    math::Vector2f overlay_saved_center_;
    float overlay_saved_zoom_ = 1.0f;
    float overlay_saved_rotation_ = 0.0f;
    bool overlay_saved_top_left_ = false;
    float overlay_saved_width_ = 0.0f;
    float overlay_saved_height_ = 0.0f;
    bool initialized_ = false;
    bool use_bloom_this_frame_ = false;
    BlendMode blend_mode_ = BlendMode::Alpha;

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
