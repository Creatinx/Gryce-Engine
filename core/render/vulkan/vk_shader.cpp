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

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#include <filesystem>
#include <system_error>

namespace gryce_engine::render {

namespace {
// 将全局 texture slot 映射到 Vulkan PBR shader 的 descriptor binding。
// 必须与 vulkan_pbr.frag / vulkan_skinned_pbr.frag 中的 layout(binding=...) 一致。
int slot_to_binding(int slot) {
    switch (slot) {
        case TextureSlots::kPBRAlbedo:    return 1;
        case TextureSlots::kPBRNormal:    return 2;
        case TextureSlots::kPBRRoughness: return 3;
        case TextureSlots::kPBRMetallic:  return 4;
        case TextureSlots::kPBRAO:        return 5;
        case TextureSlots::kPBRShadow:    return 6;
        case TextureSlots::kPBREmissive:  return 7;
        case TextureSlots::kPBRShadowC1:  return 12;
        case TextureSlots::kPBRShadowC2:  return 13;
        case TextureSlots::kPBRShadowC3:  return 14;
        case TextureSlots::kPBRShadowDepth:  return 15;
        case TextureSlots::kPBRShadowDepth1: return 16;
        case TextureSlots::kPBRShadowDepth2: return 17;
        case TextureSlots::kPBRShadowDepth3: return 18;
        case TextureSlots::kPBRSSAO:     return 19;
        case TextureSlots::kIBLIrradiance: return 9;
        case TextureSlots::kIBLPrefilter:  return 10;
        case TextureSlots::kIBLBRDF:       return 11;
        default: return slot + 1;
    }
}

// 后处理/天空盒共用固定描述符集：
// 0 = 主输入（HDR/天空盒/当前帧），1 = bloom，2 = LUT，3 = 曝光值，4 = TAA 历史
int post_process_binding(int slot) {
    if (slot == TextureSlots::kTonemapBloom) return 1;
    if (slot == TextureSlots::kTonemapLUT) return 2;
    if (slot == TextureSlots::kTonemapExposure) return 3;
    if (slot == TextureSlots::kTAAHistory) return 4;
    return 0;
}
} // namespace

VulkanShader::VulkanShader(VulkanDevice* device, VulkanSwapchain* swapchain)
    : device_(device), swapchain_(swapchain) {}

VulkanShader::~VulkanShader() {
    if (!device_ || !device_->is_valid()) return;
    VkDevice dev = device_->device();
    if (pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr);
    if (descriptor_pool_) vkDestroyDescriptorPool(dev, descriptor_pool_, nullptr);
    for (auto pool : descriptor_pools_) {
        if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
    }
    descriptor_pools_.clear();
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
                                bool post_process,
                                bool skybox,
                                bool skinned) {
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

    // 记录 SPIR-V 文件信息供热重载使用
    source_name_ = name;
    spirv_dir_ = spirv_dir;
    std::error_code ec;
    vert_mtime_ = std::filesystem::last_write_time(vert_path, ec);
    frag_mtime_ = std::filesystem::last_write_time(frag_path, ec);

    if (target) {
        auto* vk_target = dynamic_cast<VulkanFramebuffer*>(target);
        if (vk_target) {
            set_render_pass(vk_target->render_pass());
        }
    }

    set_color_output_enabled(color_output);
    set_post_process(post_process);
    set_skybox(skybox);
    skinned_ = skinned;
    return create_pipeline();
}

bool VulkanShader::shader_files_changed() const {
    if (source_name_.empty() || spirv_dir_.empty()) return false;
    std::error_code ec;
    auto vert_mtime = std::filesystem::last_write_time(spirv_dir_ + "vulkan_" + source_name_ + ".vert.spv", ec);
    if (ec) return false;
    auto frag_mtime = std::filesystem::last_write_time(spirv_dir_ + "vulkan_" + source_name_ + ".frag.spv", ec);
    if (ec) return false;
    return vert_mtime != vert_mtime_ || frag_mtime != frag_mtime_;
}

bool VulkanShader::reload() {
    if (source_name_.empty() || spirv_dir_.empty() || !device_ || !device_->is_valid()) {
        return false;
    }
    VkDevice dev = device_->device();
    std::string vert_path = spirv_dir_ + "vulkan_" + source_name_ + ".vert.spv";
    std::string frag_path = spirv_dir_ + "vulkan_" + source_name_ + ".frag.spv";

    // 先备份旧资源；重建成功后统一销毁，失败则回退，保证加载过程不出问题。
    VkPipeline old_pipeline = pipeline_;
    VkPipelineLayout old_layout = pipeline_layout_;
    VkDescriptorPool old_pool = descriptor_pool_;
    VkDescriptorSetLayout old_set_layout = descriptor_set_layout_;
    VkShaderModule old_vert = vert_module_;
    VkShaderModule old_frag = frag_module_;
    auto old_fallback_texture = std::move(fallback_texture_);
    auto old_fallback_cube = std::move(fallback_cube_);

    pipeline_ = VK_NULL_HANDLE;
    pipeline_layout_ = VK_NULL_HANDLE;
    descriptor_pool_ = VK_NULL_HANDLE;
    descriptor_set_layout_ = VK_NULL_HANDLE;
    vert_module_ = VK_NULL_HANDLE;
    frag_module_ = VK_NULL_HANDLE;
    descriptor_sets_.clear();
    cached_textures_.clear();
    // create_ubo / create_descriptor_pool 内部会清空并重建这些容器，
    // 旧句柄在成功路径统一销毁。
    std::vector<VkDescriptorPool> old_per_frame_pools = descriptor_pools_;
    descriptor_pools_.clear();
    ubo_buffers_.clear();
    palette_buffers_.clear();

    if (!load_spirv_files(vert_path, frag_path) || !create_pipeline()) {
        // 回退：销毁刚创建的部分资源，恢复旧资源
        if (vert_module_) vkDestroyShaderModule(dev, vert_module_, nullptr);
        if (frag_module_) vkDestroyShaderModule(dev, frag_module_, nullptr);
        if (descriptor_set_layout_) vkDestroyDescriptorSetLayout(dev, descriptor_set_layout_, nullptr);
        if (pipeline_layout_) vkDestroyPipelineLayout(dev, pipeline_layout_, nullptr);
        if (descriptor_pool_) vkDestroyDescriptorPool(dev, descriptor_pool_, nullptr);
        for (auto pool : descriptor_pools_) {
            if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
        }
        for (auto pool : old_per_frame_pools) {
            if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
        }
        ubo_buffers_.clear();
        palette_buffers_.clear();

        pipeline_ = old_pipeline;
        pipeline_layout_ = old_layout;
        descriptor_pool_ = old_pool;
        descriptor_set_layout_ = old_set_layout;
        vert_module_ = old_vert;
        frag_module_ = old_frag;
        fallback_texture_ = std::move(old_fallback_texture);
        fallback_cube_ = std::move(old_fallback_cube);

        GLOG_ERROR("VulkanShader::reload: rebuild failed for '{}', keeping old pipeline", source_name_);
        return false;
    }

    // 成功：销毁旧资源
    if (old_pipeline) vkDestroyPipeline(dev, old_pipeline, nullptr);
    if (old_layout) vkDestroyPipelineLayout(dev, old_layout, nullptr);
    if (old_pool) vkDestroyDescriptorPool(dev, old_pool, nullptr);
    if (old_set_layout) vkDestroyDescriptorSetLayout(dev, old_set_layout, nullptr);
    if (old_vert) vkDestroyShaderModule(dev, old_vert, nullptr);
    if (old_frag) vkDestroyShaderModule(dev, old_frag, nullptr);
    for (auto pool : old_per_frame_pools) {
        if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
    }
    // fallback 贴图在 create_pipeline 中被重建，旧的自动释放
    old_fallback_texture.reset();
    old_fallback_cube.reset();

    std::error_code ec;
    vert_mtime_ = std::filesystem::last_write_time(vert_path, ec);
    frag_mtime_ = std::filesystem::last_write_time(frag_path, ec);

    GLOG_INFO("VulkanShader: hot-reloaded '{}'", source_name_);
    return true;
}

bool VulkanShader::create_pipeline() {
    VkRenderPass render_pass = render_pass_ ? render_pass_ : swapchain_->render_pass();
    GLOG_INFO("VulkanShader::create_pipeline render_pass={} color_output={} post_process={}",
              reinterpret_cast<void*>(render_pass), color_output_enabled_, post_process_);

    if (post_process_ || skybox_) {
        // Post-process / skybox descriptor layout: 5 combined image samplers
        VkDescriptorSetLayoutBinding bindings[5]{};
        for (int i = 0; i < 5; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorBindingFlags binding_flags[5] = {
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT,
        };
        VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info{};
        binding_flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        binding_flags_info.bindingCount = 5;
        binding_flags_info.pBindingFlags = binding_flags;

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        layout_info.bindingCount = 5;
        layout_info.pBindings = bindings;
        layout_info.pNext = &binding_flags_info;
        vkCreateDescriptorSetLayout(device_->device(), &layout_info, nullptr, &descriptor_set_layout_);

        // Push constants: post-process 为 exposure+mode（fragment）；
        // skybox 为 view+projection 两个 mat4（vertex）。
        VkPushConstantRange push_range{};
        if (skybox_) {
            push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            push_range.offset = 0;
            push_range.size = sizeof(math::Matrix4f) * 2;
        } else {
            push_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            push_range.offset = 0;
            push_range.size = sizeof(PostProcessPushData);
        }

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
        pool_size.descriptorCount = static_cast<uint32_t>(frames) * 5;

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
        cached_textures_.resize(frames);
        for (auto& arr : cached_textures_) arr.fill(nullptr);

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
        // 描述符布局：UBO(0) + PBR 贴图(1-7) + IBL 贴图(9-11) + palette UBO(8, skinned)
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.reserve(skinned_ ? 20 : 19);

        VkDescriptorSetLayoutBinding ubo_binding{};
        ubo_binding.binding = 0;
        ubo_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo_binding.descriptorCount = 1;
        ubo_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(ubo_binding);

        for (int i = 1; i <= 7; ++i) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = i;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings.push_back(b);
        }

        for (int i = 9; i <= 11; ++i) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = i;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings.push_back(b);
        }

        for (int i = 12; i <= 14; ++i) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = i;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings.push_back(b);
        }

