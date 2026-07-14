#include "vk_renderer2d.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "assets/texture_data.h"
#include "render/render_context.h"
#include "render/vulkan/vk_backend.h"
#include "render/vulkan/vk_device.h"
#include "render/vulkan/vk_swapchain.h"
#include "render/vulkan/vk_texture.h"
#include "render/vulkan/vk_framebuffer.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

namespace {

static std::string get_system_font_dir() {
#ifdef _WIN32
    char win_dir[MAX_PATH] = {};
    UINT len = GetWindowsDirectoryA(win_dir, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string dir = win_dir;
        if (!dir.empty() && dir.back() != '\\') {
            dir += '\\';
        }
        return dir + "Fonts\\";
    }
    GLOG_WARN("get_system_font_dir: GetWindowsDirectoryA failed, falling back to hardcoded path");
#endif
    return "C:\\Windows\\Fonts\\";
}

bool load_spirv_file(const std::string& path, std::vector<uint32_t>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        GLOG_ERROR("VulkanRenderer2D: failed to open SPIR-V file '{}'", path);
        return false;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size % 4 != 0) {
        GLOG_ERROR("VulkanRenderer2D: SPIR-V file size not aligned to 4 bytes");
        return false;
    }
    out.resize(static_cast<size_t>(size) / 4);
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        GLOG_ERROR("VulkanRenderer2D: failed to read SPIR-V file");
        return false;
    }
    return true;
}

VkShaderModule create_shader_module(VkDevice dev, const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * 4;
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &info, nullptr, &module);
    return module;
}

// 受光照 sprite UBO 中单个光源数据，与 vulkan_2d_lit.frag 的 std140 布局一致。
// std140 中 vec3 必须从 16 字节边界开始，因此 dir 后需要 8 字节填充。
struct alignas(16) LightData {
    int type;                          // 0
    float pad0;                        // 4
    math::Vector2f pos;                // 8
    math::Vector2f dir;                // 16
    float pad_before_color[2];         // 24
    float color[3];                    // 32
    float pad1;                        // 44
    float intensity;                   // 48
    float radius;                      // 52
    float range;                       // 56
    float spot_angle;                  // 60
    float spot_softness;               // 64
};
static_assert(sizeof(LightData) == 80, "LightData size mismatch");

// 受光照 sprite UBO，与 vulkan_2d_lit.frag 的 std140 布局一致
struct alignas(16) LightUBO {
    math::Vector3f ambient;            // 0-11
    int light_count;                   // 12-15
    int use_shadow_map;                // 16-19
    int shadow_light_index;            // 20-23
    float pad0[2];                     // 24-31
    math::Matrix4f light_space_matrix; // 32-95
    LightData lights[32];              // 96-2655
};
static_assert(sizeof(LightUBO) == 2656, "LightUBO size mismatch");

// 全屏后处理顶点
struct FSVertex {
    float x, y, u, v;
};

} // namespace

VulkanRenderer2D::VulkanRenderer2D() = default;

VulkanRenderer2D::~VulkanRenderer2D() {
    shutdown();
}

bool VulkanRenderer2D::context_alive() const {
    return ctx_ && ctx_lifetime_ && ctx_lifetime_->alive.load();
}

void VulkanRenderer2D::init(RenderContext* ctx) {
    if (initialized_ || !ctx) return;
    ctx_ = ctx;
    ctx_lifetime_ = ctx->lifetime();

    vk_backend_ = dynamic_cast<VulkanBackend*>(ctx->backend());
    if (!vk_backend_) {
        GLOG_ERROR("VulkanRenderer2D: backend is not VulkanBackend");
        return;
    }
    vk_device_ = vk_backend_->device();
    vk_swapchain_ = vk_backend_->swapchain();

    if (!create_shader_modules() ||
        !create_descriptor_layouts() ||
        !create_pipeline_layouts() ||
        !create_swapchain_pipelines() ||
        !create_descriptor_sets() ||
        !create_vertex_buffers() ||
        !create_uniform_buffers() ||
        !create_fallback_textures() ||
        !create_shadow_map()) {
        shutdown();
        return;
    }

    // 优先使用项目内置 TTF（Roboto），保证跨机器一致性；失败再尝试系统字体
    {
        std::string bundled = resources::ResourcePath::resolve("res:/fonts/Roboto-Medium.ttf");
        if (bundled.empty() || !std::filesystem::exists(bundled)) {
            bundled = resources::Project::instance().root() + "/third_party/imgui/misc/fonts/Roboto-Medium.ttf";
        }
        if (std::filesystem::exists(bundled)) {
            if (font_atlas_.init(ctx_, bundled, 32.0f)) {
                using_fallback_font_ = false;
                GLOG_INFO("VulkanRenderer2D: loaded bundled font '{}'", bundled);
            }
        }
    }

    // 内置字体失败时尝试系统字体
    if (!font_atlas_.texture()) {
        std::string font_dir = get_system_font_dir();
        const char* font_names[] = {
            "arial.ttf",
            "segoeui.ttf",
            "tahoma.ttf",
            "calibri.ttf",
            "verdana.ttf",
            "times.ttf",
            "msyh.ttc",
            "simhei.ttf",
            nullptr
        };

        for (int i = 0; font_names[i]; ++i) {
            std::string font_path = font_dir + font_names[i];
            if (font_atlas_.init(ctx_, font_path, 32.0f)) {
                using_fallback_font_ = false;
                GLOG_INFO("VulkanRenderer2D: loaded system font '{}'", font_path);
                break;
            }
        }
    }

    if (!font_atlas_.texture()) {
        GLOG_WARN("VulkanRenderer2D: no system/bundled font loaded, creating fallback atlas");
        if (font_atlas_.create_fallback_atlas(ctx_, 32.0f)) {
            using_fallback_font_ = true;
            GLOG_INFO("VulkanRenderer2D: fallback atlas created");
        } else {
            GLOG_ERROR("VulkanRenderer2D: failed to create fallback font atlas, text rendering disabled");
        }
    }

    if (font_atlas_.texture()) {
        font_atlas_.texture()->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    }

    vertices_.reserve(4096);
    text_vertices_.reserve(512);
    sprite_vertices_.reserve(1024);
    lit_batches_.reserve(16);
    shadow_caster_vertices_.reserve(512);
    initialized_ = true;
    GLOG_INFO("VulkanRenderer2D initialized");
}

void VulkanRenderer2D::shutdown() {
    if (!initialized_) return;

    VkDevice dev = vk_device_ ? vk_device_->device() : VK_NULL_HANDLE;
    if (dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(dev);
    }

    destroy_bloom_targets();
    destroy_shadow_map();

    if (context_alive()) {
        if (fallback_albedo_tex_.is_valid()) ctx_->destroy_texture(fallback_albedo_tex_);
        if (fallback_normal_tex_.is_valid()) ctx_->destroy_texture(fallback_normal_tex_);
    }
    fallback_albedo_ = nullptr;
    fallback_normal_ = nullptr;

    if (context_alive() && font_atlas_.texture()) {
        font_atlas_.destroy(ctx_);
    }

    for (auto& buf : vertex_buffer_rect_) buf.shutdown();
    for (auto& buf : vertex_buffer_text_) buf.shutdown();
    for (auto& buf : vertex_buffer_sprite_) buf.shutdown();
    for (auto& buf : vertex_buffer_lit_sprite_) buf.shutdown();
    for (auto& buf : vertex_buffer_shadow_caster_) buf.shutdown();
    for (auto& buf : fs_vertex_buffer_) buf.shutdown();
    for (auto& buf : light_ubo_) buf.shutdown();

    if (dev != VK_NULL_HANDLE) {
        if (pipeline_rect_) vkDestroyPipeline(dev, pipeline_rect_, nullptr);
        if (pipeline_text_) vkDestroyPipeline(dev, pipeline_text_, nullptr);
        if (pipeline_sprite_) vkDestroyPipeline(dev, pipeline_sprite_, nullptr);
        if (pipeline_lit_sprite_) vkDestroyPipeline(dev, pipeline_lit_sprite_, nullptr);
        if (pipeline_rect_scene_) vkDestroyPipeline(dev, pipeline_rect_scene_, nullptr);
        if (pipeline_text_scene_) vkDestroyPipeline(dev, pipeline_text_scene_, nullptr);
        if (pipeline_sprite_scene_) vkDestroyPipeline(dev, pipeline_sprite_scene_, nullptr);
        if (pipeline_lit_sprite_scene_) vkDestroyPipeline(dev, pipeline_lit_sprite_scene_, nullptr);
        if (pipeline_shadow_) vkDestroyPipeline(dev, pipeline_shadow_, nullptr);
        if (pipeline_bloom_threshold_) vkDestroyPipeline(dev, pipeline_bloom_threshold_, nullptr);
        if (pipeline_bloom_blur_) vkDestroyPipeline(dev, pipeline_bloom_blur_, nullptr);
        if (pipeline_bloom_compose_) vkDestroyPipeline(dev, pipeline_bloom_compose_, nullptr);

        if (pipeline_layout_) vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr);
        if (lit_pipeline_layout_) vkDestroyPipelineLayout(dev, lit_pipeline_layout_, nullptr);
        if (shadow_pipeline_layout_) vkDestroyPipelineLayout(dev, shadow_pipeline_layout_, nullptr);
        if (bloom_pipeline_layout_) vkDestroyPipelineLayout(dev, bloom_pipeline_layout_, nullptr);
        if (bloom_compose_pipeline_layout_) vkDestroyPipelineLayout(dev, bloom_compose_pipeline_layout_, nullptr);

        for (auto& pool : descriptor_pools_) {
            if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
        if (descriptor_layout_) vkDestroyDescriptorSetLayout(dev, descriptor_layout_, nullptr);
        if (lit_descriptor_layout_) vkDestroyDescriptorSetLayout(dev, lit_descriptor_layout_, nullptr);

        if (vert_module_) vkDestroyShaderModule(dev, vert_module_, nullptr);
        if (vert_lit_module_) vkDestroyShaderModule(dev, vert_lit_module_, nullptr);
        if (frag_rect_module_) vkDestroyShaderModule(dev, frag_rect_module_, nullptr);
        if (frag_text_module_) vkDestroyShaderModule(dev, frag_text_module_, nullptr);
        if (frag_sprite_module_) vkDestroyShaderModule(dev, frag_sprite_module_, nullptr);
        if (frag_lit_sprite_module_) vkDestroyShaderModule(dev, frag_lit_sprite_module_, nullptr);
        if (vert_shadow_module_) vkDestroyShaderModule(dev, vert_shadow_module_, nullptr);
        if (frag_shadow_module_) vkDestroyShaderModule(dev, frag_shadow_module_, nullptr);
        if (vert_bloom_module_) vkDestroyShaderModule(dev, vert_bloom_module_, nullptr);
        if (frag_bloom_threshold_module_) vkDestroyShaderModule(dev, frag_bloom_threshold_module_, nullptr);
        if (frag_bloom_blur_module_) vkDestroyShaderModule(dev, frag_bloom_blur_module_, nullptr);
        if (frag_bloom_compose_module_) vkDestroyShaderModule(dev, frag_bloom_compose_module_, nullptr);
    }

    initialized_ = false;
    ctx_ = nullptr;
    ctx_lifetime_.reset();
    vk_backend_ = nullptr;
    vk_device_ = nullptr;
    vk_swapchain_ = nullptr;
}

