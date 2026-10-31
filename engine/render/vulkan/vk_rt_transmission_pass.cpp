// vk_rt_transmission_pass.cpp — Vulkan RT 折射 / 透射（P1）实现
//
// 单次派发的计算通道：主视线 ray query 取命中实例的材质 → Snell 折射 →
// 追踪折射光线取背景（屏幕内取 HDR、屏幕外取环境）→ Beer-Lambert 吸收 →
// 混合回全分辨率输出，由后端拷回 HDR。
// 设计说明见头文件；与反射通道共用 TLAS 与 MatParams SSBO。
#include "vk_rt_transmission_pass.h"

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

#include <cstring>
#include <string>
#include <vector>

#include "render/glsl_compiler.h"
#include "render/vulkan/vk_device.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

// 内联折射计算着色器（GLSL，Vulkan 语义 + GL_EXT_ray_query）。
// 主视线命中后按 instanceCustomIndex 索引 MatParams 取得该表面材质；
// transmission<=0 的像素直接 passthrough。
static const char* k_transmission_glsl = R"GLSL(
#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) uniform accelerationStructureEXT uTlas;
layout(std140, binding = 1) uniform UBO {
    mat4 invViewProj;
    mat4 viewProj;
    mat4 prevViewProj;
    vec4 camPosMaxDist;       // xyz = 相机世界坐标, w = 最大追踪距离
    vec4 resolutionStrength;  // xy = 派发分辨率, z = 透射强度, w = 保留
    vec4 envParams;           // x = 深度纹素缩放, y = prefilter 最大 mip,
                              // z = 环境强度(0 = 无环境), w = 帧序号
    vec4 temporalParams;      // 本通道不使用（占位，布局与反射通道一致）
};
layout(binding = 2) uniform texture2D uDepth;    // 全分辨率 HDR 深度
layout(binding = 3) uniform texture2D uLit;      // 全分辨率 HDR 颜色
layout(binding = 4, rgba16f) uniform image2D uOut;
layout(binding = 5) uniform samplerCube uEnv;
layout(binding = 6) uniform samplerCube uPrefilter;
// a=(roughness, metallic, transmission, ior)
// b=(attenuation_color.rgb, attenuation_distance)
// c=(thickness, 0, 0, 0)
// d=(albedo.rgb, 0) — P2 GI 用
struct MatParam { vec4 a; vec4 b; vec4 c; vec4 d; };
layout(std430, binding = 7) readonly buffer MatParams { MatParam mat_params[]; };
layout(binding = 13) uniform texture2D uNormal;

// 手写双线性：对无 sampler 的 sampled image 做 4 tap 插值，避免块状取色。
vec4 bilinear(texture2D tex, vec2 coord, ivec2 size) {
    vec2 c = coord - 0.5;
    ivec2 base = ivec2(floor(c));
    vec2 f = c - vec2(base);
    ivec2 p0 = clamp(base, ivec2(0), size - ivec2(1));
    ivec2 p1 = clamp(base + ivec2(1, 0), ivec2(0), size - ivec2(1));
    ivec2 p2 = clamp(base + ivec2(0, 1), ivec2(0), size - ivec2(1));
    ivec2 p3 = clamp(base + ivec2(1, 1), ivec2(0), size - ivec2(1));
    vec4 s00 = texelFetch(tex, p0, 0);
    vec4 s10 = texelFetch(tex, p1, 0);
    vec4 s01 = texelFetch(tex, p2, 0);
    vec4 s11 = texelFetch(tex, p3, 0);
    return mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
}

