// vk_ray_query_pass.cpp — Vulkan 硬件光追反射（P2.2，ray query）实现
//
// 通道自包含：编译内联 GLSL → SPIR-V、建计算管线、描述符集、uniform 缓冲与
// 内部反射存储图；render() 在给定命令缓冲上派发一次 8x8 工作组的光线查询。
// 射线查询在着色器内完成，无需主机端 KHR 扩展命令，因此这里不解析额外 PFN。
#include "vk_ray_query_pass.h"

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

#include <cstring>
#include <utility>

#include "render/glsl_compiler.h"
#include "render/vulkan/vk_device.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

// 内联 ray query 计算着色器（GLSL，Vulkan 语义 + GL_EXT_ray_query）。
// 从深度重建世界坐标与法线，沿反射方向在 TLAS 上做 ray query；命中则把命中点
// 重投影回屏幕采样 HDR 颜色作为反射贡献，未命中则保留原 HDR 颜色（passthrough）。
static const char* k_ray_query_glsl = R"GLSL(
#version 460
#extension GL_EXT_ray_query : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) uniform accelerationStructureEXT uTlas;
layout(std140, binding = 1) uniform UBO {
    mat4 invViewProj;
    mat4 viewProj;
    vec4 camPosMaxDist;        // xyz = 相机世界坐标, w = 最大追踪距离
    vec4 resolutionStrength;    // xy = 分辨率, z = 反射强度
};
layout(binding = 2) uniform texture2D uDepth;
layout(binding = 3) uniform texture2D uLit;
layout(binding = 4, rgba16f) uniform image2D uOut;

vec3 reconstructWorld(vec2 uv, float depth) {
    vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    vec4 world = invViewProj * clip;
    return world.xyz / world.w;
}

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    ivec2 res = ivec2(resolutionStrength.xy);
    if (px.x >= res.x || px.y >= res.y) return;

    vec2 uv = (vec2(px) + 0.5) / resolutionStrength.xy;
    float depth = texelFetch(uDepth, px, 0).r;
    vec3 lit = texelFetch(uLit, px, 0).rgb;

    // 远处（天空 / 远平面）不参与反射
    if (depth >= 1.0) {
        imageStore(uOut, px, vec4(lit, 1.0));
        return;
    }

    vec3 world = reconstructWorld(uv, depth);
    float dR = texelFetch(uDepth, min(px + ivec2(1, 0), res - ivec2(1)), 0).r;
    float dU = texelFetch(uDepth, min(px + ivec2(0, 1), res - ivec2(1)), 0).r;
    vec3 wR = reconstructWorld(uv + vec2(1.0 / float(res.x), 0.0), dR);
    vec3 wU = reconstructWorld(uv + vec2(0.0, 1.0 / float(res.y)), dU);
    vec3 n = normalize(cross(wR - world, wU - world));
    vec3 vdir = normalize(world - camPosMaxDist.xyz);
    vec3 rdir = reflect(vdir, n);

    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTlas, gl_RayFlagsOpaqueEXT, 0xFF,
                          world + n * 0.01, 0.01, rdir, camPosMaxDist.w);
    while (rayQueryProceedEXT(rq)) {}

    vec3 outCol = lit;
    if (rayQueryGetIntersectionTypeEXT(rq, true) != gl_RayQueryCommittedIntersectionNoneEXT) {
        vec3 ro = rayQueryGetIntersectionWorldRayOriginEXT(rq, true);
        vec3 rd = rayQueryGetIntersectionWorldRayDirectionEXT(rq, true);
        float t = rayQueryGetIntersectionTEXT(rq, true);
        vec3 hit = ro + rd * t;
        vec4 hc = viewProj * vec4(hit, 1.0);
        vec2 huv = (hc.xy / hc.w) * 0.5 + 0.5;
        huv.y = 1.0 - huv.y;
        ivec2 hp = ivec2(clamp(huv, 0.0, 1.0) * resolutionStrength.xy);
        vec3 refl = texelFetch(uLit, hp, 0).rgb;
        outCol = mix(lit, refl, resolutionStrength.z);
    }
    imageStore(uOut, px, vec4(outCol, 1.0));
}
)GLSL";