bool VulkanRenderer2D::create_shader_modules() {
    std::string resolved = resources::ResourcePath::resolve("res:/shaders/spirv");
    if (resolved.empty()) {
        GLOG_ERROR("VulkanRenderer2D: failed to resolve shader directory");
        return false;
    }
    if (resolved.back() != '/' && resolved.back() != '\\') {
        resolved += '/';
    }

    std::vector<uint32_t> vert_code, vert_lit_code, frag_rect_code, frag_text_code,
        frag_sprite_code, frag_lit_code, vert_shadow_code, frag_shadow_code,
        vert_bloom_code, frag_bloom_threshold_code, frag_bloom_blur_code, frag_bloom_compose_code;

    if (!load_spirv_file(resolved + "vulkan_2d.vert.spv", vert_code) ||
        !load_spirv_file(resolved + "vulkan_2d_rect.frag.spv", frag_rect_code) ||
        !load_spirv_file(resolved + "vulkan_2d_text.frag.spv", frag_text_code) ||
        !load_spirv_file(resolved + "vulkan_2d_sprite.frag.spv", frag_sprite_code)) {
        return false;
    }

    bool has_lit = load_spirv_file(resolved + "vulkan_2d_lit.vert.spv", vert_lit_code) &&
                   load_spirv_file(resolved + "vulkan_2d_lit.frag.spv", frag_lit_code);
    bool has_shadow = load_spirv_file(resolved + "vulkan_2d_shadow.vert.spv", vert_shadow_code) &&
                      load_spirv_file(resolved + "vulkan_2d_shadow.frag.spv", frag_shadow_code);
    bool has_bloom = load_spirv_file(resolved + "vulkan_2d_bloom.vert.spv", vert_bloom_code) &&
                     load_spirv_file(resolved + "vulkan_2d_bloom_threshold.frag.spv", frag_bloom_threshold_code) &&
                     load_spirv_file(resolved + "vulkan_2d_bloom_blur.frag.spv", frag_bloom_blur_code) &&
                     load_spirv_file(resolved + "vulkan_2d_bloom_compose.frag.spv", frag_bloom_compose_code);

    if (!has_lit) {
        GLOG_WARN("VulkanRenderer2D: lit sprite SPIR-V not found, 2D lighting disabled in Vulkan");
    }
    if (!has_shadow) {
        GLOG_WARN("VulkanRenderer2D: shadow SPIR-V not found, 2D shadows disabled in Vulkan");
    }
    if (!has_bloom) {
        GLOG_WARN("VulkanRenderer2D: bloom SPIR-V not found, Bloom disabled in Vulkan");
    }

    VkDevice dev = vk_device_->device();
    vert_module_ = create_shader_module(dev, vert_code);
    frag_rect_module_ = create_shader_module(dev, frag_rect_code);
    frag_text_module_ = create_shader_module(dev, frag_text_code);
    frag_sprite_module_ = create_shader_module(dev, frag_sprite_code);
    if (has_lit) {
        vert_lit_module_ = create_shader_module(dev, vert_lit_code);
        frag_lit_sprite_module_ = create_shader_module(dev, frag_lit_code);
    }
    if (has_shadow) {
        vert_shadow_module_ = create_shader_module(dev, vert_shadow_code);
        frag_shadow_module_ = create_shader_module(dev, frag_shadow_code);
    }
    if (has_bloom) {
        vert_bloom_module_ = create_shader_module(dev, vert_bloom_code);
        frag_bloom_threshold_module_ = create_shader_module(dev, frag_bloom_threshold_code);
        frag_bloom_blur_module_ = create_shader_module(dev, frag_bloom_blur_code);
        frag_bloom_compose_module_ = create_shader_module(dev, frag_bloom_compose_code);
    }

    if (!vert_module_ || !frag_rect_module_ || !frag_text_module_ || !frag_sprite_module_) {
        GLOG_ERROR("VulkanRenderer2D: failed to create shader modules");
        return false;
    }
    return true;
}

bool VulkanRenderer2D::create_descriptor_layouts() {
    // 普通 2D descriptor：仅一个 combined image sampler
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 1;
        info.pBindings = &binding;

        if (vkCreateDescriptorSetLayout(vk_device_->device(), &info, nullptr, &descriptor_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create descriptor set layout");
            return false;
        }
    }

    // 受光照 sprite descriptor：albedo + normal + UBO + shadow map
    {
        VkDescriptorSetLayoutBinding bindings[4]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = 4;
        info.pBindings = bindings;

        if (vkCreateDescriptorSetLayout(vk_device_->device(), &info, nullptr, &lit_descriptor_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create lit descriptor set layout");
            return false;
        }
    }
    return true;
}

bool VulkanRenderer2D::create_pipeline_layouts() {
    // 普通 2D pipeline layout：只有 view_proj 矩阵
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.offset = 0;
        range.size = sizeof(math::Matrix4f);

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &descriptor_layout_;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(vk_device_->device(), &info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create pipeline layout");
            return false;
        }
    }

    // 受光照 sprite pipeline layout
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(math::Matrix4f);

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &lit_descriptor_layout_;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(vk_device_->device(), &info, nullptr, &lit_pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create lit pipeline layout");
            return false;
        }
    }

    // 阴影 pipeline layout：无 descriptor set
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.offset = 0;
        range.size = sizeof(math::Matrix4f);

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 0;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(vk_device_->device(), &info, nullptr, &shadow_pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create shadow pipeline layout");
            return false;
        }
    }

    // Bloom threshold / blur pipeline layout
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(math::Vector4f);

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &descriptor_layout_;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(vk_device_->device(), &info, nullptr, &bloom_pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create bloom pipeline layout");
            return false;
        }
    }

    // Bloom compose pipeline layout
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(math::Vector4f);

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &lit_descriptor_layout_;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(vk_device_->device(), &info, nullptr, &bloom_compose_pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create bloom compose pipeline layout");
            return false;
        }
    }
    return true;
}

VkPipeline VulkanRenderer2D::create_pipeline(VkShaderModule vert_module, VkShaderModule frag_module,
                                              VkPipelineLayout layout, VkRenderPass render_pass,
                                              uint32_t vertex_stride,
                                              const VkVertexInputAttributeDescription* attrs,
                                              uint32_t attr_count,
                                              bool depth_test, bool depth_write, bool blend,
                                              bool color_output) {
    VkDevice dev = vk_device_->device();

    VkPipelineShaderStageCreateInfo vert_stage{};
    vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage.module = vert_module;
    vert_stage.pName = "main";

    VkPipelineShaderStageCreateInfo frag_stage{};
    frag_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    frag_stage.module = frag_module;
    frag_stage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vert_stage, frag_stage};

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = vertex_stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = attr_count;
    vertex_input.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;

    VkPipelineColorBlendAttachmentState blend_attach{};
    blend_attach.blendEnable = blend ? VK_TRUE : VK_FALSE;
    blend_attach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attach.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attach.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attach.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend_state{};
    blend_state.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = color_output ? 1 : 0;
    blend_state.pAttachments = color_output ? &blend_attach : nullptr;

    VkDynamicState dynamics[6] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    uint32_t dynamic_count = 2;
    if (vk_backend_ && vk_backend_->supports_dynamic_state()) {
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_CULL_MODE_EXT;
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_FRONT_FACE_EXT;
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT;
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT;
    }
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = dynamic_count;
    dynamic_state.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth_stencil;
    info.pColorBlendState = &blend_state;
    info.pDynamicState = &dynamic_state;
    info.layout = layout;
    info.renderPass = render_pass;
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        GLOG_ERROR("VulkanRenderer2D: failed to create graphics pipeline");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

