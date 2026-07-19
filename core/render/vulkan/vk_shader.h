#pragma once

#include "render/shader.h"
#include "render/skinned_vertex.h"
#include "render/vulkan/vk_buffer.h"

#include <vulkan/vulkan.h>
#include <array>
#include <memory>
#include <vector>
#include <string>

namespace gryce_engine::render {

class VulkanDevice;
class VulkanSwapchain;
class VulkanTexture;

// ---------------------------------------------------------------------------
// VulkanShader — SPIR-V + pipeline layout + descriptor set layout
// ---------------------------------------------------------------------------
class VulkanShader : public IShader {
public:
    VulkanShader() = default;
    VulkanShader(VulkanDevice* device, VulkanSwapchain* swapchain);
    ~VulkanShader() override;

    bool compile(const std::string& vertex_src, const std::string& fragment_src) override;
    bool compile(const std::vector<ShaderStageDesc>& stages) override;

    void bind() const override;
    void unbind() const override;

    void set_int(const std::string& name, int value) override;
    void set_int(const char* name, int value) override;
    void set_float(const std::string& name, float value) override;
    void set_float(const char* name, float value) override;
    void set_vec2(const std::string& name, const math::Vector2f& value) override;
    void set_vec2(const char* name, const math::Vector2f& value) override;
    void set_vec3(const std::string& name, const math::Vector3f& value) override;
    void set_vec3(const char* name, const math::Vector3f& value) override;
    void set_vec4(const std::string& name, const math::Vector4f& value) override;
    void set_vec4(const char* name, const math::Vector4f& value) override;
    void set_mat4(const std::string& name, const math::Matrix4f& value) override;
    void set_mat4(const char* name, const math::Matrix4f& value) override;
    void set_mat4_array(const char* name, const math::Matrix4f* data, uint32_t count) override;
    void set_texture(int slot, ITexture* texture) override;

    bool is_valid() const override;

    VkPipelineLayout layout() const { return pipeline_layout_; }
    VkPipeline pipeline() const { return pipeline_; }
    VkDescriptorSetLayout descriptor_set_layout() const { return descriptor_set_layout_; }
    VkDescriptorSet descriptor_set() const;
    int current_frame() const;

    bool is_post_process() const { return post_process_; }
    bool is_skybox() const { return skybox_; }
    // post-process 与 skybox 共用每帧固定描述符集（单 sampler），
    // 标准 PBR 路径则每 draw 分配独立描述符集。
    bool uses_fixed_descriptor_sets() const { return post_process_ || skybox_; }

    bool load_program(const std::string& name,
                      const std::string& shader_dir,
                      IFramebuffer* target = nullptr,
                      bool color_output = true,
                      bool post_process = false,
                      bool skybox = false,
                      bool skinned = false) override;

    void set_post_process_params(float exposure, int mode) override {
        pp_exposure_ = exposure;
        pp_mode_ = mode;
    }

    void update_ubo(VkCommandBuffer cmd) const;
    void push_constants(VkCommandBuffer cmd) const;
    void push_post_process_constants(VkCommandBuffer cmd, float exposure, int mode) const;
    void bind_descriptor_set(VkCommandBuffer cmd) const;

    // 每帧开始（渲染线程、acquire 完成之后调用）：重置该帧的描述符池与
    // draw 游标。pool 中上一周期分配的描述符集所属的命令缓冲已被
    // frame fence 保证执行完毕，因此整池 reset 是安全的。
    void on_begin_frame(int frame_index);

    // 每次 draw 调用：把当前 UBO 数据写入该帧大 UBO 的独立偏移，
    // 从该帧描述符池分配一套全新描述符集（UBO + 当前贴图），写入并绑定。
    // 这样同一帧内不同材质的 draw 互不覆盖——共享一套描述符会导致
    // GPU 执行时所有 draw 读到最后一个写入的材质。
    void prepare_draw(VkCommandBuffer cmd);

    // 纹理销毁时由 backend 调用：清除 current_textures_ / cached_textures_ 中
    // 对该指针的缓存。池槽位复用后同一地址可能属于新纹理，裸指针相等会误判
    // "已绑定"而跳过 descriptor 更新；per-draw 路径则会直接用悬垂指针取 image_view。
    void invalidate_texture_cache(const VulkanTexture* tex);

private:
    bool load_spirv_from_file(const std::string& path, std::vector<uint32_t>& out);
    bool load_spirv_files(const std::string& vert_path, const std::string& frag_path);
    void set_render_pass(VkRenderPass render_pass) { render_pass_ = render_pass; }
    void set_color_output_enabled(bool enabled) { color_output_enabled_ = enabled; }
    void set_post_process(bool pp) { post_process_ = pp; }
    void set_skybox(bool skybox) { skybox_ = skybox; }
    bool create_pipeline();
    VkShaderModule create_shader_module(const std::vector<uint32_t>& code);
    bool create_descriptor_pool();
    bool create_ubo();
    // 解析 "uLightPos[3]" 形式的光源数组下标
    static bool parse_light_index(const std::string& name, const char* field, int& index);