        // PCSS 深度采样（非比较 sampler）
        for (int i = 15; i <= 18; ++i) {
            VkDescriptorSetLayoutBinding b{};
            b.binding = i;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings.push_back(b);
        }

        // 屏幕空间 AO（半分辨率）
        VkDescriptorSetLayoutBinding ssao_binding{};
        ssao_binding.binding = 19;
        ssao_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ssao_binding.descriptorCount = 1;
        ssao_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(ssao_binding);

        if (skinned_) {
            VkDescriptorSetLayoutBinding palette_binding{};
            palette_binding.binding = 8;
            palette_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            palette_binding.descriptorCount = 1;
            palette_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            bindings.push_back(palette_binding);
        }

        VkDescriptorSetLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout_info.bindingCount = static_cast<uint32_t>(bindings.size());
        layout_info.pBindings = bindings.data();
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

        // 1x1 白色回退贴图，保证 prepare_draw 里每个贴图 binding 都写入
        // 合法描述符（新分配的描述符集内容未定义，留空可能被 shader 采样
        // 导致 GPU 读垃圾描述符挂死）。IBL 的 irradiance/prefilter 是
        // samplerCube，必须另备 1x1 立方体回退，否则 2D view 配 cube
        // 采样器是 UB（验证层报错，部分驱动 device lost）。
        fallback_texture_ = std::make_unique<VulkanTexture>(device_);
        const uint32_t white_pixel = 0xFFFFFFFF;
        if (!fallback_texture_->upload_data(&white_pixel, 1, 1, 4)) {
            GLOG_ERROR("VulkanShader: failed to create fallback texture");
            fallback_texture_.reset();
            return false;
        }
        fallback_cube_ = std::make_unique<VulkanTexture>(device_);
        const void* white_faces[6] = {&white_pixel, &white_pixel, &white_pixel,
                                      &white_pixel, &white_pixel, &white_pixel};
        if (!fallback_cube_->upload_cubemap(white_faces, 1, 1, 4)) {
            GLOG_ERROR("VulkanShader: failed to create fallback cube texture");
            fallback_cube_.reset();
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
    } else if (skinned_) {
        vertex_binding.stride = 88; // SkinnedVertexGPU（MeshVertex + bone ids + weights）
        attrs.push_back({0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0});    // position
        attrs.push_back({1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12});   // normal
        attrs.push_back({2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24});   // tangent
        attrs.push_back({3, 0, VK_FORMAT_R32G32_SFLOAT, 36});      // uv
        attrs.push_back({4, 0, VK_FORMAT_R32G32B32_SFLOAT, 44});   // color
        attrs.push_back({5, 0, VK_FORMAT_R32G32B32A32_UINT, 56});  // bone ids
        attrs.push_back({6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 72});// weights
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
    // 天空盒从立方体内部观察，禁用剔除
    raster.cullMode = (post_process_ || skybox_) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
    // Negative viewport height restores OpenGL's Y convention, so keep the same
    // winding convention as OpenGL: counter-clockwise front face with back culling.
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    // Shadow map 输出深度：启用硬件斜率缩放 depth bias。
    // 掠射角（地面、大平面）下 shader 内基于法线的 bias 不足以覆盖整个
    // 视锥尺寸阴影盒的 texel 深度差，会产生随镜头移动的 shadow acne 条纹；
    // 硬件 slope-scaled bias 按表面坡度自动加权，两者叠加后条纹消除且
    // constant 很小，不会明显 Peter-panning。
    if (!color_output_enabled_ && !post_process_ && !skybox_) {
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = 1.25f;
        raster.depthBiasSlopeFactor = 2.5f;
        raster.depthBiasClamp = 0.0f;
    }

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable = post_process_ ? VK_FALSE : VK_TRUE;
    // 天空盒深度恒为远平面：不写深度，LESS_OR_EQUAL 保证无动态状态扩展时也能通过
    depth_stencil.depthWriteEnable = (post_process_ || skybox_) ? VK_FALSE : VK_TRUE;
    depth_stencil.depthCompareOp = skybox_ ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blend_attach{};
    // 混合不是动态状态；Vulkan 必须在管线创建时定死。
    // 对 PBR / 网格统一启用 alpha 混合：
    //   - 不透明材质 alpha=1，混合结果等于不混合（安全）；
    //   - 半透明材质（TriggerZone、Glass 等）alpha<1，GL 端走 forward
    //     透明排序 + blend，VK 之前硬编码关闭混合导致它们渲染错误。
    // Shadow / post-process / skybox 仍保持不混合。
    blend_attach.blendEnable =
        (!post_process_ && !skybox_ && color_output_enabled_) ? VK_TRUE : VK_FALSE;
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

// name 必须以 field 开头、以 [i] 结尾（如 "uLightPos[3]"）。
bool VulkanShader::parse_light_index(const std::string& name, const char* field, int& index) {
    const size_t flen = std::strlen(field);
    if (name.compare(0, flen, field) != 0) return false;
    if (name.size() <= flen + 2 || name[flen] != '[' || name.back() != ']') return false;
    index = std::atoi(name.c_str() + flen + 1);
    return index >= 0 && index < k_max_lights;
}

void VulkanShader::set_int(const std::string& name, int value) {
    int light_index = -1;
    if (name == "uUseAlbedoMap") ubo_data_.use_albedo_map = value;
    else if (name == "uUseNormalMap") ubo_data_.use_normal_map = value;
    else if (name == "uUseRoughnessMap") ubo_data_.use_roughness_map = value;
    else if (name == "uUseMetallicMap") ubo_data_.use_metallic_map = value;
    else if (name == "uUseAOMap") ubo_data_.use_ao_map = value;
    else if (name == "uUseEmissiveMap") ubo_data_.use_emissive_map = value;
    else if (name == "uUseShadowMap") ubo_data_.use_shadow_map = value;
    else if (name == "uHDREnabled") ubo_data_.hdr_enabled = value;
    else if (name == "uLightCount") ubo_data_.light_count = value;
    else if (name == "uShadowLightIndex") ubo_data_.shadow_light_index = value;
    else if (name == "uUseIBL") ubo_data_.use_ibl = value;
    else if (name == "uTwoSided") ubo_data_.two_sided = value;
    else if (name == "uCascadeCount") ubo_data_.cascade_count = value;
    else if (name == "uPCSSEnabled") ubo_data_.pcss_enabled = value;
    else if (name == "uDebugMode") ubo_data_.debug_mode = value;
    else if (name == "uUseSSAO") ubo_data_.use_ssao = value;
    else if (parse_light_index(name, "uLightType", light_index)) {
        ubo_data_.lights[light_index].pos_type.w = static_cast<float>(value);
    }
    ubo_dirty_ = true;
}
void VulkanShader::set_int(const char* name, int value) {
    if (!name) return;
    set_int(std::string(name), value);
}

void VulkanShader::set_float(const std::string& name, float value) {
    int light_index = -1;
    if (name == "uRoughness") ubo_data_.roughness = value;
    else if (name == "uMetallic") ubo_data_.metallic = value;
    else if (name == "uAO") ubo_data_.ao = value;
    else if (name == "uOpacity") ubo_data_.emissive_opacity.w = value;
    else if (name == "uIBLIntensity") ubo_data_.ibl_intensity = value;
    else if (name == "uPCSSLightSize") ubo_data_.pcss_light_size = value;
    else if (name == "uPCSSMaxRadius") ubo_data_.pcss_max_radius = value;
    else if (name == "uPCSSBlockerScale") ubo_data_.pcss_tap_scale = value;
    else if (name == "uNormalOffset") shadow_normal_offset_ = value;
    else if (name == "uClearcoat") ubo_data_.clearcoat = value;
    else if (name == "uClearcoatRoughness") ubo_data_.clearcoat_roughness = value;
    else if (name == "uSheen") ubo_data_.sheen = value;
    else if (name == "uAnisotropy") ubo_data_.anisotropy = value;
    else if (name == "uAnisotropyRotation") ubo_data_.anisotropy_rotation = value;
    else if (name == "uSSAOStrength") ubo_data_.ssao_strength = value;
    else if (name == "uLightIntensity") ubo_data_.lights[0].color_intensity.w = value; // 旧版单光 API
    else if (parse_light_index(name, "uLightIntensity", light_index)) {
        ubo_data_.lights[light_index].color_intensity.w = value;
    }
    ubo_dirty_ = true;
}
void VulkanShader::set_float(const char* name, float value) {
    if (!name) return;
    set_float(std::string(name), value);
}

void VulkanShader::set_vec2(const std::string& /*name*/, const math::Vector2f& /*value*/) {}
void VulkanShader::set_vec2(const char* /*name*/, const math::Vector2f& /*value*/) {}

void VulkanShader::set_vec3(const std::string& name, const math::Vector3f& value) {
    auto to_vec4 = [](const math::Vector3f& v) { return math::Vector4f(v.x, v.y, v.z, 0.0f); };
    int light_index = -1;
    if (name == "uAlbedoColor") ubo_data_.albedo_color = to_vec4(value);
    else if (name == "uSheenTint") ubo_data_.sheen_tint = to_vec4(value);
    else if (name == "uCameraPos") ubo_data_.camera_pos = to_vec4(value);
    else if (name == "uAmbient") ubo_data_.ambient = to_vec4(value);
    else if (name == "uEmissiveColor") {
        ubo_data_.emissive_opacity.x = value.x;
        ubo_data_.emissive_opacity.y = value.y;
        ubo_data_.emissive_opacity.z = value.z;
    }
    else if (name == "uLightDir") ubo_data_.lights[0].dir_range = to_vec4(value);      // 旧版单光 API
    else if (name == "uLightColor") ubo_data_.lights[0].color_intensity = to_vec4(value); // 旧版单光 API
    else if (parse_light_index(name, "uLightPos", light_index)) {
        ubo_data_.lights[light_index].pos_type.x = value.x;
        ubo_data_.lights[light_index].pos_type.y = value.y;
        ubo_data_.lights[light_index].pos_type.z = value.z;
    }
    else if (parse_light_index(name, "uLightDir", light_index)) {
        ubo_data_.lights[light_index].dir_range.x = value.x;
        ubo_data_.lights[light_index].dir_range.y = value.y;
        ubo_data_.lights[light_index].dir_range.z = value.z;
    }
    else if (parse_light_index(name, "uLightColor", light_index)) {
        ubo_data_.lights[light_index].color_intensity.x = value.x;
        ubo_data_.lights[light_index].color_intensity.y = value.y;
        ubo_data_.lights[light_index].color_intensity.z = value.z;
    }
    ubo_dirty_ = true;
}
void VulkanShader::set_vec3(const char* name, const math::Vector3f& value) {
    if (!name) return;
    set_vec3(std::string(name), value);
}

void VulkanShader::set_vec4(const std::string& name, const math::Vector4f& value) {
    int light_index = -1;
    if (name == "uUVTransform") {
        ubo_data_.uv_transform = value;
    } else if (name == "uCascadeSplits") {
        ubo_data_.cascade_splits = value;
    } else if (name == "uCascadeBias") {
        ubo_data_.cascade_bias = value;
    } else if (name == "uCascadeFarBlend") {
        ubo_data_.cascade_far_blend = value;
    } else if (parse_light_index(name, "uLightParams", light_index)) {
        // x=range, y=cos(outer), z=cos(inner)
        ubo_data_.lights[light_index].dir_range.w = value.x;
        ubo_data_.lights[light_index].spot = math::Vector4f(value.y, value.z, 0.0f, 0.0f);
    }
    ubo_dirty_ = true;
}
void VulkanShader::set_vec4(const char* name, const math::Vector4f& value) {
    if (!name) return;
    set_vec4(std::string(name), value);
}

void VulkanShader::set_mat4(const std::string& name, const math::Matrix4f& value) {
    if (name == "uModel") model_ = value;
    else if (name == "uView") {
        view_ = value;
        ubo_data_.view_matrix = value;
        ubo_dirty_ = true;
    }
    else if (name == "uProjection") {
        // OpenGL projection matrices use Z in [-1, 1]; Vulkan NDC uses [0, 1].
        // Remap the Z row while keeping Y unchanged; Y is flipped via negative viewport.
        math::Matrix4f vk_proj = value;
        vk_proj(2, 2) = value(2, 2) * 0.5f + value(3, 2) * 0.5f;
        vk_proj(2, 3) = value(2, 3) * 0.5f + value(3, 3) * 0.5f;
        projection_ = vk_proj;
    }
    else if (name == "uLightSpaceMatrix") {
        // Shadow map 的投影矩阵同样使用 OpenGL 风格 [-1,1] Z，
        // 在 Vulkan NDC [0,1] 下会导致深度范围与采样值不一致，
        // 因此需要做与 uProjection 相同的 Z 行重映射。
        math::Matrix4f vk_light = value;
        vk_light(2, 2) = value(2, 2) * 0.5f + value(3, 2) * 0.5f;
        vk_light(2, 3) = value(2, 3) * 0.5f + value(3, 3) * 0.5f;
        light_space_matrix_ = vk_light;
    }
}
void VulkanShader::set_mat4(const char* name, const math::Matrix4f& value) {
    if (!name) return;
    set_mat4(std::string(name), value);
}

void VulkanShader::set_mat4_array(const char* name, const math::Matrix4f* data, uint32_t count) {
    if (!name) return;
    if (std::strcmp(name, "uCascadeLightSpace") == 0) {
        const uint32_t n = std::min<uint32_t>(count, k_max_cascades);
        for (uint32_t i = 0; i < n; ++i) {
            // 与单矩阵 uLightSpaceMatrix 相同的 OpenGL→Vulkan NDC z 重映射
            const math::Matrix4f& v = data[i];
            math::Matrix4f vk_m = v;
            vk_m(2, 2) = v(2, 2) * 0.5f + v(3, 2) * 0.5f;
            vk_m(2, 3) = v(2, 3) * 0.5f + v(3, 3) * 0.5f;
            ubo_data_.cascade_light_space[i] = vk_m;
        }
        ubo_data_.cascade_count = std::max(ubo_data_.cascade_count, static_cast<int>(n));
        ubo_dirty_ = true;
        return;
    }
    if (std::strcmp(name, "uBonePalette") != 0) return;
    if (!data || count == 0) {
        palette_count_ = 0;
        return;
    }
    if (count > k_max_skinning_bones) {
        GLOG_WARN("VulkanShader::set_mat4_array: bone count {} exceeds limit {}, truncated",
                  count, k_max_skinning_bones);
        count = k_max_skinning_bones;
    }
    std::memcpy(palette_.data(), data, count * sizeof(math::Matrix4f));
    palette_count_ = count;
}

bool VulkanShader::create_ubo() {
    int frames = swapchain_ ? swapchain_->frames_in_flight() : 1;
    ubo_buffers_.clear();
    ubo_buffers_.reserve(frames);
    draw_counts_.assign(frames, 0);
    // 每帧一个大 UBO，按 draw 游标以 ubo_stride_ 切分，保证同帧不同
    // draw 的材质参数互不覆盖。
    const VkDeviceSize per_frame_size = static_cast<VkDeviceSize>(ubo_stride_) * max_draws_per_frame_;
    for (int i = 0; i < frames; ++i) {
        auto buffer = std::make_unique<VulkanBuffer>();
        if (!buffer->init(device_, per_frame_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            GLOG_ERROR("VulkanShader: failed to create UBO for frame {}", i);
            return false;
        }
        ubo_buffers_.push_back(std::move(buffer));
    }

    // 蒙皮 palette UBO：每帧一个，按 draw 游标以 palette_stride_ 切分（与主 UBO 同 cursor）。
    // skinned draw 上限独立于普通 draw（256/帧），避免 8KB stride 放大主 UBO。
    palette_buffers_.clear();
    if (skinned_) {
        palette_buffers_.reserve(frames);
        const VkDeviceSize palette_size =
            static_cast<VkDeviceSize>(palette_stride_) * max_skinned_draws_per_frame_;
        for (int i = 0; i < frames; ++i) {
            auto buffer = std::make_unique<VulkanBuffer>();
            if (!buffer->init(device_, palette_size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                GLOG_ERROR("VulkanShader: failed to create palette UBO for frame {}", i);
                return false;
            }
            palette_buffers_.push_back(std::move(buffer));
        }
    }
    return true;
}

bool VulkanShader::create_descriptor_pool() {
    int frames = swapchain_ ? swapchain_->frames_in_flight() : 1;

    // 每帧一个描述符池；每 draw 从池中分配一套全新描述符集，
    // on_begin_frame 时整池 reset（比逐个 free 快，且无需 FREE bit）。
    descriptor_pools_.assign(frames, VK_NULL_HANDLE);
    for (int i = 0; i < frames; ++i) {
        VkDescriptorPoolSize pool_sizes[2]{};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        // skinned 管线每 draw 多消耗一个 palette UBO 描述符
        pool_sizes[0].descriptorCount = max_draws_per_frame_ * (skinned_ ? 2 : 1);
        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        // 每 draw 最多 10 张采样器：PBR(1-7) + IBL(9-11)
        pool_sizes[1].descriptorCount = max_draws_per_frame_ * 19;

        VkDescriptorPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.poolSizeCount = 2;
        info.pPoolSizes = pool_sizes;
        info.maxSets = max_draws_per_frame_;

        if (vkCreateDescriptorPool(device_->device(), &info, nullptr, &descriptor_pools_[i]) != VK_SUCCESS) {
            GLOG_ERROR("VulkanShader: failed to create descriptor pool for frame {}", i);
            return false;
        }
    }
    return true;
}

void VulkanShader::set_texture(int slot, ITexture* texture) {
    // post-process / skybox: 只有一个 combined image sampler，固定 binding 0。
    // PBR/IBL: 经 slot_to_binding 映射到与 GLSL layout(binding=...) 一致的 binding。
    int binding = uses_fixed_descriptor_sets() ? post_process_binding(slot)
                                               : slot_to_binding(slot);
    if (binding < 0 || binding >= k_max_texture_bindings || !texture) return;
    auto* vk_tex = dynamic_cast<VulkanTexture*>(texture);
    if (!vk_tex || !vk_tex->image_view() || !vk_tex->sampler()) return;

    if (!uses_fixed_descriptor_sets()) {
        // 非 post-process：只记录当前绑定，prepare_draw 时为每个 draw
        // 分配全新描述符集并写入，避免同帧不同材质互相覆盖。
        current_textures_[binding] = vk_tex;
        return;
    }

    int frame = current_frame();
    if (frame < 0 || frame >= static_cast<int>(cached_textures_.size())) return;

    auto& cached = cached_textures_[frame][binding];
    if (cached == vk_tex) return;
    cached = vk_tex;

    VkDescriptorImageInfo image_info{};
    image_info.imageLayout = vk_tex->is_depth()
                                 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_info.imageView = vk_tex->image_view();
    // 深度纹理在 post-process（GTAO / SSAO blur）里用于重建视图位置，
    // 必须用非比较 sampler 读原始深度；比较 sampler 会返回 0/1 比较结果。
    image_info.sampler = (vk_tex->is_depth() && vk_tex->depth_sampler())
                             ? vk_tex->depth_sampler()
                             : vk_tex->sampler();

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptor_sets_[frame];
    write.dstBinding = static_cast<uint32_t>(binding);
    write.dstArrayElement = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;

    vkUpdateDescriptorSets(device_->device(), 1, &write, 0, nullptr);
}

void VulkanShader::invalidate_texture_cache(const VulkanTexture* tex) {
    if (!tex) return;
    for (auto& t : current_textures_) {
        if (t == tex) t = nullptr;
    }
    for (auto& per_frame : cached_textures_) {
        for (auto& t : per_frame) {
            if (t == tex) t = nullptr;
        }
    }
}

bool VulkanShader::is_valid() const {
    return pipeline_ != VK_NULL_HANDLE;
}

void VulkanShader::on_begin_frame(int frame_index) {
    if (uses_fixed_descriptor_sets()) return;
    if (frame_index < 0 || frame_index >= static_cast<int>(descriptor_pools_.size())) return;
    // 该帧上一周期的命令缓冲已被 frame fence 保证执行完毕，整池 reset 安全。
    vkResetDescriptorPool(device_->device(), descriptor_pools_[frame_index], 0);
    draw_counts_[frame_index] = 0;
}

void VulkanShader::prepare_draw(VkCommandBuffer cmd) {
    if (uses_fixed_descriptor_sets() || cmd == VK_NULL_HANDLE) return;
    int frame = current_frame();
    if (frame < 0 || frame >= static_cast<int>(descriptor_pools_.size()) ||
        frame >= static_cast<int>(ubo_buffers_.size())) {
        return;
    }

    uint32_t cursor = draw_counts_[frame];
    if (cursor >= max_draws_per_frame_) {
        // 超出单帧 draw 上限：复用最后一格（该 draw 与上一个 overflow
        // draw 会共享描述符，仅影响超上限部分）。
        cursor = max_draws_per_frame_ - 1;
    } else {
        draw_counts_[frame] = cursor + 1;
    }
    const VkDeviceSize ubo_offset = static_cast<VkDeviceSize>(cursor) * ubo_stride_;

    // 1. 写入本 draw 的 UBO 数据
    ubo_buffers_[frame]->upload(&ubo_data_, sizeof(UBOData), ubo_offset);

    // 2. 从该帧池中分配全新描述符集
    VkDescriptorSetAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = descriptor_pools_[frame];
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &descriptor_set_layout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_->device(), &alloc, &set) != VK_SUCCESS || set == VK_NULL_HANDLE) {
        GLOG_ERROR("VulkanShader::prepare_draw: failed to allocate descriptor set");
        return;
    }

    // 3. 写入 UBO + 贴图。每个贴图 binding 都必须写入：未绑定的用 1x1
    // 回退贴图占位，否则新分配的描述符集对应 binding 是未定义内容，
    // shader 一旦采样（编译器可能提升条件分支外的采样）GPU 读垃圾挂死。
    VkWriteDescriptorSet writes[k_max_texture_bindings + 1]{};
    VkDescriptorBufferInfo buffer_info{};
    buffer_info.buffer = ubo_buffers_[frame]->buffer();
    buffer_info.offset = ubo_offset;
    buffer_info.range = ubo_stride_;

    uint32_t write_count = 0;
    writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[write_count].dstSet = set;
    writes[write_count].dstBinding = 0;
    writes[write_count].dstArrayElement = 0;
    writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[write_count].descriptorCount = 1;
    writes[write_count].pBufferInfo = &buffer_info;
    ++write_count;

    // 蒙皮 palette：与主 UBO 共用 cursor，上传当前 palette 缓存并绑到 binding 8。
    // palette_count_ == 0（首帧未设置）时上传单位阵，避免顶点被零矩阵压扁。
    // 超出 skinned draw 上限时 clamp 到最后一格（与主 UBO overflow 策略一致，
    // 保证 binding 8 永远写入合法描述符）。
    VkDescriptorBufferInfo palette_info{};
    if (skinned_ && frame < static_cast<int>(palette_buffers_.size())) {
        const uint32_t palette_cursor =
            cursor < max_skinned_draws_per_frame_ ? cursor : max_skinned_draws_per_frame_ - 1;
        const VkDeviceSize palette_offset = static_cast<VkDeviceSize>(palette_cursor) * palette_stride_;
        if (palette_count_ > 0) {
            palette_buffers_[frame]->upload(palette_.data(),
                                            palette_count_ * sizeof(math::Matrix4f),
                                            palette_offset);
        } else {
            // 全量 128 个单位阵：shader 可能索引任意 bone id，必须全部合法
            std::array<math::Matrix4f, k_max_skinning_bones> identity;
            for (auto& m : identity) m = math::Matrix4f::identity();
            palette_buffers_[frame]->upload(identity.data(),
                                            k_max_skinning_bones * sizeof(math::Matrix4f),
                                            palette_offset);
        }
        palette_info.buffer = palette_buffers_[frame]->buffer();
        palette_info.offset = palette_offset;
        palette_info.range = palette_stride_;

        writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write_count].dstSet = set;
        writes[write_count].dstBinding = 8;
        writes[write_count].dstArrayElement = 0;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[write_count].descriptorCount = 1;
        writes[write_count].pBufferInfo = &palette_info;
        ++write_count;
    }

    VkDescriptorImageInfo image_infos[k_max_texture_bindings]{};
    for (int binding = 1; binding < k_max_texture_bindings; ++binding) {
        // binding 8 是 skinned palette UBO，不是采样器；跳过避免误写成 image。
        if (binding == 8) continue;
        VulkanTexture* vk_tex = current_textures_[binding];
        if (!vk_tex || !vk_tex->image_view() || !vk_tex->sampler()) {
            // IBL binding 9/10 在 shader 中是 samplerCube：回退必须用立方体贴图，
            // 绑 2D view 到 cube 采样器是 UB（验证层报错，部分驱动 device lost）。
            const bool cube_binding = (binding == 9 || binding == 10);
            vk_tex = (cube_binding && fallback_cube_) ? fallback_cube_.get()
                                                      : fallback_texture_.get();
        }
        if (!vk_tex || !vk_tex->image_view() || !vk_tex->sampler()) continue;
        // PCSS 深度采样 binding（15-18）用非比较 sampler 读原始深度
        const bool pcss_depth_binding = (binding >= 15 && binding <= 18);
        VkSampler use_sampler = vk_tex->sampler();
        if (pcss_depth_binding && vk_tex->depth_sampler()) {
            use_sampler = vk_tex->depth_sampler();
        }
        auto& image_info = image_infos[binding];
        image_info.imageLayout = vk_tex->is_depth()
                                     ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                     : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        image_info.imageView = vk_tex->image_view();
        image_info.sampler = use_sampler;

        writes[write_count].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[write_count].dstSet = set;
        writes[write_count].dstBinding = static_cast<uint32_t>(binding);
        writes[write_count].dstArrayElement = 0;
        writes[write_count].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[write_count].descriptorCount = 1;
        writes[write_count].pImageInfo = &image_info;
        ++write_count;
    }
    vkUpdateDescriptorSets(device_->device(), write_count, writes, 0, nullptr);

    // 4. 绑定本 draw 独享的描述符集
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            0, 1, &set, 0, nullptr);
}