bool VulkanRenderer2D::create_swapchain_pipelines() {
    VkRenderPass swapchain_rp = vk_swapchain_->render_pass();

    // Vertex2D 属性：pos(0) + color(1) + uv(2)
    VkVertexInputAttributeDescription base_attrs[3]{};
    base_attrs[0].location = 0;
    base_attrs[0].binding = 0;
    base_attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    base_attrs[0].offset = 0;
    base_attrs[1].location = 1;
    base_attrs[1].binding = 0;
    base_attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    base_attrs[1].offset = 2 * sizeof(float);
    base_attrs[2].location = 2;
    base_attrs[2].binding = 0;
    base_attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    base_attrs[2].offset = 6 * sizeof(float);

    pipeline_rect_ = create_pipeline(vert_module_, frag_rect_module_, pipeline_layout_, swapchain_rp,
                                     sizeof(Vertex2D), base_attrs, 3, false, false, false);
    pipeline_text_ = create_pipeline(vert_module_, frag_text_module_, pipeline_layout_, swapchain_rp,
                                     sizeof(Vertex2D), base_attrs, 3, false, false, true);
    pipeline_sprite_ = create_pipeline(vert_module_, frag_sprite_module_, pipeline_layout_, swapchain_rp,
                                       sizeof(Vertex2D), base_attrs, 3, false, false, true);

    if (vert_lit_module_ != VK_NULL_HANDLE && frag_lit_sprite_module_ != VK_NULL_HANDLE) {
        VkVertexInputAttributeDescription lit_attrs[4]{};
        lit_attrs[0].location = 0;
        lit_attrs[0].binding = 0;
        lit_attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[0].offset = 0;
        lit_attrs[1].location = 1;
        lit_attrs[1].binding = 0;
        lit_attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        lit_attrs[1].offset = 2 * sizeof(float);
        lit_attrs[2].location = 2;
        lit_attrs[2].binding = 0;
        lit_attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[2].offset = 6 * sizeof(float);
        lit_attrs[3].location = 3;
        lit_attrs[3].binding = 0;
        lit_attrs[3].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[3].offset = 8 * sizeof(float);

        pipeline_lit_sprite_ = create_pipeline(vert_lit_module_, frag_lit_sprite_module_, lit_pipeline_layout_,
                                               swapchain_rp, sizeof(LitVertex2D), lit_attrs, 4,
                                               false, false, true);
    }

    if (pipeline_rect_ == VK_NULL_HANDLE || pipeline_text_ == VK_NULL_HANDLE ||
        pipeline_sprite_ == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D: failed to create swapchain pipelines");
        return false;
    }
    return true;
}

bool VulkanRenderer2D::create_descriptor_sets() {
    // One dynamic pool per frame-in-flight so resetting a pool never touches sets
    // that may still be executed by the GPU on an earlier frame.
    const int frames = vk_swapchain_ ? vk_swapchain_->frames_in_flight() : 2;
    constexpr uint32_t kMaxSetsPerFrame = 256;

    std::array<VkDescriptorPoolSize, 2> pool_sizes{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[0].descriptorCount = kMaxSetsPerFrame * 4; // up to 4 image bindings per set
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = kMaxSetsPerFrame;     // lit UBOs

    VkDevice dev = vk_device_->device();
    descriptor_pools_.resize(frames, VK_NULL_HANDLE);
    for (int i = 0; i < frames; ++i) {
        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.poolSizeCount = static_cast<uint32_t>(pool_sizes.size());
        pool_info.pPoolSizes = pool_sizes.data();
        pool_info.maxSets = kMaxSetsPerFrame;
        if (vkCreateDescriptorPool(dev, &pool_info, nullptr, &descriptor_pools_[i]) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create descriptor pool {}", i);
            return false;
        }
    }
    return true;
}

VkDescriptorSet VulkanRenderer2D::allocate_descriptor_set(VkDescriptorSetLayout layout) {
    int frame_index = vk_swapchain_ ? vk_swapchain_->current_frame_index() : 0;
    if (frame_index < 0 || frame_index >= static_cast<int>(descriptor_pools_.size()) ||
        descriptor_pools_[frame_index] == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = descriptor_pools_[frame_index];
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vk_device_->device(), &alloc, &set) != VK_SUCCESS) {
        GLOG_ERROR("VulkanRenderer2D: failed to allocate dynamic descriptor set");
        return VK_NULL_HANDLE;
    }
    return set;
}

