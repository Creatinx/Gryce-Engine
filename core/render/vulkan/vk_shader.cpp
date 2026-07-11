#include "vk_shader.h"

#include "render/mesh.h"
#include "render/texture.h"
#include "vk_buffer.h"
#include "vk_device.h"
#include "vk_swapchain.h"
#include "vk_texture.h"
#include "vk_framebuffer.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

#include <cstring>
#include <fstream>
#include <vector>

namespace gryce_engine::render {

VulkanShader::VulkanShader(VulkanDevice* device, VulkanSwapchain* swapchain)
    : device_(device), swapchain_(swapchain) {}

VulkanShader::~VulkanShader() {
    if (!device_ || !device_->is_valid()) return;
    VkDevice dev = device_->device();
    if (pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(dev, descriptor_pool_, nullptr);
    if (descriptor_set_layout_) vkDestroyDescriptorSetLayout(dev, descriptor_set_layout_, nullptr);
    if (vert_module_) vkDestroyShaderModule(dev, vert_module_, nullptr);
    if (frag_module_) vkDestroyShaderModule(dev, frag_module_, nullptr);
}

bool VulkanShader::load_spirv_from_file(const std::string& path, std::vector<uint32_t>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        GLOG_ERROR("VulkanShader: failed to open SPIR-V file '{}'", path);
        return false;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size % 4 != 0) {
        GLOG_ERROR("VulkanShader: SPIR-V file size not aligned to 4 bytes");
        return false;
    }
    out.resize(static_cast<size_t>(size) / 4);
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        GLOG_ERROR("VulkanShader: failed to read SPIR-V file");
        return false;
    }
    return true;
}

VkShaderModule VulkanShader::create_shader_module(const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * 4;
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device_->device(), &info, nullptr, &module);
    return module;
}

bool VulkanShader::load_spirv_files(const std::string& vert_path,
                                    const std::string& frag_path) {
    std::vector<uint32_t> vert_code, frag_code;
    if (!load_spirv_from_file(vert_path, vert_code) ||
        !load_spirv_from_file(frag_path, frag_code)) {
        return false;
    }
    vert_module_ = create_shader_module(vert_code);
    frag_module_ = create_shader_module(frag_code);
    if (!vert_module_ || !frag_module_) {
        GLOG_ERROR("VulkanShader: failed to create shader modules");
        return false;
    }
    return true;
}

bool VulkanShader::compile(const std::string& vertex_src, const std::string& fragment_src) {
    // Vulkan 不编译 GLSL 源码；由 RenderPipeline 显式调用 load_spirv_files + create_pipeline
    (void)vertex_src;
    (void)fragment_src;
    return true;
}

bool VulkanShader::compile(const std::vector<ShaderStageDesc>& stages) {
    for (const auto& stage : stages) {
        if (stage.stage == ShaderStage::Vertex) {
            return compile(stage.source, "");
        }
    }
    return false;
}

bool VulkanShader::load_program(const std::string& name,
                                const std::string& shader_dir,
                                IFramebuffer* target,
                                bool color_output,
                                bool post_process) {
    std::string dir = resources::ResourcePath::resolve(shader_dir);
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
        dir += '/';
    }

    std::string spirv_dir = dir + "spirv/";
    std::string vert_path = spirv_dir + "vulkan_" + name + ".vert.spv";
    std::string frag_path = spirv_dir + "vulkan_" + name + ".frag.spv";

    if (!load_spirv_files(vert_path, frag_path)) {
        GLOG_ERROR("VulkanShader::load_program: failed to load SPIR-V for '{}'", name);
        return false;
    }

    if (target) {
        auto* vk_target = dynamic_cast<VulkanFramebuffer*>(target);
        if (vk_target) {
            set_render_pass(vk_target->render_pass());
        }
    }

    set_color_output_enabled(color_output);
    set_post_process(post_process);
    return create_pipeline();
}