    VulkanDevice* device_ = nullptr;
    VulkanSwapchain* swapchain_ = nullptr;

    VkShaderModule vert_module_ = VK_NULL_HANDLE;
    VkShaderModule frag_module_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    bool color_output_enabled_ = true;
    bool post_process_ = false;
    bool skybox_ = false;
    // 骨骼蒙皮管线：顶点布局追加 bone ids/weights（stride 88），
    // 描述符布局追加 palette UBO（binding 8，vertex stage）
    bool skinned_ = false;

    // 与 GLSL std140 对齐的单光源结构（64 字节，与 GLSL Light 对应）
    struct LightUBO {
        math::Vector4f pos_type;         // xyz=position, w=type (0 方向光/1 点光/2 聚光)
        math::Vector4f dir_range;        // xyz=direction, w=range
        math::Vector4f color_intensity;  // xyz=color, w=intensity
        math::Vector4f spot;             // x=cos(outer), y=cos(inner)
    };
    static constexpr int k_max_lights = 8;

    // 与 GLSL std140 对齐的 UBO（material + ambient + lights）
    // GLSL 中 vec3 一律以 vec4 存储，避免尾部填充不一致
    struct alignas(16) UBOData {
        math::Vector4f albedo_color;
        math::Vector4f camera_pos;
        math::Vector4f emissive_opacity; // xyz=emissive, w=opacity
        math::Vector4f ambient;          // xyz=环境光颜色
        math::Vector4f uv_transform;     // xy=uv scale, zw=uv offset
        float roughness;
        float metallic;
        float ao;
        float shadow_bias;
        int use_shadow_map;
        int use_albedo_map;
        int use_normal_map;
        int use_roughness_map;
        int use_metallic_map;
        int use_ao_map;
        int use_emissive_map;
        int hdr_enabled;
        int light_count;
        int shadow_light_index;
        int pad0;
        int pad1;
        LightUBO lights[k_max_lights];
    };

    // 非 post-process 路径：每 draw 独立描述符 + UBO 偏移。
    // 每帧一个描述符池（on_begin_frame 整池 reset）和一个大 UBO
    // （HOST_VISIBLE|COHERENT），按 draw 游标以 ubo_stride_ 对齐切分。
    // stride 按 256 对齐（>= minUniformBufferOffsetAlignment 常见最大值）。
    static constexpr size_t ubo_stride_ = (sizeof(UBOData) + 255) / 256 * 256;
    static constexpr uint32_t max_draws_per_frame_ = 2048;
    static constexpr int k_max_texture_bindings = 8;

    std::vector<std::unique_ptr<VulkanBuffer>> ubo_buffers_;      // 每帧一个大 UBO
    std::vector<VkDescriptorPool> descriptor_pools_;              // 每帧一个池
    std::vector<VkDescriptorSet> descriptor_sets_;                // post-process：每帧固定集
    std::vector<uint32_t> draw_counts_;                           // 每帧 draw 游标

    // 骨骼 palette（仅 skinned_ 管线）：每帧一个大 UBO，按 draw 游标切分。
    // 与主 UBO 共用同一 cursor，保证一次 draw 的 material 与 palette 对齐。
    static constexpr size_t palette_stride_ = k_max_skinning_bones * sizeof(math::Matrix4f); // 8192
    static constexpr uint32_t max_skinned_draws_per_frame_ = 256;
    std::vector<std::unique_ptr<VulkanBuffer>> palette_buffers_;  // 每帧一个 palette UBO
    // set_mat4_array("uBonePalette") 写入的当前 palette 缓存（渲染线程本地）
    mutable std::array<math::Matrix4f, k_max_skinning_bones> palette_{};
    mutable uint32_t palette_count_ = 0;

    // 当前各 binding 绑定的贴图（set_texture 记录，prepare_draw 写入新集）
    std::array<VulkanTexture*, k_max_texture_bindings> current_textures_{};

    // 1x1 白色回退贴图：prepare_draw 对每个贴图 binding 都必须写入一个
    // 合法 image view + sampler。新分配的描述符集内容是未定义的，若某个
    // binding 留空而 shader（条件分支被编译器提升后）仍采样它，GPU 会读到
    // 垃圾描述符并可能直接挂死（fence 永不 signal，表现为整个窗口卡死）。
    std::unique_ptr<VulkanTexture> fallback_texture_;

    // post-process 仍使用每帧固定描述符集（每帧只绑一张贴图，无串扰问题），
    // 沿用按 frame/binding 的更新缓存。
    mutable std::vector<std::array<VulkanTexture*, k_max_texture_bindings>> cached_textures_;

    mutable UBOData ubo_data_{};
    mutable math::Matrix4f model_;
    mutable math::Matrix4f view_;
    mutable math::Matrix4f projection_;
    mutable math::Matrix4f light_space_matrix_;
    mutable bool ubo_dirty_ = true;

    // Post-process parameters
    float pp_exposure_ = 1.0f;
    int pp_mode_ = 1;
};

} // namespace gryce_engine::render