bool VulkanRenderer2D::create_vertex_buffers() {
    const int frames = vk_swapchain_ ? vk_swapchain_->frames_in_flight() : 2;

    vertex_buffer_rect_.resize(frames);
    vertex_buffer_text_.resize(frames);
    vertex_buffer_sprite_.resize(frames);
    vertex_buffer_lit_sprite_.resize(frames);
    vertex_buffer_shadow_caster_.resize(frames);
    fs_vertex_buffer_.resize(frames);

    vertex_buffer_rect_capacity_.assign(frames, 4096 * sizeof(Vertex2D));
    vertex_buffer_text_capacity_.assign(frames, 4096 * sizeof(Vertex2D));
    vertex_buffer_sprite_capacity_.assign(frames, 4096 * sizeof(Vertex2D));
    vertex_buffer_lit_sprite_capacity_.assign(frames, 4096 * sizeof(LitVertex2D));
    vertex_buffer_shadow_caster_capacity_.assign(frames, 4096 * sizeof(ShadowCasterVertex));
    fs_vertex_buffer_capacity_.assign(frames, 3 * sizeof(FSVertex));

    for (int i = 0; i < frames; ++i) {
        if (!vertex_buffer_rect_[i].init(vk_device_, vertex_buffer_rect_capacity_[i],
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !vertex_buffer_text_[i].init(vk_device_, vertex_buffer_text_capacity_[i],
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !vertex_buffer_sprite_[i].init(vk_device_, vertex_buffer_sprite_capacity_[i],
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !vertex_buffer_lit_sprite_[i].init(vk_device_, vertex_buffer_lit_sprite_capacity_[i],
                                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !vertex_buffer_shadow_caster_[i].init(vk_device_, vertex_buffer_shadow_caster_capacity_[i],
                                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
            !fs_vertex_buffer_[i].init(vk_device_, fs_vertex_buffer_capacity_[i],
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer2D::create_uniform_buffers() {
    const int frames = vk_swapchain_ ? vk_swapchain_->frames_in_flight() : 2;
    light_ubo_.resize(frames);
    for (int i = 0; i < frames; ++i) {
        if (!light_ubo_[i].init(vk_device_, sizeof(LightUBO),
                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            return false;
        }
    }
    return true;
}

bool VulkanRenderer2D::create_fallback_textures() {
    fallback_albedo_tex_ = ctx_->create_texture();
    fallback_normal_tex_ = ctx_->create_texture();
    ITexture* albedo = ctx_->texture(fallback_albedo_tex_);
    ITexture* normal = ctx_->texture(fallback_normal_tex_);
    if (!albedo || !normal) return false;

    unsigned char white[] = {255, 255, 255, 255};
    unsigned char blue[] = {128, 128, 255, 255};
    if (!albedo->upload_data(white, 1, 1, 4) ||
        !normal->upload_data(blue, 1, 1, 4)) {
        GLOG_ERROR("VulkanRenderer2D: failed to create fallback textures");
        return false;
    }
    albedo->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    albedo->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    normal->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    normal->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    fallback_albedo_ = dynamic_cast<VulkanTexture*>(albedo);
    fallback_normal_ = dynamic_cast<VulkanTexture*>(normal);
    return fallback_albedo_ && fallback_normal_;
}

bool VulkanRenderer2D::create_shadow_map() {
    if (!ctx_) return false;
    if (vert_shadow_module_ == VK_NULL_HANDLE) return true; // 无阴影 shader，不影响初始化

    shadow_map_tex_ = ctx_->create_texture();
    ITexture* tex = ctx_->texture(shadow_map_tex_);
    if (!tex || !tex->create_depth(k_shadow_map_size, k_shadow_map_size)) {
        GLOG_ERROR("VulkanRenderer2D: failed to create shadow map texture");
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToBorder, TextureWrap::ClampToBorder);

    shadow_map_ = dynamic_cast<VulkanTexture*>(tex);
    if (!shadow_map_) return false;

    shadow_fb_ = ctx_->create_framebuffer();
    IFramebuffer* fb = ctx_->framebuffer(shadow_fb_);
    auto* vk_fb = dynamic_cast<VulkanFramebuffer*>(fb);
    if (!vk_fb || !vk_fb->create(k_shadow_map_size, k_shadow_map_size)) {
        GLOG_ERROR("VulkanRenderer2D: failed to create shadow framebuffer");
        return false;
    }
    vk_fb->attach_depth_texture(tex);
    if (!vk_fb->is_complete()) {
        GLOG_ERROR("VulkanRenderer2D: shadow framebuffer incomplete");
        return false;
    }

    VkVertexInputAttributeDescription shadow_attr{};
    shadow_attr.location = 0;
    shadow_attr.binding = 0;
    shadow_attr.format = VK_FORMAT_R32G32_SFLOAT;
    shadow_attr.offset = 0;

    pipeline_shadow_ = create_pipeline(vert_shadow_module_, frag_shadow_module_, shadow_pipeline_layout_,
                                       vk_fb->render_pass(), sizeof(ShadowCasterVertex),
                                       &shadow_attr, 1, true, true, false, false);
    if (pipeline_shadow_ == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D: failed to create shadow pipeline");
        return false;
    }

    return true;
}

void VulkanRenderer2D::destroy_shadow_map() {
    VkDevice dev = vk_device_ ? vk_device_->device() : VK_NULL_HANDLE;
    if (pipeline_shadow_ != VK_NULL_HANDLE && dev != VK_NULL_HANDLE) {
        vkDestroyPipeline(dev, pipeline_shadow_, nullptr);
        pipeline_shadow_ = VK_NULL_HANDLE;
    }
    if (context_alive()) {
        if (shadow_fb_.is_valid()) ctx_->destroy_framebuffer(shadow_fb_);
        if (shadow_map_tex_.is_valid()) ctx_->destroy_texture(shadow_map_tex_);
    }
    shadow_fb_ = RHIFramebufferHandle{};
    shadow_map_tex_ = RHITextureHandle{};
    shadow_map_ = nullptr;
}

bool VulkanRenderer2D::create_bloom_targets() {
    if (!ctx_ || screen_width_ <= 0.0f || screen_height_ <= 0.0f) {
        return false;
    }
    if (vert_bloom_module_ == VK_NULL_HANDLE) {
        return false;
    }

    destroy_bloom_pipelines();

    int w = static_cast<int>(screen_width_);
    int h = static_cast<int>(screen_height_);

    auto create_color_fb = [&](TextureFormat fmt, RHITextureHandle& tex, RHIFramebufferHandle& fb) -> bool {
        tex = ctx_->create_texture();
        auto* vk_tex = dynamic_cast<VulkanTexture*>(ctx_->texture(tex));
        if (!vk_tex || !vk_tex->create(fmt, w, h, nullptr)) {
            GLOG_ERROR("VulkanRenderer2D: failed to create bloom texture");
            return false;
        }
        vk_tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        vk_tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        fb = ctx_->create_framebuffer();
        auto* vk_fb = dynamic_cast<VulkanFramebuffer*>(ctx_->framebuffer(fb));
        if (!vk_fb || !vk_fb->create(w, h)) {
            GLOG_ERROR("VulkanRenderer2D: failed to create bloom framebuffer");
            return false;
        }
        vk_fb->attach_color_texture(vk_tex);
        if (!vk_fb->is_complete()) {
            GLOG_ERROR("VulkanRenderer2D: bloom framebuffer incomplete");
            return false;
        }
        return true;
    };

    if (!create_color_fb(TextureFormat::RGBA16F, scene_texture_, scene_fb_) ||
        !create_color_fb(TextureFormat::RGBA8, bloom_texture_a_, bloom_fb_a_) ||
        !create_color_fb(TextureFormat::RGBA8, bloom_texture_b_, bloom_fb_b_)) {
        destroy_bloom_targets();
        return false;
    }

    // scene FBO 默认清除为透明黑
    auto* scene_fb = dynamic_cast<VulkanFramebuffer*>(ctx_->framebuffer(scene_fb_));
    if (scene_fb) {
        scene_fb->set_clear_color(0.0f, 0.0f, 0.0f, 0.0f);
    }

    VkRenderPass scene_rp = scene_fb->render_pass();
    VkRenderPass bloom_rp = dynamic_cast<VulkanFramebuffer*>(ctx_->framebuffer(bloom_fb_a_))->render_pass();
    VkRenderPass swapchain_rp = vk_swapchain_->render_pass();

    // Vertex2D 基础属性
    VkVertexInputAttributeDescription base_attrs[3]{};
    base_attrs[0].location = 0;
    base_attrs[0].binding = 0;
    base_attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    base_attrs[0].offset = 0;
    base_attrs[1].location = 1;
    base_attrs[1].binding = 0;
    base_attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    base_attrs[1].offset = 2 * sizeof(float);
    base_attrs[2].location = 2;
    base_attrs[2].binding = 0;
    base_attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    base_attrs[2].offset = 6 * sizeof(float);

    pipeline_rect_scene_ = create_pipeline(vert_module_, frag_rect_module_, pipeline_layout_, scene_rp,
                                           sizeof(Vertex2D), base_attrs, 3, false, false, false);
    pipeline_text_scene_ = create_pipeline(vert_module_, frag_text_module_, pipeline_layout_, scene_rp,
                                           sizeof(Vertex2D), base_attrs, 3, false, false, true);
    pipeline_sprite_scene_ = create_pipeline(vert_module_, frag_sprite_module_, pipeline_layout_, scene_rp,
                                             sizeof(Vertex2D), base_attrs, 3, false, false, true);

    if (vert_lit_module_ != VK_NULL_HANDLE && frag_lit_sprite_module_ != VK_NULL_HANDLE) {
        VkVertexInputAttributeDescription lit_attrs[4]{};
        lit_attrs[0].location = 0;
        lit_attrs[0].binding = 0;
        lit_attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[0].offset = 0;
        lit_attrs[1].location = 1;
        lit_attrs[1].binding = 0;
        lit_attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        lit_attrs[1].offset = 2 * sizeof(float);
        lit_attrs[2].location = 2;
        lit_attrs[2].binding = 0;
        lit_attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[2].offset = 6 * sizeof(float);
        lit_attrs[3].location = 3;
        lit_attrs[3].binding = 0;
        lit_attrs[3].format = VK_FORMAT_R32G32_SFLOAT;
        lit_attrs[3].offset = 8 * sizeof(float);

        pipeline_lit_sprite_scene_ = create_pipeline(vert_lit_module_, frag_lit_sprite_module_, lit_pipeline_layout_,
                                                     scene_rp, sizeof(LitVertex2D), lit_attrs, 4,
                                                     false, false, true);
    }

    // 全屏后处理顶点属性
    VkVertexInputAttributeDescription fs_attrs[2]{};
    fs_attrs[0].location = 0;
    fs_attrs[0].binding = 0;
    fs_attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    fs_attrs[0].offset = 0;
    fs_attrs[1].location = 1;
    fs_attrs[1].binding = 0;
    fs_attrs[1].format = VK_FORMAT_R32G32_SFLOAT;
    fs_attrs[1].offset = 2 * sizeof(float);

    pipeline_bloom_threshold_ = create_pipeline(vert_bloom_module_, frag_bloom_threshold_module_,
                                                bloom_pipeline_layout_, bloom_rp,
                                                sizeof(FSVertex), fs_attrs, 2, false, false, false);
    pipeline_bloom_blur_ = create_pipeline(vert_bloom_module_, frag_bloom_blur_module_,
                                           bloom_pipeline_layout_, bloom_rp,
                                           sizeof(FSVertex), fs_attrs, 2, false, false, false);
    pipeline_bloom_compose_ = create_pipeline(vert_bloom_module_, frag_bloom_compose_module_,
                                              bloom_compose_pipeline_layout_, swapchain_rp,
                                              sizeof(FSVertex), fs_attrs, 2, false, false, false);

    if (pipeline_rect_scene_ == VK_NULL_HANDLE || pipeline_text_scene_ == VK_NULL_HANDLE ||
        pipeline_sprite_scene_ == VK_NULL_HANDLE ||
        (frag_lit_sprite_module_ != VK_NULL_HANDLE && pipeline_lit_sprite_scene_ == VK_NULL_HANDLE) ||
        pipeline_bloom_threshold_ == VK_NULL_HANDLE || pipeline_bloom_blur_ == VK_NULL_HANDLE ||
        pipeline_bloom_compose_ == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanRenderer2D: failed to create bloom pipelines");
        destroy_bloom_targets();
        return false;
    }

    bloom_initialized_ = true;
    return true;
}

void VulkanRenderer2D::destroy_bloom_targets() {
    destroy_bloom_pipelines();
    if (context_alive()) {
        if (scene_fb_.is_valid()) ctx_->destroy_framebuffer(scene_fb_);
        if (bloom_fb_a_.is_valid()) ctx_->destroy_framebuffer(bloom_fb_a_);
        if (bloom_fb_b_.is_valid()) ctx_->destroy_framebuffer(bloom_fb_b_);
        if (scene_texture_.is_valid()) ctx_->destroy_texture(scene_texture_);
        if (bloom_texture_a_.is_valid()) ctx_->destroy_texture(bloom_texture_a_);
        if (bloom_texture_b_.is_valid()) ctx_->destroy_texture(bloom_texture_b_);
    }
    scene_fb_ = RHIFramebufferHandle{};
    bloom_fb_a_ = RHIFramebufferHandle{};
    bloom_fb_b_ = RHIFramebufferHandle{};
    scene_texture_ = RHITextureHandle{};
    bloom_texture_a_ = RHITextureHandle{};
    bloom_texture_b_ = RHITextureHandle{};
    bloom_initialized_ = false;
}

void VulkanRenderer2D::destroy_bloom_pipelines() {
    VkDevice dev = vk_device_ ? vk_device_->device() : VK_NULL_HANDLE;
    if (dev == VK_NULL_HANDLE) return;

    auto destroy_if = [&](VkPipeline& p) {
        if (p != VK_NULL_HANDLE) {
            vkDestroyPipeline(dev, p, nullptr);
            p = VK_NULL_HANDLE;
        }
    };
    destroy_if(pipeline_rect_scene_);
    destroy_if(pipeline_text_scene_);
    destroy_if(pipeline_sprite_scene_);
    destroy_if(pipeline_lit_sprite_scene_);
    destroy_if(pipeline_bloom_threshold_);
    destroy_if(pipeline_bloom_blur_);
    destroy_if(pipeline_bloom_compose_);
}

void VulkanRenderer2D::write_lit_descriptor_set(VkDescriptorSet set, int frame_index,
                                                VulkanTexture* albedo, VulkanTexture* normal,
                                                bool use_shadow) {
    if (set == VK_NULL_HANDLE || !fallback_albedo_) return;
    if (frame_index < 0 || frame_index >= static_cast<int>(light_ubo_.size())) return;

    VkDevice dev = vk_device_->device();

    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = light_ubo_[frame_index].buffer();
    buffer_info.offset = 0;
    buffer_info.range = sizeof(LightUBO);

    VulkanTexture* albedo_tex = albedo && albedo->image_view() ? albedo : fallback_albedo_;
    VulkanTexture* normal_tex = normal && normal->image_view() ? normal : fallback_normal_;

    VkDescriptorImageInfo infos[4]{};
    infos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[0].imageView = albedo_tex->image_view();
    infos[0].sampler = albedo_tex->sampler();

    infos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    infos[1].imageView = normal_tex->image_view();
    infos[1].sampler = normal_tex->sampler();

    infos[2].imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    infos[2].imageView = use_shadow && shadow_map_ ? shadow_map_->image_view() : fallback_albedo_->image_view();
    infos[2].sampler = use_shadow && shadow_map_ ? shadow_map_->sampler() : fallback_albedo_->sampler();

    VkWriteDescriptorSet writes[4]{};
    // binding 0: albedo
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].descriptorCount = 1;
    writes[0].pImageInfo = &infos[0];
    // binding 1: normal
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].dstArrayElement = 0;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &infos[1];
    // binding 2: lights UBO
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].dstArrayElement = 0;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo = &buffer_info;
    // binding 3: shadow map
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 3;
    writes[3].dstArrayElement = 0;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].descriptorCount = 1;
    writes[3].pImageInfo = &infos[2];

    vkUpdateDescriptorSets(dev, 4, writes, 0, nullptr);
}

void VulkanRenderer2D::begin_frame(float screen_width, float screen_height) {
    vertices_.clear();
    text_vertices_.clear();
    sprite_vertices_.clear();
    sprite_texture_ = nullptr;
    lit_batches_.clear();
    shadow_caster_vertices_.clear();
    reset_lights();
    int frame_index = vk_swapchain_ ? vk_swapchain_->current_frame_index() : 0;
    if (frame_index >= 0 && frame_index < static_cast<int>(descriptor_pools_.size()) &&
        descriptor_pools_[frame_index] != VK_NULL_HANDLE) {
        vkResetDescriptorPool(vk_device_->device(), descriptor_pools_[frame_index], 0);
    }
    screen_width_ = screen_width;
    screen_height_ = screen_height;
    GLOG_INFO("VulkanRenderer2D::begin_frame screen={}x{} cam_center=({}, {}) zoom={}",
              screen_width_, screen_height_, camera_center_.x, camera_center_.y, camera_zoom_);
    ortho_ = math::Matrix4f::ortho(0.0f, screen_width, screen_height, 0.0f, -1.0f, 1.0f);

    math::Vector2f screen_center(screen_width * 0.5f, screen_height * 0.5f);
    math::Matrix4f view = math::Matrix4f::translate(screen_center.x, screen_center.y, 0.0f)
                        * math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
                        * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
    view_proj_ = ortho_ * view;

    if (bloom_params_.enabled && !bloom_initialized_ && screen_width_ > 0.0f && screen_height_ > 0.0f) {
        create_bloom_targets();
    }
    if (bloom_params_.enabled && bloom_initialized_) {
        ITexture* scene_tex = ctx_->texture(scene_texture_);
        bool need_recreate = !scene_tex ||
                             static_cast<int>(scene_tex->width()) != static_cast<int>(screen_width_) ||
                             static_cast<int>(scene_tex->height()) != static_cast<int>(screen_height_);
        if (need_recreate) {
            destroy_bloom_targets();
            create_bloom_targets();
        }
    }
}

void VulkanRenderer2D::set_camera(const math::Vector2f& center, float zoom) {
    camera_center_ = center;
    camera_zoom_ = zoom <= 0.0f ? 1.0f : zoom;
    if (screen_width_ > 0.0f && screen_height_ > 0.0f) {
        math::Vector2f screen_center(screen_width_ * 0.5f, screen_height_ * 0.5f);
        math::Matrix4f view = math::Matrix4f::translate(screen_center.x, screen_center.y, 0.0f)
                            * math::Matrix4f::scale(camera_zoom_, camera_zoom_, 1.0f)
                            * math::Matrix4f::translate(-camera_center_.x, -camera_center_.y, 0.0f);
        view_proj_ = ortho_ * view;
    }
}

math::Vector2f VulkanRenderer2D::world_to_screen(const math::Vector2f& world) const {
    math::Vector2f screen_center(screen_width_ * 0.5f, screen_height_ * 0.5f);
    return screen_center + (world - camera_center_) * camera_zoom_;
}

void VulkanRenderer2D::end_frame() {
    use_bloom_this_frame_ = bloom_params_.enabled && bloom_initialized_;
    bool did_shadow = !shadow_caster_vertices_.empty() && pipeline_shadow_ != VK_NULL_HANDLE && shadow_fb_.is_valid();

    // 1. 阴影 pass（在场景渲染前完成，lit sprite 采样）
    if (did_shadow) {
        render_shadow_pass();
    }

    // 2. 绑定到场景 FBO（Bloom）或回到 swapchain（刚完成阴影 pass）
    if (use_bloom_this_frame_) {
        ctx_->push_command([this](IRenderBackend*) {
            vk_backend_->bind_framebuffer(scene_fb_);
        });
    } else if (did_shadow) {
        ctx_->push_command([this](IRenderBackend*) {
            vk_backend_->unbind_framebuffer();
        });
    }

    // 3. 渲染不受光照的 2D 几何体
    flush_batches();
    flush_sprite_batch();

    // 4. 前向光照
    if (!lit_batches_.empty() && lit_pipeline_layout_ != VK_NULL_HANDLE) {
        render_lit_sprites_forward(use_bloom_this_frame_);
    }

    // 5. Bloom 后处理
    if (use_bloom_this_frame_) {
        render_bloom_pass();
    }
}

void VulkanRenderer2D::render_shadow_pass() {
    if (!ctx_ || shadow_caster_vertices_.empty() || !shadow_fb_.is_valid() ||
        pipeline_shadow_ == VK_NULL_HANDLE) {
        return;
    }

    // 找到第一个投射阴影的光源（Directional 优先，否则 Spot）
    const Light2D* shadow_light = nullptr;
    int shadow_light_index = -1;
    for (int i = 0; i < static_cast<int>(lights_.size()); ++i) {
        if (lights_[i].type == LightType2D::Directional || lights_[i].type == LightType2D::Spot) {
            shadow_light = &lights_[i];
            shadow_light_index = i;
            break;
        }
    }
    if (!shadow_light) return;

    math::Vector2f dir = shadow_light->direction.normalized();
    if (dir.length_sq() < 1e-6f) dir = math::Vector2f(0.0f, -1.0f);

    math::Vector2f center = camera_center_;
    float view_size = std::max(screen_width_, screen_height_) / camera_zoom_;
    math::Vector2f eye = center - dir * view_size;
    math::Matrix4f light_view = math::Matrix4f::look_at(
        math::Vector3f(eye.x, eye.y, 0.0f),
        math::Vector3f(center.x, center.y, 0.0f),
        math::Vector3f(0.0f, 0.0f, 1.0f));
    math::Matrix4f light_proj = math::Matrix4f::ortho(
        -view_size, view_size, -view_size, view_size, 0.1f, view_size * 2.0f);
    math::Matrix4f light_space = light_proj * light_view;

    auto verts_shared = std::make_shared<std::vector<ShadowCasterVertex>>(std::move(shadow_caster_vertices_));
    shadow_caster_vertices_.clear();

    ctx_->push_command([this, verts_shared, light_space](IRenderBackend*) {
        vk_backend_->bind_framebuffer(shadow_fb_);

        int frame_index = vk_swapchain_->current_frame_index();
        if (frame_index < 0 || frame_index >= static_cast<int>(vertex_buffer_shadow_caster_.size())) {
            return;
        }

        VulkanBuffer* vertex_buffer = &vertex_buffer_shadow_caster_[frame_index];
        VkDeviceSize* capacity = &vertex_buffer_shadow_caster_capacity_[frame_index];
        VkDeviceSize required = verts_shared->size() * sizeof(ShadowCasterVertex);
        if (required > *capacity) {
            vertex_buffer->shutdown();
            *capacity = required * 2;
            if (!vertex_buffer->init(vk_device_, *capacity,
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                GLOG_ERROR("VulkanRenderer2D: failed to resize shadow caster vertex buffer");
                return;
            }
        }
        vertex_buffer->upload(verts_shared->data(), required);

        VkCommandBuffer cmd = vk_backend_->current_command_buffer();
        if (cmd == VK_NULL_HANDLE) return;

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(k_shadow_map_size);
        viewport.height = static_cast<float>(k_shadow_map_size);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vk_backend_->set_viewport_cached(cmd, viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {static_cast<uint32_t>(k_shadow_map_size), static_cast<uint32_t>(k_shadow_map_size)};
        vk_backend_->set_scissor_cached(cmd, scissor);

        vk_backend_->bind_pipeline(cmd, pipeline_shadow_);

        if (vk_backend_->supports_dynamic_state()) {
            vk_backend_->set_cull_mode_cached(cmd, VK_CULL_MODE_NONE);
            vk_backend_->set_front_face_cached(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
            vk_backend_->set_depth_test_cached(cmd, VK_TRUE);
            vk_backend_->set_depth_write_cached(cmd, VK_TRUE);
        }

        vkCmdPushConstants(cmd, shadow_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(math::Matrix4f), &light_space);

        VkBuffer buffers[] = {vertex_buffer->buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
        vkCmdDraw(cmd, static_cast<uint32_t>(verts_shared->size()), 1, 0, 0);
    });
}

void VulkanRenderer2D::render_lit_sprites_forward(bool offscreen) {
    if (lit_batches_.empty() || !lit_pipeline_layout_) return;

    VkPipeline lit_pipeline = offscreen ? pipeline_lit_sprite_scene_ : pipeline_lit_sprite_;
    if (lit_pipeline == VK_NULL_HANDLE) return;

    // 构建光源 UBO
    LightUBO ubo{};
    ubo.ambient = math::Vector3f(ambient_light_.r, ambient_light_.g, ambient_light_.b);
    int light_count = static_cast<int>(std::min<size_t>(lights_.size(), k_max_lights));
    ubo.light_count = light_count;
    for (int i = 0; i < light_count; ++i) {
        const Light2D& L = lights_[i];
        LightData& d = ubo.lights[i];
        d.type = static_cast<int>(L.type);
        d.pos = L.position;
        d.dir = L.direction;
        d.color[0] = L.color.r;
        d.color[1] = L.color.g;
        d.color[2] = L.color.b;
        d.intensity = L.intensity;
        d.radius = L.radius;
        d.range = L.range;
        d.spot_angle = L.spot_angle;
        d.spot_softness = L.spot_softness;
    }

    int shadow_light_index = -1;
    for (int i = 0; i < light_count; ++i) {
        if (lights_[i].type == LightType2D::Directional || lights_[i].type == LightType2D::Spot) {
            shadow_light_index = i;
            break;
        }
    }
    bool use_shadow = (shadow_light_index >= 0 && shadow_fb_.is_valid() && shadow_map_);
    ubo.use_shadow_map = use_shadow ? 1 : 0;
    ubo.shadow_light_index = shadow_light_index;
    if (use_shadow) {
        const Light2D& shadow_light = lights_[shadow_light_index];
        math::Vector2f dir = shadow_light.direction.normalized();
        if (dir.length_sq() < 1e-6f) dir = math::Vector2f(0.0f, -1.0f);
        math::Vector2f center = camera_center_;
        float view_size = std::max(screen_width_, screen_height_) / camera_zoom_;
        math::Vector2f eye = center - dir * view_size;
        math::Matrix4f light_view = math::Matrix4f::look_at(
            math::Vector3f(eye.x, eye.y, 0.0f),
            math::Vector3f(center.x, center.y, 0.0f),
            math::Vector3f(0.0f, 0.0f, 1.0f));
        math::Matrix4f light_proj = math::Matrix4f::ortho(
            -view_size, view_size, -view_size, view_size, 0.1f, view_size * 2.0f);
        ubo.light_space_matrix = light_proj * light_view;
    }

    auto batches_copy = std::make_shared<std::vector<LitBatch>>();
    batches_copy->reserve(lit_batches_.size());
    for (auto& batch : lit_batches_) {
        batches_copy->push_back({batch.albedo, batch.normal, std::vector<LitVertex2D>(batch.verts)});
    }
    lit_batches_.clear();

    math::Matrix4f view_proj = view_proj_;
    auto ubo_copy = std::make_shared<LightUBO>(ubo);

    // 上传 UBO
    ctx_->push_command([this, ubo_copy](IRenderBackend*) {
        int frame_index = vk_swapchain_->current_frame_index();
        if (frame_index < 0 || frame_index >= static_cast<int>(light_ubo_.size())) return;
        light_ubo_[frame_index].upload(ubo_copy.get(), sizeof(LightUBO));
    });

    // 逐批次绘制
    for (auto& batch : *batches_copy) {
        auto batch_shared = std::make_shared<LitBatch>(std::move(batch));
        ctx_->push_command([this, batch_shared, lit_pipeline, view_proj, use_shadow](IRenderBackend*) {
            if (batch_shared->verts.empty()) return;

            int frame_index = vk_swapchain_->current_frame_index();
            if (frame_index < 0 || frame_index >= static_cast<int>(vertex_buffer_lit_sprite_.size())) return;

            VulkanBuffer* vertex_buffer = &vertex_buffer_lit_sprite_[frame_index];
            VkDeviceSize* capacity = &vertex_buffer_lit_sprite_capacity_[frame_index];
            VkDeviceSize required = batch_shared->verts.size() * sizeof(LitVertex2D);
            if (required > *capacity) {
                vertex_buffer->shutdown();
                *capacity = required * 2;
                if (!vertex_buffer->init(vk_device_, *capacity,
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                    GLOG_ERROR("VulkanRenderer2D: failed to resize lit sprite vertex buffer");
                    return;
                }
            }
            vertex_buffer->upload(batch_shared->verts.data(), required);

            VkCommandBuffer cmd = vk_backend_->current_command_buffer();
            if (cmd == VK_NULL_HANDLE) return;

            VkViewport viewport{};
            viewport.x = 0.0f;
            viewport.y = screen_height_;
            viewport.width = screen_width_;
            viewport.height = -screen_height_;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vk_backend_->set_viewport_cached(cmd, viewport);

            VkRect2D scissor{};
            scissor.offset = {0, 0};
            scissor.extent = {static_cast<uint32_t>(screen_width_), static_cast<uint32_t>(screen_height_)};
            vk_backend_->set_scissor_cached(cmd, scissor);

            vk_backend_->bind_pipeline(cmd, lit_pipeline);
            vk_backend_->set_dynamic_state_2d(cmd);

            vkCmdPushConstants(cmd, lit_pipeline_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(math::Matrix4f), &view_proj);

            VkDescriptorSet set = allocate_descriptor_set(lit_descriptor_layout_);
            if (set == VK_NULL_HANDLE) {
                GLOG_ERROR("VulkanRenderer2D::render_lit_sprites_forward failed to allocate descriptor set");
                return;
            }
            write_lit_descriptor_set(set, frame_index,
                                     batch_shared->albedo ? dynamic_cast<VulkanTexture*>(batch_shared->albedo) : nullptr,
                                     batch_shared->normal ? dynamic_cast<VulkanTexture*>(batch_shared->normal) : nullptr,
                                     use_shadow);
            vk_backend_->bind_descriptor_set(cmd, lit_pipeline_layout_, set);

            VkBuffer buffers[] = {vertex_buffer->buffer()};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
            vkCmdDraw(cmd, static_cast<uint32_t>(batch_shared->verts.size()), 1, 0, 0);
        });
    }
}

void VulkanRenderer2D::render_bloom_pass() {
    if (!bloom_initialized_) return;

    int w = static_cast<int>(screen_width_);
    int h = static_cast<int>(screen_height_);
    float inv_w = 1.0f / static_cast<float>(w);
    float inv_h = 1.0f / static_cast<float>(h);

    // 1. Threshold: scene -> bloom_a
    draw_fullscreen_pass(bloom_fb_a_, pipeline_bloom_threshold_, bloom_pipeline_layout_,
                         false, {{scene_texture_, 0}},
                         &bloom_params_.threshold, sizeof(bloom_params_.threshold));

    // 2. Blur passes: bloom_a <-> bloom_b
    RHITextureHandle src = bloom_texture_a_;
    RHIFramebufferHandle dst = bloom_fb_b_;
    for (int i = 0; i < bloom_params_.blur_passes * 2; ++i) {
        bool horizontal = (i % 2) == 0;
        math::Vector2f dir = horizontal ? math::Vector2f(1.0f, 0.0f) : math::Vector2f(0.0f, 1.0f);
        math::Vector4f push(dir.x, dir.y, inv_w, inv_h);
        draw_fullscreen_pass(dst, pipeline_bloom_blur_, bloom_pipeline_layout_,
                             false, {{src, 0}},
                             &push, sizeof(push));
        std::swap(src, bloom_texture_a_);
        std::swap(dst, bloom_fb_a_);
    }
    // 最终 bloom 结果在 src 中

    // 3. Compose: scene + bloom -> backbuffer
    draw_fullscreen_pass(RHIFramebufferHandle{}, pipeline_bloom_compose_, bloom_compose_pipeline_layout_,
                         true, {{scene_texture_, 0}, {src, 1}},
                         &bloom_params_.intensity, sizeof(bloom_params_.intensity));
}

void VulkanRenderer2D::draw_fullscreen_pass(RHIFramebufferHandle fb,
                                             VkPipeline pipeline, VkPipelineLayout layout,
                                             bool use_lit_descriptor_set,
                                             const std::vector<std::pair<RHITextureHandle, uint32_t>>& bindings,
                                             const void* push_constants, size_t push_size) {
    if (pipeline == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) return;
    if (push_size > 16) return;

    float screen_w = screen_width_;
    float screen_h = screen_height_;

    std::array<uint8_t, 16> push_data{};
    if (push_size > 0) {
        std::memcpy(push_data.data(), push_constants, push_size);
    }

    ctx_->push_command([=](IRenderBackend*) mutable {
        if (fb.is_valid()) {
            vk_backend_->bind_framebuffer(fb);
        } else {
            vk_backend_->unbind_framebuffer();
        }

        int frame_index = vk_swapchain_->current_frame_index();
        if (frame_index < 0 || frame_index >= static_cast<int>(fs_vertex_buffer_.size())) return;

        VkDescriptorSetLayout set_layout = use_lit_descriptor_set ? lit_descriptor_layout_ : descriptor_layout_;
        VkDescriptorSet set = allocate_descriptor_set(set_layout);
        if (set == VK_NULL_HANDLE) {
            GLOG_ERROR("VulkanRenderer2D::draw_fullscreen_pass failed to allocate descriptor set");
            return;
        }

        VulkanBuffer* vertex_buffer = &fs_vertex_buffer_[frame_index];
        FSVertex verts[3] = {
            {-1.0f, -1.0f, 0.0f, 0.0f},
            { 3.0f, -1.0f, 2.0f, 0.0f},
            {-1.0f,  3.0f, 0.0f, 2.0f}
        };
        vertex_buffer->upload(verts, sizeof(verts));

        VkCommandBuffer cmd = vk_backend_->current_command_buffer();
        if (cmd == VK_NULL_HANDLE) return;

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = screen_h;
        viewport.width = screen_w;
        viewport.height = -screen_h;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vk_backend_->set_viewport_cached(cmd, viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {static_cast<uint32_t>(screen_w), static_cast<uint32_t>(screen_h)};
        vk_backend_->set_scissor_cached(cmd, scissor);

        vk_backend_->bind_pipeline(cmd, pipeline);
        vk_backend_->set_dynamic_state_2d(cmd);

        // 更新 descriptor image bindings
        std::vector<VkDescriptorImageInfo> image_infos(bindings.size());
        std::vector<VkWriteDescriptorSet> writes(bindings.size());
        for (size_t i = 0; i < bindings.size(); ++i) {
            ITexture* tex = ctx_->texture(bindings[i].first);
            auto* vk_tex = dynamic_cast<VulkanTexture*>(tex);
            if (!vk_tex || !vk_tex->image_view()) vk_tex = fallback_albedo_;
            image_infos[i].imageLayout = (bindings[i].second == 3)
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            image_infos[i].imageView = vk_tex->image_view();
            image_infos[i].sampler = vk_tex->sampler();

            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = bindings[i].second;
            writes[i].dstArrayElement = 0;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo = &image_infos[i];
        }
        if (!writes.empty()) {
            vkUpdateDescriptorSets(vk_device_->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        vk_backend_->bind_descriptor_set(cmd, layout, set);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           static_cast<uint32_t>(push_size), push_data.data());

        VkBuffer buffers[] = {vertex_buffer->buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    });
}

void VulkanRenderer2D::push_vertex(float x, float y, const Color& color, float u, float v) {
    vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

void VulkanRenderer2D::push_text_vertex(float x, float y, const Color& color, float u, float v) {
    text_vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

void VulkanRenderer2D::push_sprite_vertex(float x, float y, const Color& color, float u, float v) {
    sprite_vertices_.push_back({x, y, color.r, color.g, color.b, color.a, u, v});
}

void VulkanRenderer2D::push_lit_vertex(ITexture* albedo, ITexture* normal,
                                        float x, float y, const Color& color,
                                        float u, float v, float nu, float nv) {
    auto it = find_lit_batch(albedo, normal);
    it->verts.push_back({x, y, color.r, color.g, color.b, color.a, u, v, nu, nv});
}

void VulkanRenderer2D::push_shadow_caster_vertex(float x, float y) {
    shadow_caster_vertices_.push_back({x, y});
}

std::vector<VulkanRenderer2D::LitBatch>::iterator VulkanRenderer2D::find_lit_batch(ITexture* albedo, ITexture* normal) {
    for (auto it = lit_batches_.begin(); it != lit_batches_.end(); ++it) {
        if (it->albedo == albedo && it->normal == normal) {
            return it;
        }
    }
    lit_batches_.push_back({albedo, normal, {}});
    return std::prev(lit_batches_.end());
}

void VulkanRenderer2D::flush_batches() {
    VkPipeline rect_pipeline = use_bloom_this_frame_ ? pipeline_rect_scene_ : pipeline_rect_;
    VkPipeline text_pipeline = use_bloom_this_frame_ ? pipeline_text_scene_ : pipeline_text_;
    flush_batch(std::move(vertices_), false, nullptr, rect_pipeline, pipeline_layout_);
    flush_batch(std::move(text_vertices_), true, font_atlas_.texture(), text_pipeline, pipeline_layout_);
}

void VulkanRenderer2D::flush_sprite_batch() {
    if (sprite_vertices_.empty() || !sprite_texture_) return;
    GLOG_INFO("VulkanRenderer2D::flush_sprite_batch: verts={} texture={} format={}",
              sprite_vertices_.size(),
              reinterpret_cast<uintptr_t>(sprite_texture_),
              static_cast<int>(dynamic_cast<VulkanTexture*>(sprite_texture_)->format()));
    VkPipeline pipeline = use_bloom_this_frame_ ? pipeline_sprite_scene_ : pipeline_sprite_;
    flush_batch(std::move(sprite_vertices_), false, sprite_texture_, pipeline, pipeline_layout_);
    sprite_texture_ = nullptr;
}

void VulkanRenderer2D::flush_batch(std::vector<Vertex2D>&& verts, bool is_text, ITexture* texture,
                                    VkPipeline pipeline, VkPipelineLayout layout) {
    const bool is_sprite = (!is_text && texture != nullptr);

    if (verts.empty() || !context_alive() || !vk_backend_ || pipeline == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) {
        return;
    }

    auto verts_shared = std::make_shared<std::vector<Vertex2D>>(std::move(verts));
    math::Matrix4f view_proj = view_proj_;
    ITexture* tex = texture;
    float screen_w = screen_width_;
    float screen_h = screen_height_;

    ctx_->push_command([this, verts_shared, is_text, is_sprite, tex, pipeline, layout,
                        screen_w, screen_h, view_proj](IRenderBackend*) {
        int frame_index = vk_swapchain_->current_frame_index();
        if (frame_index < 0 || frame_index >= static_cast<int>(vertex_buffer_rect_.size())) {
            return;
        }

        VulkanBuffer* vertex_buffer = nullptr;
        VkDeviceSize* capacity = nullptr;
        if (is_text) {
            vertex_buffer = &vertex_buffer_text_[frame_index];
            capacity = &vertex_buffer_text_capacity_[frame_index];
        } else if (is_sprite) {
            vertex_buffer = &vertex_buffer_sprite_[frame_index];
            capacity = &vertex_buffer_sprite_capacity_[frame_index];
        } else {
            vertex_buffer = &vertex_buffer_rect_[frame_index];
            capacity = &vertex_buffer_rect_capacity_[frame_index];
        }

        VkDeviceSize required = verts_shared->size() * sizeof(Vertex2D);
        if (required > *capacity) {
            vertex_buffer->shutdown();
            *capacity = required * 2;
            if (!vertex_buffer->init(vk_device_, *capacity,
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                GLOG_ERROR("VulkanRenderer2D: failed to resize vertex buffer");
                return;
            }
        }
        vertex_buffer->upload(verts_shared->data(), required);

        VkCommandBuffer cmd = vk_backend_->current_command_buffer();
        if (cmd == VK_NULL_HANDLE) return;

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = screen_h;
        viewport.width = screen_w;
        viewport.height = -screen_h;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        GLOG_INFO("VulkanRenderer2D::flush_batch viewport={} {} {} {} verts={}",
                  viewport.x, viewport.y, viewport.width, viewport.height, verts_shared->size());
        vk_backend_->set_viewport_cached(cmd, viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {static_cast<uint32_t>(screen_w), static_cast<uint32_t>(screen_h)};
        vk_backend_->set_scissor_cached(cmd, scissor);

        vk_backend_->bind_pipeline(cmd, pipeline);
        vk_backend_->set_dynamic_state_2d(cmd);

        struct alignas(16) PushConstants {
            math::Matrix4f view_proj;
        } pc;
        pc.view_proj = view_proj;
        GLOG_INFO("VulkanRenderer2D::flush_batch view_proj col0=({}, {}, {}, {}) col1=({}, {}, {}, {}) col3=({}, {}, {}, {})",
                  pc.view_proj(0,0), pc.view_proj(1,0), pc.view_proj(2,0), pc.view_proj(3,0),
                  pc.view_proj(0,1), pc.view_proj(1,1), pc.view_proj(2,1), pc.view_proj(3,1),
                  pc.view_proj(0,3), pc.view_proj(1,3), pc.view_proj(2,3), pc.view_proj(3,3));
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(PushConstants), &pc);

        // Allocate a fresh descriptor set for this draw batch so we never update a
        // set that is already referenced by recorded commands.
        VkDescriptorSet set = allocate_descriptor_set(descriptor_layout_);
        if (set == VK_NULL_HANDLE) {
            GLOG_ERROR("VulkanRenderer2D::flush_batch failed to allocate descriptor set");
            return;
        }

        VulkanTexture* tex_to_bind = nullptr;
        if (is_text) {
            tex_to_bind = font_atlas_.texture() ? dynamic_cast<VulkanTexture*>(font_atlas_.texture()) : nullptr;
        } else if (is_sprite) {
            tex_to_bind = tex ? dynamic_cast<VulkanTexture*>(tex) : nullptr;
        } else {
            tex_to_bind = fallback_albedo_;
        }
        if (!tex_to_bind || !tex_to_bind->image_view() || !tex_to_bind->sampler()) {
            tex_to_bind = fallback_albedo_;
        }

        VkDescriptorImageInfo image_info{};
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.imageView = tex_to_bind->image_view();
        image_info.sampler = tex_to_bind->sampler();

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(vk_device_->device(), 1, &write, 0, nullptr);

        vk_backend_->bind_descriptor_set(cmd, layout, set);

        VkBuffer buffers[] = {vertex_buffer->buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, buffers, offsets);
        vkCmdDraw(cmd, static_cast<uint32_t>(verts_shared->size()), 1, 0, 0);
    });
}

void VulkanRenderer2D::draw_rect(float x, float y, float w, float h, const Color& color) {
    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    push_vertex(x0, y0, color, 0, 0);
    push_vertex(x1, y0, color, 0, 0);
    push_vertex(x1, y1, color, 0, 0);

    push_vertex(x0, y0, color, 0, 0);
    push_vertex(x1, y1, color, 0, 0);
    push_vertex(x0, y1, color, 0, 0);
}

void VulkanRenderer2D::draw_polygon(const std::vector<math::Vector2f>& points, const Color& color) {
    if (points.size() < 3) return;
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        push_vertex(points[0].x, points[0].y, color, 0, 0);
        push_vertex(points[i].x, points[i].y, color, 0, 0);
        push_vertex(points[i + 1].x, points[i + 1].y, color, 0, 0);
    }
}

void VulkanRenderer2D::draw_circle(float cx, float cy, float r, int segments, const Color& color) {
    if (segments < 3) segments = 3;
    const float pi = 3.14159265358979323846f;
    for (int i = 0; i < segments; ++i) {
        float a0 = 2.0f * pi * static_cast<float>(i) / static_cast<float>(segments);
        float a1 = 2.0f * pi * static_cast<float>(i + 1) / static_cast<float>(segments);
        float x0 = cx + r * std::cos(a0);
        float y0 = cy + r * std::sin(a0);
        float x1 = cx + r * std::cos(a1);
        float y1 = cy + r * std::sin(a1);

        push_vertex(cx, cy, color, 0, 0);
        push_vertex(x0, y0, color, 0, 0);
        push_vertex(x1, y1, color, 0, 0);
    }
}

void VulkanRenderer2D::draw_sprite(float x, float y, float w, float h,
                                    ITexture* texture, const Color& tint) {
    draw_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, texture, tint);
}

void VulkanRenderer2D::draw_sprite_region(float x, float y, float w, float h,
                                           float u0, float v0, float u1, float v1,
                                           ITexture* texture, const Color& tint) {
    if (!texture || !texture->is_valid()) {
        draw_rect(x, y, w, h, tint);
        return;
    }

    if (texture != sprite_texture_) {
        flush_sprite_batch();
        sprite_texture_ = texture;
    }

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    push_sprite_vertex(x0, y0, tint, u0, v0);
    push_sprite_vertex(x1, y0, tint, u1, v0);
    push_sprite_vertex(x1, y1, tint, u1, v1);

    push_sprite_vertex(x0, y0, tint, u0, v0);
    push_sprite_vertex(x1, y1, tint, u1, v1);
    push_sprite_vertex(x0, y1, tint, u0, v1);
}

void VulkanRenderer2D::set_ambient_light(const Color& color) {
    ambient_light_ = color;
}

void VulkanRenderer2D::add_light(const Light2D& light) {
    if (static_cast<int>(lights_.size()) < k_max_lights) {
        lights_.push_back(light);
    }
}

void VulkanRenderer2D::add_point_light(const math::Vector2f& pos, float radius,
                                        const Color& color, float intensity) {
    Light2D light;
    light.type = LightType2D::Point;
    light.position = pos;
    light.radius = radius;
    light.color = color;
    light.intensity = intensity;
    add_light(light);
}

void VulkanRenderer2D::reset_lights() {
    ambient_light_ = Color::black();
    lights_.clear();
}

void VulkanRenderer2D::draw_lit_sprite(float x, float y, float w, float h,
                                        ITexture* albedo, ITexture* normal_map,
                                        const Color& tint) {
    draw_lit_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f,
                           albedo, normal_map, tint,
                           0.0f, 0.0f, 1.0f, 1.0f);
}

void VulkanRenderer2D::draw_lit_sprite_region(float x, float y, float w, float h,
                                               float u0, float v0, float u1, float v1,
                                               ITexture* albedo, ITexture* normal_map,
                                               const Color& tint,
                                               float nu0, float nv0,
                                               float nu1, float nv1) {
    if (!albedo || !albedo->is_valid()) {
        draw_rect(x, y, w, h, tint);
        return;
    }

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    push_lit_vertex(albedo, normal_map, x0, y0, tint, u0, v0, nu0, nv0);
    push_lit_vertex(albedo, normal_map, x1, y0, tint, u1, v0, nu1, nv0);
    push_lit_vertex(albedo, normal_map, x1, y1, tint, u1, v1, nu1, nv1);

    push_lit_vertex(albedo, normal_map, x0, y0, tint, u0, v0, nu0, nv0);
    push_lit_vertex(albedo, normal_map, x1, y1, tint, u1, v1, nu1, nv1);
    push_lit_vertex(albedo, normal_map, x0, y1, tint, u0, v1, nu0, nv1);
}

void VulkanRenderer2D::draw_shadow_caster(float x, float y, float w, float h) {
    if (!initialized_) return;
    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;
    push_shadow_caster_vertex(x0, y0);
    push_shadow_caster_vertex(x1, y0);
    push_shadow_caster_vertex(x1, y1);
    push_shadow_caster_vertex(x0, y0);
    push_shadow_caster_vertex(x1, y1);
    push_shadow_caster_vertex(x0, y1);
}

void VulkanRenderer2D::set_bloom(const BloomParams& params) {
    bloom_params_ = params;
    if (bloom_params_.enabled && !bloom_initialized_ && screen_width_ > 0.0f && screen_height_ > 0.0f) {
        create_bloom_targets();
    }
    if (!bloom_params_.enabled && bloom_initialized_) {
        destroy_bloom_targets();
    }
}

void VulkanRenderer2D::draw_text(float x, float y, const std::string& text, float font_size, const Color& color) {
    if (!font_atlas_.texture()) {
        float cursor_x = x;
        float cursor_y = y;
        float block_w = font_size * 0.6f;
        float block_h = font_size;
        for (char c : text) {
            if (c == '\n') {
                cursor_x = x;
                cursor_y += block_h;
                continue;
            }
            if (c != ' ') {
                draw_rect(cursor_x, cursor_y - block_h * 0.8f, block_w, block_h, color);
            }
            cursor_x += block_w;
        }
        return;
    }

    float scale = font_size / font_atlas_.font_size();
    float cursor_x = x;
    float cursor_y = y;

    for (char c : text) {
        if (c == '\n') {
            cursor_x = x;
            cursor_y += font_atlas_.font_size() * scale;
            continue;
        }

        const Glyph* g = font_atlas_.get_glyph(c);
        if (!g) continue;

        float x0 = cursor_x + g->offset_x * scale;
        float y0 = cursor_y + g->offset_y * scale;
        float x1 = x0 + g->width * scale;
        float y1 = y0 + g->height * scale;

        push_text_vertex(x0, y0, color, g->uv0_x, g->uv0_y);
        push_text_vertex(x1, y0, color, g->uv1_x, g->uv0_y);
        push_text_vertex(x1, y1, color, g->uv1_x, g->uv1_y);

        push_text_vertex(x0, y0, color, g->uv0_x, g->uv0_y);
        push_text_vertex(x1, y1, color, g->uv1_x, g->uv1_y);
        push_text_vertex(x0, y1, color, g->uv0_x, g->uv1_y);

        cursor_x += g->advance * scale;
    }
}

void VulkanRenderer2D::draw_font_atlas_debug(float x, float y, float size) {
    if (!font_atlas_.texture()) return;

    Color c = Color::white();
    push_text_vertex(x, y, c, 0.0f, 0.0f);
    push_text_vertex(x + size, y, c, 1.0f, 0.0f);
    push_text_vertex(x + size, y + size, c, 1.0f, 1.0f);

    push_text_vertex(x, y, c, 0.0f, 0.0f);
    push_text_vertex(x + size, y + size, c, 1.0f, 1.0f);
    push_text_vertex(x, y + size, c, 0.0f, 1.0f);
}

RHITextureHandle VulkanRenderer2D::create_texture_from_data(const assets::TextureData* data) {
    if (!ctx_ || !data || data->empty()) return RHITextureHandle{};

    RHITextureHandle handle = ctx_->create_texture();
    if (!handle.is_valid()) return RHITextureHandle{};

    ITexture* tex = ctx_->texture(handle);
    auto* vk_tex = dynamic_cast<VulkanTexture*>(tex);
    if (!vk_tex || !vk_tex->upload_data(data->pixels.data(), data->width, data->height, data->channels)) {
        ctx_->destroy_texture(handle);
        return RHITextureHandle{};
    }
    return handle;
}

ITexture* VulkanRenderer2D::resolve_texture(RHITextureHandle handle) const {
    return ctx_ ? ctx_->texture(handle) : nullptr;
}

} // namespace gryce_engine::render