void VulkanShader::update_ubo(VkCommandBuffer /*cmd*/) const {
    // 保留空实现：UBO 上传已并入 prepare_draw（每 draw 独立偏移）。
}

void VulkanShader::push_constants(VkCommandBuffer cmd) const {
    // push constant 是命令缓冲状态：每帧重录 CB 后必须无条件重新写入，
    // 不能用脏标记跨帧跳过（否则验证层报 VUID-vkCmdDraw-None-08601，
    // 且着色器读到的是未定义数据）。
    if (post_process_) {
        PostProcessPushData data{};
        data.exposure = pp_params_.exposure;
        data.ev100 = pp_params_.ev100;
        data.mode = pp_params_.tone_map_mode;
        data.dithering = pp_params_.dithering;
        data.white_point = pp_params_.white_point;
        data.black_point = pp_params_.black_point;
        data.contrast = pp_params_.contrast;
        data.saturation = pp_params_.saturation;
        data.lift = pp_params_.lift;
        data.gamma = pp_params_.gamma;
        data.gain = pp_params_.gain;
        data.shadows = pp_params_.shadows;
        data.midtones = pp_params_.midtones;
        data.highlights = pp_params_.highlights;
        data.bloom_enabled = pp_params_.bloom_enabled;
        data.bloom_threshold = pp_params_.bloom_threshold;
        data.bloom_intensity = pp_params_.bloom_intensity;
        data.film_grain = pp_params_.film_grain;
        data.vignette = pp_params_.vignette;
        data.chromatic_aberration = pp_params_.chromatic_aberration;
        data.use_lut = pp_params_.use_lut;
        data.lut_strength = pp_params_.lut_strength;
        data.auto_exposure = pp_params_.auto_exposure;
        data.ae_target_luminance = pp_params_.ae_target_luminance;
        data.ae_min_exposure = pp_params_.ae_min_exposure;
        data.ae_max_exposure = pp_params_.ae_max_exposure;
        data.ae_speed = pp_params_.ae_speed;
        data.taa_enabled = pp_params_.taa_enabled;
        data.taa_weight = pp_params_.taa_weight;
        data.ssao_enabled = pp_params_.ssao_enabled;
        data.ssao_strength = pp_params_.ssao_strength;
        data.ssao_radius = pp_params_.ssao_radius;
        data.ssao_near = pp_params_.ssao_near;
        data.ssao_far = pp_params_.ssao_far;
        data.ssao_tan_half = pp_params_.ssao_tan_half;
        data.ssao_aspect = pp_params_.ssao_aspect;
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(data), &data);
    } else if (skybox_) {
        float matrices[2 * 16];
        for (int i = 0; i < 16; ++i) {
            matrices[i] = view_.m[i];
            matrices[16 + i] = projection_.m[i];
        }
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(matrices), matrices);
    } else if (!color_output_enabled_) {
        // 阴影深度 pass：{ lightSpace, model, normalOffset }
        // （无颜色输出 = shadow map 管线；normal offset 在顶点阶段沿法线推几何）
        struct ShadowPushData {
            math::Matrix4f light_space;
            math::Matrix4f model;
            float normal_offset;
            float pad[3];
        } data{};
        data.light_space = light_space_matrix_;
        data.model = model_;
        data.normal_offset = shadow_normal_offset_;
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
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
}