void main() {
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = ivec2(imageSize(uOut));
    if (gid.x >= size.x || gid.y >= size.y) return;

    vec4 lit = texelFetch(uLit, gid, 0);
    float depth = texelFetch(uDepth, gid, 0).r;
    if (depth >= 1.0) {              // 背景/天空：无可折射表面
        imageStore(uOut, gid, lit);
        return;
    }

    vec3 camPos = camPosMaxDist.xyz;
    float maxDist = camPosMaxDist.w;

    // 主视线：远平面重建方向
    vec2 uv = (vec2(gid) + 0.5) / vec2(size);
    vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0, 1.0);
    vec4 w = invViewProj * clip;
    vec3 dir = normalize(w.xyz / w.w - camPos);

    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTlas, 0, 0xFF, camPos, 0.0, dir, maxDist);
    while (rayQueryProceedEXT(rq)) { }
    if (rayQueryGetIntersectionTypeEXT(rq, true) ==
        gl_RayQueryCommittedIntersectionNoneEXT) {
        imageStore(uOut, gid, lit);   // 主视线未命中：原样输出
        return;
    }

    MatParam mp = mat_params[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)];
    float transmission = mp.a.z;
    if (transmission <= 0.0) {       // 非透射材质：原样输出
        imageStore(uOut, gid, lit);
        return;
    }

    float ior = max(mp.a.w, 1.0e-4);
    vec3 n;
    if (resolutionStrength.w > 0.5) {
        // 深度+法线预通道：RGB=世界法线 [0,1]（与反射通道同源），
        // 预通道逐像素对应主视线命中面，正好是本通道要折射的表面。
        n = normalize(texelFetch(uNormal, gid, 0).rgb * 2.0 - 1.0);
    } else {
        n = -dir;   // 无预通道的退化兜底：按面向相机的平面处理
    }
    if (dot(n, dir) > 0.0) n = -n;   // 法线朝向入射侧

    vec3 hitPos = camPos + dir * rayQueryGetIntersectionTEXT(rq, true);
    vec3 refr = refract(dir, n, 1.0 / ior);
    if (dot(refr, refr) < 1.0e-8) refr = reflect(dir, n);   // 全内反射兜底

    vec3 origin2 = hitPos + refr * 1.0e-3;
    rayQueryEXT rq2;
    rayQueryInitializeEXT(rq2, uTlas, 0, 0xFF, origin2, 0.0, refr, maxDist);
    while (rayQueryProceedEXT(rq2)) { }

    vec3 behind;
    float travel = 0.0;
    if (rayQueryGetIntersectionTypeEXT(rq2, true) ==
        gl_RayQueryCommittedIntersectionNoneEXT) {
        behind = textureLod(uEnv, refr, 0.0).rgb * envParams.z;
    } else {
        travel = rayQueryGetIntersectionTEXT(rq2, true);
        vec3 p2 = origin2 + refr * travel;
        vec4 clip2 = viewProj * vec4(p2, 1.0);
        if (clip2.w <= 0.0) {
            behind = textureLod(uEnv, refr, 0.0).rgb * envParams.z;
        } else {
            vec2 ndc = clip2.xy / clip2.w;
            vec2 uv2 = vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
            behind = bilinear(uLit, uv2 * vec2(size), size).rgb;
        }
    }

    // Beer-Lambert 吸收：thickness>0 用薄壁厚度，否则用折射光线实际行程。
    float d = (mp.c.x > 0.0) ? mp.c.x : max(travel, 0.0);
    vec3 att = clamp(mp.b.rgb, vec3(1.0e-4), vec3(1.0));
    float dist = max(mp.b.w, 1.0e-4);
    vec3 sigma = -log(att) / dist;
    vec3 absorb = exp(-sigma * d);

    float k = clamp(transmission, 0.0, 1.0) * resolutionStrength.z;
    imageStore(uOut, gid, vec4(mix(lit.rgb, behind * absorb, k), lit.a));
}
)GLSL";