bool VulkanShader::create_pipeline() {
    VkRenderPass render_pass = render_pass_ ? render_pass_ : swapchain_->render_pass();
    GLOG_INFO("VulkanShader::create_pipeline render_pass={} color_output={} post_process={}",
              reinterpret_cast<void*>(render_pass), color_output_enabled_, post_process_);

    if (post_process_) {
        // Post-process descriptor layout: single combined image sampler at binding 0
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorBindingFlags binding_flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{};
        binding_flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        binding_flags_info.bindingCount = 1;
        binding_flags_info.pBindingFlags = &binding_flags;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout_info.bindingCount = 1;
        layout_info.pBindings = &binding;
        layout_info.pNext = &binding_flags_info;
        vkCreateDescriptorSetLayout(device_->device(), &layout_info, nullptr, &descriptor_set_layout_);

        // Push constants: exposure (float) + mode (int)
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        push_range.offset = 0;
        push_range.size = 8;

        VkPipelineLayoutCreateInfo pl_info{};
        pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl_info.setLayoutCount = 1;
        pl_info.pSetLayouts = &descriptor_set_layout_;
        pl_info.pushConstantRangeCount = 1;
        pl_info.pPushConstantRanges = &push_range;

        if (vkCreatePipelineLayout(device_->device(), &pl_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanShader: failed to create post-process pipeline layout");
            return false;
        }

        int frames = swapchain_ ? swapchain_->frames_in_flight() : 1;

        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = static_cast<uint32_t>(frames);

        VkDescriptorPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        pool_info.maxSets = frames;

        if (vkCreateDescriptorPool(device_->device(), &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanShader: failed to create post-process descriptor pool");
            return false;
        }

        descriptor_sets_.resize(frames, VK_NULL_HANDLE);
        std::vector<VkDescriptorSetLayout> layouts(frames, descriptor_set_layout_);
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = descriptor_pool_;
        alloc.descriptorSetCount = static_cast<uint32_t>(frames);
        alloc.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device_->device(), &alloc, descriptor_sets_.data()) != VK_SUCCESS) {
            GLOG_ERROR("VulkanShader: failed to allocate post-process descriptor sets");
            return false;
        }
    } else {
        // 描述符布局：UBO + 5 PBR 贴图 + shadow map
        VkDescriptorSetLayoutBinding bindings[7]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        for (uint32_t i = 0; i < 6; ++i) {
            bindings[i + 1].binding = i + 1;
            bindings[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i + 1].descriptorCount = 1;
            bindings[i + 1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorBindingFlags binding_flags[7]{};
        for (int i = 1; i < 7; ++i) {
            binding_flags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        }

        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{};
        binding_flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        binding_flags_info.bindingCount = 7;
        binding_flags_info.pBindingFlags = binding_flags;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout_info.bindingCount = 7;
        layout_info.pBindings = bindings;
        layout_info.pNext = &binding_flags_info;
        vkCreateDescriptorSetLayout(device_->device(), &layout_info, nullptr, &descriptor_set_layout_);

        VkPipelineLayoutCreateInfo pl_info{};
        pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl_info.setLayoutCount = 1;
        pl_info.pSetLayouts = &descriptor_set_layout_;

        // Push constants：4 个 mat4（model / view / projection / light_space）
        VkPushConstantRange push_range{};
        push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(math::Matrix4f) * 4;
        pl_info.pushConstantRangeCount = 1;
        pl_info.pPushConstantRanges = &push_range;

        if (vkCreatePipelineLayout(device_->device(), &pl_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
            GLOG_ERROR("VulkanShader: failed to create pipeline layout");
            return false;
        }

        if (!create_ubo() || !create_descriptor_pool()) {
            return false;
        }
    }

    // shader stages
    VkPipelineShaderStageCreateInfo vert_stage{};
    vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vert_stage.module = vert_module_;
    vert_stage.pName = "main";

    VkPipelineShaderStageCreateInfo frag_stage{};
    frag_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    frag_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    frag_stage.module = frag_module_;
    frag_stage.pName = "main";

    VkPipelineShaderStageCreateInfo stages[] = {vert_stage, frag_stage};

    // vertex input
    std::vector<VkVertexInputAttributeDescription> attrs;
    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.binding = 0;
    vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    if (post_process_) {
        vertex_binding.stride = 16; // vec2 pos + vec2 uv
        attrs.push_back({0, 0, VK_FORMAT_R32G32_SFLOAT, 0});   // position
        attrs.push_back({1, 0, VK_FORMAT_R32G32_SFLOAT, 8});   // uv
    } else {
        vertex_binding.stride = 56; // MeshVertex
        attrs.push_back({0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0});   // position
        attrs.push_back({1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12});  // normal
        attrs.push_back({2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24});  // tangent
        attrs.push_back({3, 0, VK_FORMAT_R32G32_SFLOAT, 36});     // uv
        attrs.push_back({4, 0, VK_FORMAT_R32G32B32_SFLOAT, 44});  // color
    }

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vertex_input.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // 动态 viewport / scissor，由 backend 每帧设置
    const bool dynamic_cdb = device_->supports_extended_dynamic_state();

    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    VkDynamicState dynamics[6] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    uint32_t dynamic_count = 2;
    if (dynamic_cdb && !post_process_) {
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_CULL_MODE_EXT;
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_FRONT_FACE_EXT;
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT;
        dynamics[dynamic_count++] = VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT;
    }
    dynamic_state.dynamicStateCount = dynamic_count;
    dynamic_state.pDynamicStates = dynamics;

    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = post_process_ ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    // Negative viewport height restores OpenGL's Y convention, so keep the same
    // winding convention as OpenGL: counter-clockwise front face with back culling.
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = post_process_ ? VK_FALSE : VK_TRUE;
    depth_stencil.depthWriteEnable = post_process_ ? VK_FALSE : VK_TRUE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attach{};
    blend_attach.blendEnable = (dynamic_cdb && !post_process_) ? VK_TRUE : VK_FALSE;
    blend_attach.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attach.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attach.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attach.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attach.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_attach.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attach;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = pipeline_layout_;
    pipeline_info.renderPass = render_pass;
    pipeline_info.subpass = 0;

    // Shadow pass 无 color attachment，关闭 color blend
    if (!color_output_enabled_) {
        pipeline_info.pColorBlendState = nullptr;
    }

    if (vkCreateGraphicsPipelines(device_->device(), VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                  &pipeline_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanShader: failed to create graphics pipeline");
        return false;
    }
    return true;
}

void VulkanShader::bind() const {}
void VulkanShader::unbind() const {}

void VulkanShader::set_int(const std::string& name, int value) {
    if (name == "uUseAlbedoMap") ubo_data_.use_albedo_map = value;
    else if (name == "uUseNormalMap") ubo_data_.use_normal_map = value;
    else if (name == "uUseRoughnessMap") ubo_data_.use_roughness_map = value;
    else if (name == "uUseMetallicMap") ubo_data_.use_metallic_map = value;
    else if (name == "uUseAOMap") ubo_data_.use_ao_map = value;
    else if (name == "uUseShadowMap") ubo_data_.use_shadow_map = value;
    ubo_dirty_ = true;
}
void VulkanShader::set_int(const char* name, int value) { set_int(std::string(name), value); }

void VulkanShader::set_float(const std::string& name, float value) {
    if (name == "uRoughness") ubo_data_.roughness = value;
    else if (name == "uMetallic") ubo_data_.metallic = value;
    else if (name == "uAO") ubo_data_.ao = value;
    else if (name == "uShadowBias") ubo_data_.shadow_bias = value;
    else if (name == "uLightIntensity") ubo_data_.light_intensity = value;
    ubo_dirty_ = true;
}
void VulkanShader::set_float(const char* name, float value) { set_float(std::string(name), value); }

void VulkanShader::set_vec2(const std::string& /*name*/, const math::Vector2f& /*value*/) {}
void VulkanShader::set_vec2(const char* /*name*/, const math::Vector2f& /*value*/) {}

void VulkanShader::set_vec3(const std::string& name, const math::Vector3f& value) {
    auto to_vec4 = [](const math::Vector3f& v) { return math::Vector4f(v.x, v.y, v.z, 0.0f); };
    if (name == "uAlbedoColor") ubo_data_.albedo_color = to_vec4(value);
    else if (name == "uCameraPos") ubo_data_.camera_pos = to_vec4(value);
    else if (name == "uLightDir") ubo_data_.light_dir = to_vec4(value);
    else if (name == "uLightColor") ubo_data_.light_color = to_vec4(value);
    ubo_dirty_ = true;
}
void VulkanShader::set_vec3(const char* name, const math::Vector3f& value) { set_vec3(std::string(name), value); }

void VulkanShader::set_vec4(const std::string& /*name*/, const math::Vector4f& /*value*/) {}
void VulkanShader::set_vec4(const char* /*name*/, const math::Vector4f& /*value*/) {}

void VulkanShader::set_mat4(const std::string& name, const math::Matrix4f& value) {
    if (name == "uModel") model_ = value;
    else if (name == "uView") view_ = value;
    else if (name == "uProjection") {
        // OpenGL projection matrices use Z in [-1, 1]; Vulkan NDC uses [0, 1].
        // Remap the Z row while keeping Y unchanged; Y is flipped via negative viewport.
        math::Matrix4f vk_proj = value;
        vk_proj(2, 2) = value(2, 2) * 0.5f + value(3, 2) * 0.5f;
        vk_proj(2, 3) = value(2, 3) * 0.5f + value(3, 3) * 0.5f;
        projection_ = vk_proj;
    }
    else if (name == "uLightSpaceMatrix") light_space_matrix_ = value;
    push_dirty_ = true;
}
void VulkanShader::set_mat4(const char* name, const math::Matrix4f& value) { set_mat4(std::string(name), value); }

bool VulkanShader::create_ubo() {
    int frames = swapchain_ ? swapchain_->frames_in_flight() : 1;
    ubo_buffers_.clear();
    ubo_buffers_.reserve(frames);
    for (int i = 0; i < frames; ++i) {
        auto buffer = std::make_unique<VulkanBuffer>();
        if (!buffer->init(device_, ubo_size_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            GLOG_ERROR("VulkanShader: failed to create UBO for frame {}", i);
            return false;
        }
        ubo_buffers_.push_back(std::move(buffer));
    }
    return true;
}

bool VulkanShader::create_descriptor_pool() {
    int frames = swapchain_ ? swapchain_->frames_in_flight() : 1;

    VkDescriptorPoolSize pool_sizes[2]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[0].descriptorCount = static_cast<uint32_t>(frames);
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[1].descriptorCount = static_cast<uint32_t>(frames * 6);

    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    info.poolSizeCount = 2;
    info.pPoolSizes = pool_sizes;
    info.maxSets = frames;

    if (vkCreateDescriptorPool(device_->device(), &info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        GLOG_ERROR("VulkanShader: failed to create descriptor pool");
        return false;
    }

    descriptor_sets_.resize(frames, VK_NULL_HANDLE);
    std::vector<VkDescriptorSetLayout> layouts(frames, descriptor_set_layout_);
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = descriptor_pool_;
    alloc.descriptorSetCount = static_cast<uint32_t>(frames);
    alloc.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_->device(), &alloc, descriptor_sets_.data()) != VK_SUCCESS) {
        GLOG_ERROR("VulkanShader: failed to allocate descriptor sets");
        return false;
    }

    // 为每个 frame 绑定对应的 UBO
    for (int i = 0; i < frames; ++i) {
        VkDescriptorBufferInfo buffer_info{};
        buffer_info.buffer = ubo_buffers_[i]->buffer();
        buffer_info.offset = 0;
        buffer_info.range = ubo_size_;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptor_sets_[i];
        write.dstBinding = 0;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
    }
    return true;
}

void VulkanShader::set_texture(int slot, ITexture* texture) {
    // post-process: binding 0 starts at slot 0
    // PBR: slot 0~4 对应 material 贴图，binding 1~5; slot 5 对应 shadow map，binding 6
    int binding = post_process_ ? slot : (slot + 1);
    if (binding < 0 || binding > 6 || !texture) return;
    auto* vk_tex = dynamic_cast<VulkanTexture*>(texture);
    if (!vk_tex || !vk_tex->image_view() || !vk_tex->sampler()) return;

    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = vk_tex->is_depth()
                                 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = vk_tex->image_view();
    image_info.sampler = vk_tex->sampler();

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_sets_[current_frame()];
    write.dstBinding = static_cast<uint32_t>(binding);
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;

    vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
}

bool VulkanShader::is_valid() const {
    return pipeline_ != VK_NULL_HANDLE;
}

void VulkanShader::update_ubo(VkCommandBuffer /*cmd*/) const {
    if (!ubo_dirty_) return;
    int frame = current_frame();
    if (frame < 0 || frame >= static_cast<int>(ubo_buffers_.size())) return;
    ubo_buffers_[frame]->upload(&ubo_data_, sizeof(UBOData));
    ubo_dirty_ = false;
}

void VulkanShader::push_constants(VkCommandBuffer cmd) const {
    if (!push_dirty_) return;
    if (post_process_) {
        struct PushData {
            float exposure;
            int mode;
        } data{pp_exposure_, pp_mode_};
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(data), &data);
    } else {
        float matrices[4 * 16];
        for (int i = 0; i < 16; ++i) {
            matrices[i] = model_.m[i];
            matrices[16 + i] = view_.m[i];
            matrices[32 + i] = projection_.m[i];
            matrices[48 + i] = light_space_matrix_.m[i];
        }
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(matrices), matrices);
    }
    push_dirty_ = false;
}

void VulkanShader::push_post_process_constants(VkCommandBuffer cmd, float exposure, int mode) const {
    if (!pipeline_layout_) return;
    struct PushData {
        float exposure;
        int mode;
    } data{exposure, mode};
    vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(data), &data);
}

int VulkanShader::current_frame() const {
    return swapchain_ ? swapchain_->current_frame_index() : 0;
}

VkDescriptorSet VulkanShader::descriptor_set() const {
    int frame = current_frame();
    if (frame < 0 || frame >= static_cast<int>(descriptor_sets_.size())) return VK_NULL_HANDLE;
    return descriptor_sets_[frame];
}

void VulkanShader::bind_descriptor_set(VkCommandBuffer cmd) const {
    VkDescriptorSet set = descriptor_set();
    if (set != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                0, 1, &set, 0, nullptr);
    }
}

} // namespace gryce_engine::render