void VulkanShader::push_post_process_constants(VkCommandBuffer cmd,
                                               const PostProcessParams& params) const {
    if (!pipeline_layout_) return;
    PostProcessPushData data{};
    data.exposure = params.exposure;
    data.ev100 = params.ev100;
    data.mode = params.tone_map_mode;
    data.dithering = params.dithering;
    data.white_point = params.white_point;
    data.black_point = params.black_point;
    data.contrast = params.contrast;
    data.saturation = params.saturation;
    data.lift = params.lift;
    data.gamma = params.gamma;
    data.gain = params.gain;
    data.shadows = params.shadows;
    data.midtones = params.midtones;
    data.highlights = params.highlights;
    data.bloom_enabled = params.bloom_enabled;
    data.bloom_threshold = params.bloom_threshold;
    data.bloom_intensity = params.bloom_intensity;
    data.film_grain = params.film_grain;
    data.vignette = params.vignette;
    data.chromatic_aberration = params.chromatic_aberration;
    data.use_lut = params.use_lut;
    data.lut_strength = params.lut_strength;
    data.auto_exposure = params.auto_exposure;
    data.ae_target_luminance = params.ae_target_luminance;
    data.ae_min_exposure = params.ae_min_exposure;
    data.ae_max_exposure = params.ae_max_exposure;
    data.ae_speed = params.ae_speed;
    data.taa_enabled = params.taa_enabled;
    data.taa_weight = params.taa_weight;
    data.ssao_enabled = params.ssao_enabled;
    data.ssao_strength = params.ssao_strength;
    data.ssao_radius = params.ssao_radius;
    data.ssao_near = params.ssao_near;
    data.ssao_far = params.ssao_far;
    data.ssao_tan_half = params.ssao_tan_half;
    data.ssao_aspect = params.ssao_aspect;
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