bool VkRTTransmissionPass::init(VulkanDevice* device) {
    if (valid_) return true;
    if (!device || !device->is_valid()) return false;
    device_ = device;
    VkDevice dev = device->device();

    std::vector<uint32_t> spirv;
    std::string err;
    if (!compile_glsl_to_spirv(k_transmission_glsl, "rt_transmission.comp", GlslStage::Compute,
                               spirv, err)) {
        GLOG_WARN("VkRTTransmissionPass: GLSL compile failed ({}); transmission disabled", err);
        return false;
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv.size() * sizeof(uint32_t);
    module_info.pCode = spirv.data();
    if (vkCreateShaderModule(dev, &module_info, nullptr, &module_) != VK_SUCCESS) {
        GLOG_ERROR("VkRTTransmissionPass: vkCreateShaderModule failed");
        return false;
    }

    if (fallback_sampler_ == VK_NULL_HANDLE) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_LINEAR;
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(dev, &si, nullptr, &fallback_sampler_) != VK_SUCCESS) {
            GLOG_WARN("VkRTTransmissionPass: fallback sampler creation failed");
            fallback_sampler_ = VK_NULL_HANDLE;
        }
    }

    // 描述符布局：TLAS / uniform / 深度 / HDR / 存储输出 / 环境 cube /
    // prefilter cube / 材质参数 SSBO / 法线图。
    struct BindingDesc {
        uint32_t binding;
        VkDescriptorType type;
    };
    const BindingDesc k_bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
        {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
        {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
        {13, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    };
    constexpr uint32_t k_binding_count = sizeof(k_bindings) / sizeof(k_bindings[0]);
    VkDescriptorSetLayoutBinding bindings[k_binding_count]{};
    for (uint32_t i = 0; i < k_binding_count; ++i) {
        bindings[i].binding = k_bindings[i].binding;
        bindings[i].descriptorType = k_bindings[i].type;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = k_binding_count;
    set_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(dev, &set_info, nullptr, &desc_layout_) != VK_SUCCESS) {
        GLOG_ERROR("VkRTTransmissionPass: vkCreateDescriptorSetLayout failed");
        return false;
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &desc_layout_;
    if (vkCreatePipelineLayout(dev, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        GLOG_ERROR("VkRTTransmissionPass: vkCreatePipelineLayout failed");
        return false;
    }

    VkComputePipelineCreateInfo pipe_info{};
    pipe_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipe_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipe_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipe_info.stage.module = module_;
    pipe_info.stage.pName = "main";
    pipe_info.layout = layout_;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipe_info, nullptr, &pipeline_) !=
        VK_SUCCESS) {
        GLOG_ERROR("VkRTTransmissionPass: vkCreateComputePipelines failed");
        return false;
    }

    VkDescriptorPoolSize pool_sizes[6]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    pool_sizes[0].descriptorCount = 1;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = 1;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[2].descriptorCount = 3;
    pool_sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[3].descriptorCount = 1;
    pool_sizes[4].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[4].descriptorCount = 2;
    pool_sizes[5].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[5].descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 6;
    pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(dev, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
        GLOG_ERROR("VkRTTransmissionPass: vkCreateDescriptorPool failed");
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool_;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts = &desc_layout_;
    if (vkAllocateDescriptorSets(dev, &alloc_info, &set_) != VK_SUCCESS) {
        GLOG_ERROR("VkRTTransmissionPass: vkAllocateDescriptorSets failed");
        return false;
    }

    for (uint32_t s = 0; s < 3; ++s) {
        ubo_[s] = new VulkanBuffer();
        if (!ubo_[s]->init(device, sizeof(RayQueryUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            GLOG_ERROR("VkRTTransmissionPass: UBO init failed");
            return false;
        }
    }

    valid_ = true;
    return true;
}

void VkRTTransmissionPass::shutdown() {
    if (!device_) return;
    VkDevice dev = device_->device();

    for (uint32_t i = 0; i < 3; ++i) {
        if (ubo_[i]) {
            ubo_[i]->shutdown();
            delete ubo_[i];
            ubo_[i] = nullptr;
        }
    }
    destroy_targets();
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (desc_layout_) vkDestroyDescriptorSetLayout(dev, desc_layout_, nullptr);
    if (module_) vkDestroyShaderModule(dev, module_, nullptr);
    if (fallback_sampler_) vkDestroySampler(dev, fallback_sampler_, nullptr);

    pool_ = VK_NULL_HANDLE;
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    desc_layout_ = VK_NULL_HANDLE;
    module_ = VK_NULL_HANDLE;
    fallback_sampler_ = VK_NULL_HANDLE;
    set_ = VK_NULL_HANDLE;
    device_ = nullptr;
    valid_ = false;
}

bool VkRTTransmissionPass::create_image(VkImage& image, VmaAllocation& alloc,
                                        VkImageView& view, uint32_t width, uint32_t height) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(device_->allocator(), &info, &alloc_info, &image, &alloc, nullptr) !=
        VK_SUCCESS) {
        GLOG_ERROR("VkRTTransmissionPass: output image creation failed");
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_->device(), &view_info, nullptr, &view) != VK_SUCCESS) {
        GLOG_ERROR("VkRTTransmissionPass: output image view creation failed");
        return false;
    }
    return true;
}

void VkRTTransmissionPass::destroy_targets() {
    if (!device_) return;
    VkDevice dev = device_->device();
    if (out_view_) vkDestroyImageView(dev, out_view_, nullptr);
    if (out_image_) vmaDestroyImage(device_->allocator(), out_image_, out_alloc_);
    out_view_ = VK_NULL_HANDLE;
    out_image_ = VK_NULL_HANDLE;
    out_alloc_ = nullptr;
    out_w_ = out_h_ = 0;
    out_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

bool VkRTTransmissionPass::ensure_targets(uint32_t width, uint32_t height) {
    if (out_image_ && out_w_ == static_cast<int>(width) &&
        out_h_ == static_cast<int>(height)) {
        return true;
    }
    destroy_targets();
    if (!create_image(out_image_, out_alloc_, out_view_, width, height)) {
        destroy_targets();
        return false;
    }
    out_w_ = static_cast<int>(width);
    out_h_ = static_cast<int>(height);
    out_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void VkRTTransmissionPass::cmd_transition_image(VkCommandBuffer cmd, VkImage image,
                                                VkImageLayout old_layout,
                                                VkImageLayout new_layout,
                                                VkAccessFlags src_access,
                                                VkAccessFlags dst_access) const {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &barrier);
}

void VkRTTransmissionPass::render(VkCommandBuffer cmd,
                                  VkAccelerationStructureKHR tlas,
                                  VkImageView depth_view, VkImageView lit_view,
                                  VkImageView normal_view,
                                  VkImageView env_view, VkSampler env_sampler,
                                  VkImageView prefilter_view, VkSampler prefilter_sampler,
                                  VkBuffer params_buffer, uint32_t prefilter_mip_levels,
                                  uint32_t width, uint32_t height,
                                  const math::Matrix4f& view_proj,
                                  const math::Matrix4f& inv_view_proj,
                                  const math::Vector3f& camera_pos,
                                  const PostProcessParams& pp,
                                  uint32_t frame_slot) {
    if (!valid_ || !device_) return;
    if (!ensure_targets(width, height)) return;
    VkDevice dev = device_->device();

    RayQueryUniforms u{};
    u.inv_view_proj = inv_view_proj;
    u.view_proj = view_proj;
    u.prev_view_proj = view_proj;  // 本通道不做时域累积，占位保持一致布局
    u.cam_pos_max_dist =
        math::Vector4f(camera_pos.x, camera_pos.y, camera_pos.z,
                       pp.rt_transmission_max_distance > 0.0f ? pp.rt_transmission_max_distance
                                                              : 100.0f);
    u.resolution_strength = math::Vector4f(static_cast<float>(width),
                                           static_cast<float>(height), 1.0f,
                                           normal_view != VK_NULL_HANDLE ? 1.0f : 0.0f);
    // envParams.z = 0 时着色器不产生环境项（无环境贴图或强度为 0）。
    const bool has_env = (env_view != VK_NULL_HANDLE) && pp.env_intensity > 0.0f;
    u.env_params = math::Vector4f(1.0f, static_cast<float>(prefilter_mip_levels),
                                  has_env ? pp.env_intensity : 0.0f, 0.0f);
    u.temporal_params = math::Vector4f(1.0f, 0.0f, 0.0f, 0.0f);
    VulkanBuffer* ubo = ubo_[frame_slot % 3];
    if (!ubo) return;
    ubo->upload(&u, sizeof(u), 0);

    VkDescriptorBufferInfo ubo_info{};
    ubo_info.buffer = ubo->buffer();
    ubo_info.offset = 0;
    ubo_info.range = sizeof(RayQueryUniforms);

    VkWriteDescriptorSetAccelerationStructureKHR as_info{};
    as_info.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    as_info.accelerationStructureCount = 1;
    as_info.pAccelerationStructures = &tlas;

    VkDescriptorImageInfo depth_info{};
    depth_info.imageView = depth_view;
    depth_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo lit_info{};
    lit_info.imageView = lit_view;
    lit_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // 法线槽位：无预通道时用 lit 视图占位（着色器按 resolutionStrength.w 门控不读）。
    VkDescriptorImageInfo normal_info{};
    normal_info.imageView = normal_view != VK_NULL_HANDLE ? normal_view : lit_view;
    normal_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo out_info{};
    out_info.imageView = out_view_;
    out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // 环境槽位：无环境时也要占住组合图像采样器（空视图/空采样器会让驱动在
    // vkCmdDispatch 内崩），用兜底采样器 + 另一张有效的 cube 顶上，着色器按
    // envParams.z == 0 门控不会真采样其结果。
    VkImageView env_bind = env_view != VK_NULL_HANDLE ? env_view : prefilter_view;
    VkImageView prefilter_bind = prefilter_view != VK_NULL_HANDLE ? prefilter_view : env_view;
    VkSampler env_samp = env_sampler != VK_NULL_HANDLE ? env_sampler : fallback_sampler_;
    VkSampler prefilter_samp =
        prefilter_sampler != VK_NULL_HANDLE ? prefilter_sampler : fallback_sampler_;
    VkDescriptorImageInfo env_desc{};
    env_desc.imageView = env_bind;
    env_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    env_desc.sampler = env_samp;
    VkDescriptorImageInfo prefilter_desc{};
    prefilter_desc.imageView = prefilter_bind;
    prefilter_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    prefilter_desc.sampler = prefilter_samp;

    VkDescriptorBufferInfo params_info{};
    params_info.buffer = params_buffer;
    params_info.offset = 0;
    params_info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[9]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &as_info;
    writes[0].dstSet = set_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &ubo_info;
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
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = set_;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].pImageInfo = &env_desc;
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = set_;
    writes[6].dstBinding = 6;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].pImageInfo = &prefilter_desc;
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = set_;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[7].pBufferInfo = &params_info;
    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = set_;
    writes[8].dstBinding = 13;
    writes[8].descriptorCount = 1;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[8].pImageInfo = &normal_info;  // 深度+法线预通道（RGB=世界法线）
    vkUpdateDescriptorSets(dev, 9, writes, 0, nullptr);

    // 输出图：写入需 GENERAL。
    if (out_layout_ != VK_IMAGE_LAYOUT_GENERAL) {
        cmd_transition_image(cmd, out_image_, out_layout_, VK_IMAGE_LAYOUT_GENERAL, 0,
                             VK_ACCESS_SHADER_WRITE_BIT);
        out_layout_ = VK_IMAGE_LAYOUT_GENERAL;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0,
                            nullptr);
    const uint32_t gx = (width + 7) / 8;
    const uint32_t gy = (height + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);
}

} // namespace gryce_engine::render