VkRayQueryPass::VkRayQueryPass(VkRayQueryPass&& other) noexcept { *this = std::move(other); }

VkRayQueryPass& VkRayQueryPass::operator=(VkRayQueryPass&& other) noexcept {
    if (this != &other) {
        device_ = other.device_;
        valid_ = other.valid_;
        module_ = other.module_;
        layout_ = other.layout_;
        pipeline_ = other.pipeline_;
        desc_layout_ = other.desc_layout_;
        pool_ = other.pool_;
        set_ = other.set_;
        ubo_ = other.ubo_;
        one_shot_pool_ = other.one_shot_pool_;
        refl_image_ = other.refl_image_;
        refl_alloc_ = other.refl_alloc_;
        refl_view_ = other.refl_view_;
        refl_w_ = other.refl_w_;
        refl_h_ = other.refl_h_;
        refl_format_ = other.refl_format_;
        tlas_bound_ = other.tlas_bound_;

        other.device_ = nullptr;
        other.valid_ = false;
        other.module_ = VK_NULL_HANDLE;
        other.layout_ = VK_NULL_HANDLE;
        other.pipeline_ = VK_NULL_HANDLE;
        other.desc_layout_ = VK_NULL_HANDLE;
        other.pool_ = VK_NULL_HANDLE;
        other.set_ = VK_NULL_HANDLE;
        other.ubo_ = nullptr;
        other.one_shot_pool_ = VK_NULL_HANDLE;
        other.refl_image_ = VK_NULL_HANDLE;
        other.refl_alloc_ = nullptr;
        other.refl_view_ = VK_NULL_HANDLE;
        other.refl_w_ = 0;
        other.refl_h_ = 0;
        other.tlas_bound_ = false;
    }
    return *this;
}

