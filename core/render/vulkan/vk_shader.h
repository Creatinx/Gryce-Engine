#pragma once

#include "render/shader.h"
#include "render/vulkan/vk_buffer.h"

#include <vulkan/vulkan.h>
#include <memory>
#include <vector>
#include <string>

namespace gryce_engine::render {

class VulkanDevice;
class VulkanSwapchain;

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
    void set_texture(int slot, ITexture* texture) override;

    bool is_valid() const override;

    VkPipelineLayout layout() const { return pipeline_layout_; }
    VkPipeline pipeline() const { return pipeline_; }
    VkDescriptorSetLayout descriptor_set_layout() const { return descriptor_set_layout_; }
    VkDescriptorSet descriptor_set() const;
    int current_frame() const;

    bool is_post_process() const { return post_process_; }

    bool load_program(const std::string& name,
                      const std::string& shader_dir,
                      IFramebuffer* target = nullptr,
                      bool color_output = true,
                      bool post_process = false) override;

    void set_post_process_params(float exposure, int mode) override {
        pp_exposure_ = exposure;
        pp_mode_ = mode;
        push_dirty_ = true;
    }

    void update_ubo(VkCommandBuffer cmd) const;
    void push_constants(VkCommandBuffer cmd) const;
    void push_post_process_constants(VkCommandBuffer cmd, float exposure, int mode) const;
    void bind_descriptor_set(VkCommandBuffer cmd) const;

private:
    bool load_spirv_from_file(const std::string& path, std::vector<uint32_t>& out);
    bool load_spirv_files(const std::string& vert_path, const std::string& frag_path);
    void set_render_pass(VkRenderPass render_pass) { render_pass_ = render_pass; }
    void set_color_output_enabled(bool enabled) { color_output_enabled_ = enabled; }
    void set_post_process(bool pp) { post_process_ = pp; }
    bool create_pipeline();
    VkShaderModule create_shader_module(const std::vector<uint32_t>& code);
    bool create_descriptor_pool();
    bool create_ubo();

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

    // 与 GLSL std140 对齐的 UBO（material + light）
    // GLSL 中 uAlbedoColor/uCameraPos/uLightDir/uLightColor 使用 vec4，避免 vec3 尾部填充不一致
    struct alignas(16) UBOData {
        math::Vector4f albedo_color;
        math::Vector4f camera_pos;
        math::Vector4f light_dir;
        math::Vector4f light_color;
        float roughness;
        float metallic;
        float ao;
        float shadow_bias;
        float light_intensity;
        int use_shadow_map;
        int use_albedo_map;
        int use_normal_map;
        int use_roughness_map;
        int use_metallic_map;
        int use_ao_map;
    };
    static_assert(sizeof(UBOData) <= 256, "UBO size exceeds 256 bytes");

    std::vector<std::unique_ptr<VulkanBuffer>> ubo_buffers_;
    std::vector<VkDescriptorSet> descriptor_sets_;
    static constexpr size_t ubo_size_ = 256;

    mutable UBOData ubo_data_{};
    mutable math::Matrix4f model_;
    mutable math::Matrix4f view_;
    mutable math::Matrix4f projection_;
    mutable math::Matrix4f light_space_matrix_;
    mutable bool ubo_dirty_ = true;
    mutable bool push_dirty_ = true;

    // Post-process parameters
    float pp_exposure_ = 1.0f;
    int pp_mode_ = 1;
};

} // namespace gryce_engine::render
