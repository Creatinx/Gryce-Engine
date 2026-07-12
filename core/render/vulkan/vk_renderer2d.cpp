#include "vk_renderer2d.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "render/render_context.h"
#include "render/vulkan/vk_backend.h"
#include "render/vulkan/vk_device.h"
#include "render/vulkan/vk_swapchain.h"
#include "render/vulkan/vk_texture.h"
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

// 受光照 sprite 的 push constants，必须与 vulkan_2d_lit.vert/frag 的 std430 布局一致。
// 注意：GLSL vec3 是 16 字节对齐的，而 C++ math::Vector3f 是 4 字节对齐，因此这里全部使用
// float 数组 + 显式 padding，避免跨编译器/平台的布局差异。
struct alignas(16) LitPushConstants {
    math::Matrix4f ortho;        // 0-63
    math::Vector2f screen_size;  // 64-71
    float ambient[3];            // 72-83
    float pad0;                  // 84-87
    math::Vector2f light_pos;    // 88-95
    float light_radius;          // 96-99
    float light_intensity;       // 100-103
    float light_color[3];        // 104-115
    float pad1;                  // 116-119
};
static_assert(sizeof(LitPushConstants) == 120, "LitPushConstants size mismatch");

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
        !create_descriptor_layout() ||
        !create_pipeline_layout()) {
        shutdown();
        return;
    }

    pipeline_rect_ = create_pipeline(vert_module_, frag_rect_module_, pipeline_layout_);
    pipeline_text_ = create_pipeline(vert_module_, frag_text_module_, pipeline_layout_);
    pipeline_sprite_ = create_pipeline(vert_module_, frag_sprite_module_, pipeline_layout_);
    if (vert_lit_module_ != VK_NULL_HANDLE && frag_lit_sprite_module_ != VK_NULL_HANDLE) {
        pipeline_lit_sprite_ = create_pipeline(vert_lit_module_, frag_lit_sprite_module_, lit_pipeline_layout_);
    }
    if (pipeline_rect_ == VK_NULL_HANDLE || pipeline_text_ == VK_NULL_HANDLE ||
        pipeline_sprite_ == VK_NULL_HANDLE ||
        (frag_lit_sprite_module_ != VK_NULL_HANDLE && pipeline_lit_sprite_ == VK_NULL_HANDLE) ||
        !create_descriptor_sets() ||
        !create_vertex_buffer()) {
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

    // FontAtlas 上传的是 bottom-up 位图；Vulkan 把 row 0 当作图像顶部，
    // 因此纹理在 Vulkan 坐标系中是上下翻转的。FontAtlas 生成的 OpenGL 风格 UV
    // (uv0_y = 1 - y0/atlas) 正好把 quad 顶边映射到纹理中 glyph 的顶部（v=1），
    // 所以 Vulkan 端不需要再 flip UV。
    if (font_atlas_.texture()) {
        font_atlas_.texture()->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    }

    // 绑定字体图集到所有 text descriptor set
    if (font_atlas_.texture()) {
        auto* vk_tex = dynamic_cast<VulkanTexture*>(font_atlas_.texture());
        if (vk_tex && vk_tex->image_view() && vk_tex->sampler()) {
            VkDescriptorImageInfo image_info{};
            image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            image_info.imageView = vk_tex->image_view();
            image_info.sampler = vk_tex->sampler();

            std::vector<VkWriteDescriptorSet> writes;
            writes.reserve(text_descriptor_sets_.size());
            for (VkDescriptorSet set : text_descriptor_sets_) {
                VkWriteDescriptorSet write{};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = set;
                write.dstBinding = 0;
                write.dstArrayElement = 0;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.descriptorCount = 1;
                write.pImageInfo = &image_info;
                writes.push_back(write);
            }
            if (!writes.empty()) {
                vkUpdateDescriptorSets(vk_device_->device(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
            }
        } else {
            GLOG_ERROR("VulkanRenderer2D: font texture invalid, text will be blocks");
        }
    } else {
        GLOG_ERROR("VulkanRenderer2D: no font texture, text rendering disabled");
    }

    vertices_.reserve(4096);
    initialized_ = true;
    GLOG_INFO("VulkanRenderer2D initialized");
}

void VulkanRenderer2D::shutdown() {
    if (!initialized_) return;

    VkDevice dev = vk_device_ ? vk_device_->device() : VK_NULL_HANDLE;
    if (dev != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(dev);
        if (pipeline_rect_) vkDestroyPipeline(dev, pipeline_rect_, nullptr);
        if (pipeline_text_) vkDestroyPipeline(dev, pipeline_text_, nullptr);
        if (pipeline_sprite_) vkDestroyPipeline(dev, pipeline_sprite_, nullptr);
        if (pipeline_lit_sprite_) vkDestroyPipeline(dev, pipeline_lit_sprite_, nullptr);
        if (pipeline_layout_) vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr);
        if (lit_pipeline_layout_) vkDestroyPipelineLayout(dev, lit_pipeline_layout_, nullptr);
        if (descriptor_pool_) vkDestroyDescriptorPool(dev, descriptor_pool_, nullptr);
        if (descriptor_layout_) vkDestroyDescriptorSetLayout(dev, descriptor_layout_, nullptr);
        if (vert_module_) vkDestroyShaderModule(dev, vert_module_, nullptr);
        if (frag_rect_module_) vkDestroyShaderModule(dev, frag_rect_module_, nullptr);
        if (frag_text_module_) vkDestroyShaderModule(dev, frag_text_module_, nullptr);
        if (frag_sprite_module_) vkDestroyShaderModule(dev, frag_sprite_module_, nullptr);
        if (frag_lit_sprite_module_) vkDestroyShaderModule(dev, frag_lit_sprite_module_, nullptr);
    }

    for (size_t i = 0; i < vertex_buffer_rect_.size(); ++i) {
        vertex_buffer_rect_[i].shutdown();
    }
    for (size_t i = 0; i < vertex_buffer_text_.size(); ++i) {
        vertex_buffer_text_[i].shutdown();
    }
    for (size_t i = 0; i < vertex_buffer_sprite_.size(); ++i) {
        vertex_buffer_sprite_[i].shutdown();
    }
    for (size_t i = 0; i < vertex_buffer_lit_sprite_.size(); ++i) {
        vertex_buffer_lit_sprite_[i].shutdown();
    }

    if (context_alive() && font_atlas_.texture()) {
        font_atlas_.destroy(ctx_);
    }

    pipeline_rect_ = VK_NULL_HANDLE;
    pipeline_text_ = VK_NULL_HANDLE;
    pipeline_sprite_ = VK_NULL_HANDLE;
    pipeline_lit_sprite_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    lit_pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_layout_ = VK_NULL_HANDLE;
    text_descriptor_sets_.clear();
    sprite_descriptor_sets_.clear();
    vert_module_ = VK_NULL_HANDLE;
    frag_rect_module_ = VK_NULL_HANDLE;
    frag_text_module_ = VK_NULL_HANDLE;
    frag_sprite_module_ = VK_NULL_HANDLE;
    frag_lit_sprite_module_ = VK_NULL_HANDLE;

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

    std::vector<uint32_t> vert_code, vert_lit_code, frag_rect_code, frag_text_code, frag_sprite_code, frag_lit_code;
    if (!load_spirv_file(resolved + "vulkan_2d.vert.spv", vert_code) ||
        !load_spirv_file(resolved + "vulkan_2d_rect.frag.spv", frag_rect_code) ||
        !load_spirv_file(resolved + "vulkan_2d_text.frag.spv", frag_text_code) ||
        !load_spirv_file(resolved + "vulkan_2d_sprite.frag.spv", frag_sprite_code)) {
        return false;
    }
    // lit sprite shader 可选：文件缺失时 2D 光照不可用
    bool has_lit = load_spirv_file(resolved + "vulkan_2d_lit.vert.spv", vert_lit_code) &&
                   load_spirv_file(resolved + "vulkan_2d_lit.frag.spv", frag_lit_code);
    if (!has_lit) {
        GLOG_WARN("VulkanRenderer2D: lit sprite SPIR-V not found, 2D lighting disabled in Vulkan");
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
    if (!vert_module_ || !frag_rect_module_ || !frag_text_module_ || !frag_sprite_module_) {
        GLOG_ERROR("VulkanRenderer2D: failed to create shader modules");
        return false;
    }
    return true;
}

bool VulkanRenderer2D::create_descriptor_layout() {
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
    return true;
}

bool VulkanRenderer2D::create_pipeline_layout() {
    // 普通 2D pipeline layout：只有 ortho 矩阵
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

    // 受光照 sprite pipeline layout：ortho + screen_size + ambient + 1 point light
    {
        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        range.offset = 0;
        range.size = sizeof(LitPushConstants);

        VkPipelineLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        info.setLayoutCount = 1;
        info.pSetLayouts = &descriptor_layout_;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &range;

        if (vkCreatePipelineLayout(vk_device_->device(), &info, nullptr, &lit_pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanRenderer2D: failed to create lit pipeline layout");
            return false;
        }
    }
    return true;
}

VkPipeline VulkanRenderer2D::create_pipeline(VkShaderModule frag_module, VkPipelineLayout layout) {
    VkDevice dev = vk_device_->device();

    VkPipelineShaderStageCreateInfo vert_stage{};
    vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage.module = vert_module_;
    vert_stage.pName = "main";

    VkPipelineShaderStageCreateInfo frag_stage{};
    frag_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    frag_stage.module = frag_module;
    frag_stage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vert_stage, frag_stage};

    // Vertex2D: vec2 pos + vec4 color + vec2 uv = 32 bytes
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = sizeof(Vertex2D);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[1].offset = 2 * sizeof(float);
    attrs[2].location = 2;
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset = 6 * sizeof(float);

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
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
    depth_stencil.depthTestEnable = VK_FALSE;
    depth_stencil.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend_attach{};
    blend_attach.blendEnable = VK_TRUE;
    // shader 输出非预乘 alpha，标准 SRC_ALPHA/ONE_MINUS_SRC_ALPHA 混合
    blend_attach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attach.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attach.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attach.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attach;

    // 2D pipeline：viewport/scissor 始终动态；若设备支持 extended dynamic state，
    // 也把 cull/depth/front_face 设为动态，防止 3D 管线留下的动态状态影响 2D。
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
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic_state;
    info.layout = layout;
    info.renderPass = vk_swapchain_->render_pass();
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        GLOG_ERROR("VulkanRenderer2D: failed to create graphics pipeline");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

bool VulkanRenderer2D::create_descriptor_sets() {
    const int frames = vk_swapchain_ ? vk_swapchain_->frames_in_flight() : 2;

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = static_cast<uint32_t>(frames * 2);

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    pool_info.maxSets = static_cast<uint32_t>(frames * 2);

    VkDevice dev = vk_device_->device();
    if (vkCreateDescriptorPool(dev, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanRenderer2D: failed to create descriptor pool");
        return false;
    }

    std::vector<VkDescriptorSetLayout> layouts(frames * 2, descriptor_layout_);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = descriptor_pool_;
    alloc.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    alloc.pSetLayouts = layouts.data();

    std::vector<VkDescriptorSet> sets(layouts.size(), VK_NULL_HANDLE);
    if (vkAllocateDescriptorSets(dev, &alloc, sets.data()) != VK_SUCCESS) {
        GLOG_ERROR("VulkanRenderer2D: failed to allocate descriptor sets");
        return false;
    }

    text_descriptor_sets_.assign(sets.begin(), sets.begin() + frames);
    sprite_descriptor_sets_.assign(sets.begin() + frames, sets.end());
    return true;
}

bool VulkanRenderer2D::create_vertex_buffer() {
    const int frames = vk_swapchain_ ? vk_swapchain_->frames_in_flight() : 2;
    const VkDeviceSize capacity = 4096 * sizeof(Vertex2D);

    vertex_buffer_rect_.resize(frames);
    vertex_buffer_text_.resize(frames);
    vertex_buffer_sprite_.resize(frames);
    vertex_buffer_lit_sprite_.resize(frames);
    vertex_buffer_rect_capacity_.assign(frames, capacity);
    vertex_buffer_text_capacity_.assign(frames, capacity);
    vertex_buffer_sprite_capacity_.assign(frames, capacity);
    vertex_buffer_lit_sprite_capacity_.assign(frames, capacity);

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
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            return false;
        }
    }
    return true;
}

void VulkanRenderer2D::begin_frame(float screen_width, float screen_height) {
    vertices_.clear();
    text_vertices_.clear();
    sprite_vertices_.clear();
    sprite_texture_ = nullptr;
    lit_sprite_vertices_.clear();
    lit_sprite_texture_ = nullptr;
    reset_lights();
    screen_width_ = screen_width;
    screen_height_ = screen_height;
    ortho_ = math::Matrix4f::ortho(0.0f, screen_width, 0.0f, screen_height, -1.0f, 1.0f);
}

void VulkanRenderer2D::end_frame() {
    flush_sprite_batch();
    flush_lit_sprite_batch();
    flush_batches();
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

void VulkanRenderer2D::flush_batches() {
    flush_batch(std::move(vertices_), false, nullptr, pipeline_rect_, pipeline_layout_, false);
    flush_batch(std::move(text_vertices_), true, font_atlas_.texture(), pipeline_text_, pipeline_layout_, false);
}

void VulkanRenderer2D::flush_sprite_batch() {
    if (sprite_vertices_.empty() || !sprite_texture_) return;
    flush_batch(std::move(sprite_vertices_), false, sprite_texture_, pipeline_sprite_, pipeline_layout_, false);
    sprite_texture_ = nullptr;
}

void VulkanRenderer2D::flush_batch(std::vector<Vertex2D>&& verts, bool is_text, ITexture* texture,
                                    VkPipeline pipeline, VkPipelineLayout layout, bool is_lit) {
    const bool is_sprite = (!is_text && texture != nullptr && !is_lit);

    if (verts.empty() || !context_alive() || !vk_backend_ || pipeline == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) {
        return;
    }

    auto verts_shared = std::make_shared<std::vector<Vertex2D>>(std::move(verts));
    math::Matrix4f ortho = ortho_;
    ITexture* tex = texture;
    float screen_w = screen_width_;
    float screen_h = screen_height_;
    Color ambient = ambient_light_;
    PointLight light = point_light_;
    bool has_light = has_point_light_;

    ctx_->push_command([this, verts_shared, ortho, is_text, is_sprite, tex, pipeline, layout, is_lit,
                        screen_w, screen_h, ambient, light, has_light](IRenderBackend*) {
        int frame_index = vk_swapchain_->current_frame_index();
        if (frame_index < 0 || frame_index >= static_cast<int>(vertex_buffer_rect_.size())) {
            GLOG_ERROR("VulkanRenderer2D::flush_batch: frame_index {} out of range {}",
                       frame_index, vertex_buffer_rect_.size());
            return;
        }

        VulkanBuffer* vertex_buffer = nullptr;
        VkDeviceSize* capacity = nullptr;
        if (is_text) {
            vertex_buffer = &vertex_buffer_text_[frame_index];
            capacity = &vertex_buffer_text_capacity_[frame_index];
        } else if (is_lit) {
            vertex_buffer = &vertex_buffer_lit_sprite_[frame_index];
            capacity = &vertex_buffer_lit_sprite_capacity_[frame_index];
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
        viewport.y = 0.0f;
        viewport.width = screen_w;
        viewport.height = screen_h;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vk_backend_->set_viewport_cached(cmd, viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = {static_cast<uint32_t>(screen_w), static_cast<uint32_t>(screen_h)};
        vk_backend_->set_scissor_cached(cmd, scissor);

        vk_backend_->bind_pipeline(cmd, pipeline);
        vk_backend_->set_dynamic_state_2d(cmd);

        if (is_lit) {
            LitPushConstants pc{};
            pc.ortho = ortho;
            pc.screen_size = math::Vector2f(screen_w, screen_h);
            pc.ambient[0] = ambient.r;
            pc.ambient[1] = ambient.g;
            pc.ambient[2] = ambient.b;
            pc.light_pos = has_light ? light.pos : math::Vector2f::zero();
            pc.light_radius = has_light ? light.radius : 0.0f;
            pc.light_intensity = has_light ? light.intensity : 0.0f;
            pc.light_color[0] = has_light ? light.color.r : 0.0f;
            pc.light_color[1] = has_light ? light.color.g : 0.0f;
            pc.light_color[2] = has_light ? light.color.b : 0.0f;
            vkCmdPushConstants(cmd, layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(LitPushConstants), &pc);
        } else {
            struct alignas(16) PushConstants {
                math::Matrix4f ortho;
            } pc;
            pc.ortho = ortho;
            vkCmdPushConstants(cmd, layout,
                               VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(PushConstants), &pc);
        }

        VkDescriptorSet set = VK_NULL_HANDLE;
        if (is_text) {
            set = text_descriptor_sets_[frame_index];
        } else if (is_sprite || is_lit) {
            set = sprite_descriptor_sets_[frame_index];
            auto* vk_tex = dynamic_cast<VulkanTexture*>(tex);
            if (vk_tex && vk_tex->image_view() && vk_tex->sampler()) {
                VkDescriptorImageInfo image_info{};
                image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                image_info.imageView = vk_tex->image_view();
                image_info.sampler = vk_tex->sampler();

                VkWriteDescriptorSet write{};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = set;
                write.dstBinding = 0;
                write.dstArrayElement = 0;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.descriptorCount = 1;
                write.pImageInfo = &image_info;
                vkUpdateDescriptorSets(vk_device_->device(), 1, &write, 0, nullptr);
            }
        }

        if (set != VK_NULL_HANDLE) {
            vk_backend_->bind_descriptor_set(cmd, layout, set);
        }

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

void VulkanRenderer2D::add_point_light(const math::Vector2f& pos, float radius,
                                        const Color& color, float intensity) {
    // 当前 Vulkan forward lighting 只支持一个点光源；保留最强/最近的一个
    point_light_ = {pos, radius, color, intensity};
    has_point_light_ = true;
}

void VulkanRenderer2D::reset_lights() {
    ambient_light_ = Color::black();
    has_point_light_ = false;
    point_light_ = {};
}

void VulkanRenderer2D::draw_lit_sprite(float x, float y, float w, float h,
                                        ITexture* albedo, ITexture* /*normal_map*/,
                                        const Color& tint) {
    draw_lit_sprite_region(x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, albedo, nullptr, tint);
}

void VulkanRenderer2D::draw_lit_sprite_region(float x, float y, float w, float h,
                                               float u0, float v0, float u1, float v1,
                                               ITexture* albedo, ITexture* /*normal_map*/,
                                               const Color& tint) {
    if (!albedo || !albedo->is_valid() || pipeline_lit_sprite_ == VK_NULL_HANDLE) {
        // 光照 pipeline 未就绪时退化为普通 sprite
        draw_sprite_region(x, y, w, h, u0, v0, u1, v1, albedo, tint);
        return;
    }

    if (albedo != lit_sprite_texture_) {
        flush_lit_sprite_batch();
        lit_sprite_texture_ = albedo;
    }

    float x0 = x, y0 = y;
    float x1 = x + w, y1 = y + h;

    lit_sprite_vertices_.push_back({x0, y0, tint.r, tint.g, tint.b, tint.a, u0, v0});
    lit_sprite_vertices_.push_back({x1, y0, tint.r, tint.g, tint.b, tint.a, u1, v0});
    lit_sprite_vertices_.push_back({x1, y1, tint.r, tint.g, tint.b, tint.a, u1, v1});

    lit_sprite_vertices_.push_back({x0, y0, tint.r, tint.g, tint.b, tint.a, u0, v0});
    lit_sprite_vertices_.push_back({x1, y1, tint.r, tint.g, tint.b, tint.a, u1, v1});
    lit_sprite_vertices_.push_back({x0, y1, tint.r, tint.g, tint.b, tint.a, u0, v1});
}

void VulkanRenderer2D::flush_lit_sprite_batch() {
    if (lit_sprite_vertices_.empty() || !lit_sprite_texture_) return;
    flush_batch(std::move(lit_sprite_vertices_), false, lit_sprite_texture_,
                pipeline_lit_sprite_, lit_pipeline_layout_, true);
    lit_sprite_texture_ = nullptr;
}

void VulkanRenderer2D::draw_text(float x, float y, const std::string& text, float font_size, const Color& color) {
    // 只有在完全没有图集时才走纯色方块兜底；fallback atlas 本身也包含字形 UV，应优先采样
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

ITexture* VulkanRenderer2D::resolve_texture(RHITextureHandle handle) const {
    return ctx_ ? ctx_->texture(handle) : nullptr;
}

} // namespace gryce_engine::render