bool VkRayQueryPass::init(VulkanDevice* device) {
    shutdown();
    if (!device || !device->is_valid()) return false;
    device_ = device;
    VkDevice dev = device->device();

    std::vector<uint32_t> spirv;
    std::string err;
    if (!compile_glsl_to_spirv(k_ray_query_glsl, "ray_query.comp", GlslStage::Compute, spirv, err)) {
        GLOG_WARN("VkRayQueryPass: GLSL compile failed ({}); ray-query reflection disabled", err);
        return false;
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv.size() * sizeof(uint32_t);
    module_info.pCode = spirv.data();
    if (vkCreateShaderModule(dev, &module_info, nullptr, &module_) != VK_SUCCESS) {
        GLOG_ERROR("VkRayQueryPass: vkCreateShaderModule failed");
        return false;
    }

    // 描述符集布局：TLAS / uniform / 采样深度 / 采样 HDR / 存储输出
    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[3].binding = 3;
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = 5;
    set_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &set_info, nullptr, &desc_layout_) != VK_SUCCESS) {
        GLOG_ERROR("VkRayQueryPass: vkCreateDescriptorSetLayout failed");
        return false;
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &desc_layout_;
    if (vkCreatePipelineLayout(dev, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        GLOG_ERROR("VkRayQueryPass: vkCreatePipelineLayout failed");
        return false;
    }

    // 双槽位轮转（在途帧），避免上一帧命令缓冲还在读时本帧覆盖描述符。
    constexpr uint32_t k_slots = 2;
    VkDescriptorPoolSize pool_sizes[5]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    pool_sizes[0].descriptorCount = k_slots;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = k_slots;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[2].descriptorCount = k_slots * 2;
    pool_sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[3].descriptorCount = k_slots;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = k_slots;
    pool_info.poolSizeCount = 4;
    pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(dev, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
        GLOG_ERROR("VkRayQueryPass: vkCreateDescriptorPool failed");
        return false;
    }

    VkDescriptorSet sets[2]{};
    VkDescriptorSetLayout layouts[2] = {desc_layout_, desc_layout_};
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool_;
    alloc_info.descriptorSetCount = k_slots;
    alloc_info.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(dev, &alloc_info, sets) != VK_SUCCESS) {
        GLOG_ERROR("VkRayQueryPass: vkAllocateDescriptorSets failed");
        return false;
    }
    set_ = sets[0];  // 单帧派发，槽位 0 足够（render 每帧重写）

    // uniform 缓冲（HOST_VISIBLE + COHERENT，每帧 map-free 上传）
    ubo_ = new VulkanBuffer();
    if (!ubo_->init(device, sizeof(RayQueryUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        GLOG_ERROR("VkRayQueryPass: uniform buffer init failed");
        return false;
    }

    // 一次性命令池（仅用于反射图创建时的布局转置）
    VkCommandPoolCreateInfo cp_info{};
    cp_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_info.queueFamilyIndex = device->graphics_queue_family();
    cp_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(dev, &cp_info, nullptr, &one_shot_pool_) != VK_SUCCESS) {
        GLOG_ERROR("VkRayQueryPass: vkCreateCommandPool failed");
        return false;
    }

    valid_ = true;
    GLOG_INFO("VkRayQueryPass: ray-query reflection pipeline ready");
    return true;
}

void VkRayQueryPass::shutdown() {
    if (!device_ || !device_->is_valid()) {
        device_ = nullptr;
        valid_ = false;
        module_ = VK_NULL_HANDLE;
        layout_ = VK_NULL_HANDLE;
        pipeline_ = VK_NULL_HANDLE;
        desc_layout_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE;
        set_ = VK_NULL_HANDLE;
        ubo_ = nullptr;
        one_shot_pool_ = VK_NULL_HANDLE;
        refl_image_ = VK_NULL_HANDLE;
        refl_alloc_ = nullptr;
        refl_view_ = VK_NULL_HANDLE;
        return;
    }
    VkDevice dev = device_->device();
    if (pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (desc_layout_) vkDestroyDescriptorSetLayout(dev, desc_layout_, nullptr);
    if (module_) vkDestroyShaderModule(dev, module_, nullptr);
    if (one_shot_pool_) vkDestroyCommandPool(dev, one_shot_pool_, nullptr);
    if (ubo_) { ubo_->shutdown(); delete ubo_; ubo_ = nullptr; }
    destroy_reflection_target();
    // refl_image_/refl_view_ 在 destroy_reflection_target 中已清
    device_ = nullptr;
    valid_ = false;
}

void VkRayQueryPass::transition_image(VkImage image, VkImageLayout old_layout,
                                     VkImageLayout new_layout, VkAccessFlags src_access,
                                     VkAccessFlags dst_access, VkImageAspectFlags aspect) {
    VkDevice dev = device_->device();
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = one_shot_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &alloc, &cmd);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = old_layout;
    bar.newLayout = new_layout;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.srcAccessMask = src_access;
    bar.dstAccessMask = dst_access;
    bar.image = image;
    bar.subresourceRange.aspectMask = aspect;
    bar.subresourceRange.levelCount = 1;
    bar.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);
    vkEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(dev, &fence_info, nullptr, &fence);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(device_->graphics_queue(), 1, &submit, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, ~0ULL);

    vkDestroyFence(dev, fence, nullptr);
    vkFreeCommandBuffers(dev, one_shot_pool_, 1, &cmd);
}

bool VkRayQueryPass::create_reflection_target(uint32_t width, uint32_t height) {
    destroy_reflection_target();
    VkDevice dev = device_->device();

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = refl_format_;
    info.extent.width = width;
    info.extent.height = height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(device_->allocator(), &info, &alloc_info, &refl_image_, &refl_alloc_,
                       nullptr) != VK_SUCCESS) {
        GLOG_ERROR("VkRayQueryPass: vmaCreateImage failed");
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = refl_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = refl_format_;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(dev, &view_info, nullptr, &refl_view_) != VK_SUCCESS) {
        GLOG_ERROR("VkRayQueryPass: vkCreateImageView failed");
        return false;
    }

    // 初始布局：UNDEFINED → GENERAL（存储图可直接写入）
    transition_image(refl_image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    refl_w_ = static_cast<int>(width);
    refl_h_ = static_cast<int>(height);
    return true;
}

void VkRayQueryPass::destroy_reflection_target() {
    if (!device_ || !device_->is_valid()) {
        refl_image_ = VK_NULL_HANDLE;
        refl_alloc_ = nullptr;
        refl_view_ = VK_NULL_HANDLE;
        refl_w_ = 0;
        refl_h_ = 0;
        return;
    }
    VkDevice dev = device_->device();
    if (refl_view_) vkDestroyImageView(dev, refl_view_, nullptr);
    if (refl_image_) vmaDestroyImage(device_->allocator(), refl_image_, refl_alloc_);
    refl_image_ = VK_NULL_HANDLE;
    refl_alloc_ = nullptr;
    refl_view_ = VK_NULL_HANDLE;
    refl_w_ = 0;
    refl_h_ = 0;
}

bool VkRayQueryPass::ensure_reflection_target(uint32_t width, uint32_t height) {
    if (refl_image_ != VK_NULL_HANDLE && refl_w_ == static_cast<int>(width) &&
        refl_h_ == static_cast<int>(height)) {
        return true;
    }
    return create_reflection_target(width, height);
}

void VkRayQueryPass::render(VkCommandBuffer cmd,
                            VkAccelerationStructureKHR tlas,
                            VkImageView depth_view,
                            VkImageView lit_view,
                            uint32_t width, uint32_t height,
                            const math::Matrix4f& view_proj,
                            const math::Matrix4f& inv_view_proj,
                            const math::Vector3f& camera_pos,
                            const PostProcessParams& pp) {
    if (!valid_ || cmd == VK_NULL_HANDLE || tlas == VK_NULL_HANDLE) return;
    if (!ensure_reflection_target(width, height)) return;

    // 上传 uniform
    RayQueryUniforms u{};
    u.inv_view_proj = inv_view_proj;
    u.view_proj = view_proj;
    u.cam_pos_max_dist = math::Vector4f(camera_pos.x, camera_pos.y, camera_pos.z,
                                        pp.rt_reflection_max_distance);
    u.resolution_strength =
        math::Vector4f(static_cast<float>(width), static_cast<float>(height),
                       pp.rt_reflection_enabled ? 1.0f : 0.0f, 0.0f);
    ubo_->upload(&u, sizeof(u));

    // 描述符写入（每帧重写，因图像视图随尺寸/帧变化）
    VkDescriptorBufferInfo buf_info{};
    buf_info.buffer = ubo_->buffer();
    buf_info.offset = 0;
    buf_info.range = VK_WHOLE_SIZE;

    VkDescriptorImageInfo depth_info{};
    depth_info.imageView = depth_view;
    depth_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo lit_info{};
    lit_info.imageView = lit_view;
    lit_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo out_info{};
    out_info.imageView = refl_view_;
    out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSetAccelerationStructureKHR accel_write{};
    accel_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    accel_write.accelerationStructureCount = 1;
    accel_write.pAccelerationStructures = &tlas;

    VkWriteDescriptorSet writes[5]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &accel_write;
    writes[0].dstSet = set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &buf_info;

    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set_;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[2].pImageInfo = &depth_info;

    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set_;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[3].pImageInfo = &lit_info;

    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = set_;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].pImageInfo = &out_info;

    vkUpdateDescriptorSets(device_->device(), 5, writes, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);

    const uint32_t gx = (width + 7) / 8;
    const uint32_t gy = (height + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);

    // 存储图写完成 → 准备被后端拷回 HDR 颜色（TRANSFER_SRC）
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.image = refl_image_;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1;
    bar.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);

    tlas_bound_ = true;
}

} // namespace gryce_engine::render
