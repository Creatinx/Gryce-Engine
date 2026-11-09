// vk_rt_pathtrace_pass.cpp — Vulkan RT 路径追踪 GI（P2）实现
//
// 单派发计算通道：主视线命中后按 bounce 循环做 cosine 加权半球采样的路径
// 追踪，间接光经弹射按 albedo*(1-metallic) 调制；命中面辐照度优先取屏幕
// 投影的 HDR 颜色（深度遮挡校验），出屏/被遮挡回退环境立方体贴图。
// 设计说明见头文件；与折射/反射通道共用 TLAS 与 MatParams SSBO。
#include "vk_rt_pathtrace_pass.h"

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

#include <cstring>
#include <string>
#include <vector>

#include "render/glsl_compiler.h"
#include "render/vulkan/vk_device.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

// 每帧重写整套描述符集，必须与 frames_in_flight 对齐轮转（与 ubo_ 一致）。
static constexpr uint32_t k_frame_slots = 3;

// 内联路径追踪计算着色器（GLSL，Vulkan 语义 + GL_EXT_ray_query）。
// 布局与透射通道一致（binding 0-7、13），便于复用同一套描述符组装代码。
static const char* k_pathtrace_glsl = R"GLSL(
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
    vec4 resolutionStrength;  // xy = 派发分辨率, z = GI 强度, w = 有无法线预通道
    vec4 envParams;           // x = 深度纹素缩放, y = prefilter 最大 mip,
                              // z = 环境强度(0 = 无环境), w = 帧序号
    vec4 temporalParams;      // x = 最大弹射数, y = 每像素采样数, zw = 保留
};
layout(binding = 2) uniform texture2D uDepth;    // 全分辨率 HDR 深度
layout(binding = 3) uniform texture2D uLit;      // 全分辨率 HDR 颜色
layout(binding = 4, rgba16f) uniform image2D uOut;
layout(binding = 5) uniform samplerCube uEnv;
layout(binding = 6) uniform samplerCube uPrefilter;   // 布局占位，GI 不用
// a=(roughness, metallic, transmission, ior)
// b=(attenuation_color.rgb, attenuation_distance)
// c=(thickness, 0, 0, 0)
// d=(albedo.rgb, 0)
struct MatParam { vec4 a; vec4 b; vec4 c; vec4 d; };
layout(std430, binding = 7) readonly buffer MatParams { MatParam mat_params[]; };
layout(binding = 13) uniform texture2D uNormal;  // RGB=世界法线 [0,1]

// PCG 哈希：像素坐标 + 帧序号 + 采样序号 → 32bit 随机流。
uint pcg(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
float rnd(inout uint state) { return float(pcg(state)) / 4294967296.0; }

// cosine 加权半球采样（切线系构造，上方向取绝对值最小轴避免退化）。
vec3 cosine_dir(vec3 n, float r1, float r2) {
    float phi = 6.2831853 * r1;
    float r = sqrt(r2);
    vec3 t = normalize(abs(n.y) < 0.9 ? cross(n, vec3(0, 1, 0))
                                      : cross(n, vec3(1, 0, 0)));
    vec3 b = cross(n, t);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(1.0 - r2));
}

// 由屏幕 uv + 深度反投影出世界坐标（遮挡校验用）。
vec3 reconstructWorld(vec2 uv, float depth) {
    vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    vec4 world = invViewProj * clip;
    return world.xyz / world.w;
}

// 手写双线性（与透射通道一致）。
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
    if (depth >= 1.0) {              // 背景/天空：无间接光入射面
        imageStore(uOut, gid, lit);
        return;
    }

    vec3 camPos = camPosMaxDist.xyz;
    float maxDist = camPosMaxDist.w;
    int bounces = int(temporalParams.x);
    int spp = max(int(temporalParams.y), 1);

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
        imageStore(uOut, gid, lit);
        return;
    }
    MatParam mp = mat_params[rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true)];

    vec3 n;
    if (resolutionStrength.w > 0.5) {
        n = normalize(texelFetch(uNormal, gid, 0).rgb * 2.0 - 1.0);
    } else {
        n = -dir;   // 无预通道的退化兜底
    }
    if (dot(n, dir) > 0.0) n = -n;

    vec3 hitPos = camPos + dir * rayQueryGetIntersectionTEXT(rq, true);

    // 路径追踪：cosine 加权采样（PDF 含 cos/pi，权重直接相加）。
    vec3 gi = vec3(0.0);
    vec3 throughput = mp.d.rgb * (1.0 - clamp(mp.a.y, 0.0, 1.0));
    vec3 p = hitPos + n * 1.0e-3;
    uint seed = uint(gid.x) * 1973u + uint(gid.y) * 9277u +
                uint(envParams.w) * 26699u;
    for (int s = 0; s < spp; ++s) {
        vec3 osp = p;
        vec3 on = n;
        vec3 thr = throughput;
        for (int b = 0; b < bounces; ++b) {
            vec3 d = cosine_dir(on, rnd(seed), rnd(seed));
            rayQueryEXT grq;
            rayQueryInitializeEXT(grq, uTlas, 0, 0xFF, osp, 0.0, d, maxDist);
            while (rayQueryProceedEXT(grq)) { }
            if (rayQueryGetIntersectionTypeEXT(grq, true) ==
                gl_RayQueryCommittedIntersectionNoneEXT) {
                gi += thr * textureLod(uEnv, d, 0.0).rgb * envParams.z;
                break;
            }
            float t2 = rayQueryGetIntersectionTEXT(grq, true);
            vec3 p2 = osp + d * t2;
            MatParam m2 =
                mat_params[rayQueryGetIntersectionInstanceCustomIndexEXT(grq, true)];

            // 命中面辐照度：投影回屏幕取 HDR（深度不符 = 被遮挡，回退环境）。
            vec4 c2 = viewProj * vec4(p2, 1.0);
            vec3 L;
            if (c2.w <= 0.0) {
                L = textureLod(uEnv, d, 0.0).rgb * envParams.z;
            } else {
                vec2 ndc = c2.xy / c2.w;
                vec2 uv2 = vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
                vec2 sp = uv2 * vec2(size);
                ivec2 si = ivec2(clamp(sp, vec2(0.0), vec2(size) - 1.0));
                float dpt = texelFetch(uDepth, si, 0).r;
                // 遮挡校验（修漏光）：弹射命中点投影回屏幕后，该像素必须确实是这个
                // 命中面。被更近的物体挡住时直接采 uLit 会把遮挡物的光照当成间接光
                // —— 表现为 GI"透墙"漏光。比较屏幕深度重建点与命中点到相机的线性
                // 距离，相对差过大即判遮挡，退回环境 cube。
                vec3 scene_w = reconstructWorld(uv2, dpt);
                float scene_lin = length(scene_w - camPos);
                float hit_lin = length(p2 - camPos);
                float rel = abs(scene_lin - hit_lin) / max(hit_lin, 1e-3);
                if (dpt >= 1.0 || rel > 0.06) {
                    L = textureLod(uEnv, d, 0.0).rgb * envParams.z;
                } else {
                    L = bilinear(uLit, sp, size).rgb;
                }
            }
            gi += thr * L;

            // 弹射命中点法线：预通道投影近似（被遮挡时退化为 -d）。
            if (c2.w > 0.0 && resolutionStrength.w > 0.5) {
                vec2 ndc = c2.xy / c2.w;
                ivec2 si = ivec2(clamp(vec2(ndc.x * 0.5 + 0.5,
                                            1.0 - (ndc.y * 0.5 + 0.5)) *
                                       vec2(size), vec2(0.0), vec2(size) - 1.0));
                on = normalize(texelFetch(uNormal, si, 0).rgb * 2.0 - 1.0);
            } else {
                on = -d;
            }
            if (dot(on, d) > 0.0) on = -on;
            thr *= m2.d.rgb * (1.0 - clamp(m2.a.y, 0.0, 1.0));
            if (max(max(thr.r, thr.g), thr.b) < 1.0e-3) break;
            osp = p2 + on * 1.0e-3;
        }
    }
    gi /= float(spp);
    float k = clamp(resolutionStrength.z, 0.0, 4.0);
    imageStore(uOut, gid, vec4(lit.rgb + gi * k, lit.a));
}
)GLSL";

bool VkRTPathTracePass::init(VulkanDevice* device) {
    if (valid_) return true;
    if (!device || !device->is_valid()) return false;
    device_ = device;
    VkDevice dev = device->device();

    std::vector<uint32_t> spirv;
    std::string err;
    if (!compile_glsl_to_spirv(k_pathtrace_glsl, "rt_pathtrace.comp", GlslStage::Compute,
                               spirv, err)) {
        GLOG_WARN("VkRTPathTracePass: GLSL compile failed ({}); path tracing disabled", err);
        return false;
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = spirv.size() * sizeof(uint32_t);
    module_info.pCode = spirv.data();
    if (vkCreateShaderModule(dev, &module_info, nullptr, &module_) != VK_SUCCESS) {
        GLOG_ERROR("VkRTPathTracePass: vkCreateShaderModule failed");
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
            GLOG_WARN("VkRTPathTracePass: fallback sampler creation failed");
            fallback_sampler_ = VK_NULL_HANDLE;
        }
    }

    // 描述符布局与透射通道一致：TLAS / uniform / 深度 / HDR / 存储输出 /
    // 环境 cube / prefilter cube（占位）/ 材质参数 SSBO / 法线图。
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
        GLOG_ERROR("VkRTPathTracePass: vkCreateDescriptorSetLayout failed");
        return false;
    }

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &desc_layout_;
    if (vkCreatePipelineLayout(dev, &layout_info, nullptr, &layout_) != VK_SUCCESS) {
        GLOG_ERROR("VkRTPathTracePass: vkCreatePipelineLayout failed");
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
        GLOG_ERROR("VkRTPathTracePass: vkCreateComputePipelines failed");
        return false;
    }

    VkDescriptorPoolSize pool_sizes[6]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    pool_sizes[0].descriptorCount = k_frame_slots;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = k_frame_slots;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[2].descriptorCount = 3 * k_frame_slots;
    pool_sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[3].descriptorCount = k_frame_slots;
    pool_sizes[4].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[4].descriptorCount = 2 * k_frame_slots;
    pool_sizes[5].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[5].descriptorCount = k_frame_slots;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = k_frame_slots;
    pool_info.poolSizeCount = 6;
    pool_info.pPoolSizes = pool_sizes;
    if (vkCreateDescriptorPool(dev, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
        GLOG_ERROR("VkRTPathTracePass: vkCreateDescriptorPool failed");
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.descriptorPool = pool_;
    alloc_info.descriptorSetCount = k_frame_slots;
    // pSetLayouts 是数组：每套 set 都要一个 layout 句柄，只给单指针会让后两套
    // 读到栈上的垃圾（验证层报 Invalid VkDescriptorSetLayout / VK_NULL_HANDLE）。
    VkDescriptorSetLayout set_layouts[k_frame_slots] = {desc_layout_, desc_layout_, desc_layout_};
    alloc_info.pSetLayouts = set_layouts;
    if (vkAllocateDescriptorSets(dev, &alloc_info, set_) != VK_SUCCESS) {
        GLOG_ERROR("VkRTPathTracePass: vkAllocateDescriptorSets failed");
        return false;
    }

    for (uint32_t s = 0; s < 3; ++s) {
        ubo_[s] = new VulkanBuffer();
        if (!ubo_[s]->init(device, sizeof(RayQueryUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            GLOG_ERROR("VkRTPathTracePass: UBO init failed");
            return false;
        }
    }

    valid_ = true;
    return true;
}

void VkRTPathTracePass::shutdown() {
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
    set_[0] = VK_NULL_HANDLE;
    set_[1] = VK_NULL_HANDLE;
    set_[2] = VK_NULL_HANDLE;
    device_ = nullptr;
    valid_ = false;
}

bool VkRTPathTracePass::create_image(VkImage& image, VmaAllocation& alloc,
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
        GLOG_ERROR("VkRTPathTracePass: output image creation failed");
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
        GLOG_ERROR("VkRTPathTracePass: output image view creation failed");
        return false;
    }
    return true;
}

void VkRTPathTracePass::destroy_targets() {
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

bool VkRTPathTracePass::ensure_targets(uint32_t width, uint32_t height) {
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

void VkRTPathTracePass::cmd_transition_image(VkCommandBuffer cmd, VkImage image,
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

void VkRTPathTracePass::render(VkCommandBuffer cmd,
                               VkAccelerationStructureKHR tlas,
                               VkImageView depth_view, VkImageView lit_view,
                               VkImageView normal_view,
                               VkImageView env_view, VkSampler env_sampler,
                               VkBuffer params_buffer,
                               uint32_t width, uint32_t height,
                               const math::Matrix4f& view_proj,
                               const math::Matrix4f& inv_view_proj,
                               const math::Vector3f& camera_pos,
                               const PostProcessParams& pp,
                               uint32_t frame_slot) {
    if (!valid_ || !device_) return;
    if (!ensure_targets(width, height)) return;
    // GI 采样槽需要有效 cube 视图（空组合图像采样器进 dispatch 会崩）；完全无
    // IBL 的场景直接跳过本通道（间接光无来源，GI 本身也无意义）。
    if (env_view == VK_NULL_HANDLE) return;
    VkDevice dev = device_->device();

    static int frame_index = 0;  // 递增帧序号：给 RNG 每帧换种子（后续时域累积可复用）
    RayQueryUniforms u{};
    u.inv_view_proj = inv_view_proj;
    u.view_proj = view_proj;
    u.prev_view_proj = view_proj;  // 暂不做时域重投影，占位保持一致布局
    u.cam_pos_max_dist =
        math::Vector4f(camera_pos.x, camera_pos.y, camera_pos.z,
                       pp.rt_reflection_max_distance > 0.0f ? pp.rt_reflection_max_distance
                                                            : 100.0f);
    u.resolution_strength = math::Vector4f(static_cast<float>(width),
                                           static_cast<float>(height), 1.0f,
                                           normal_view != VK_NULL_HANDLE ? 1.0f : 0.0f);
    const bool has_env = (env_view != VK_NULL_HANDLE) && pp.env_intensity > 0.0f;
    u.env_params = math::Vector4f(1.0f, 0.0f, has_env ? pp.env_intensity : 0.0f,
                                  static_cast<float>(frame_index++));
    u.temporal_params = math::Vector4f(2.0f, 1.0f, 0.0f, 0.0f);  // 2 弹射, 1 spp
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
    VkDescriptorImageInfo normal_info{};
    normal_info.imageView = normal_view != VK_NULL_HANDLE ? normal_view : lit_view;
    normal_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo out_info{};
    out_info.imageView = out_view_;
    out_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    // 环境槽位：无环境贴图时用兜底采样器顶住（与透射通道同策略，envParams.z=0 门控）。
    VkSampler env_samp = env_sampler != VK_NULL_HANDLE ? env_sampler : fallback_sampler_;
    VkImageView env_bind = env_view;   // 调用方保证非空（见上方门控）
    VkDescriptorImageInfo env_desc{};
    env_desc.imageView = env_bind;
    env_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    env_desc.sampler = env_samp;
    VkDescriptorImageInfo prefilter_desc = env_desc;

    VkDescriptorBufferInfo params_info{};
    params_info.buffer = params_buffer;
    params_info.offset = 0;
    params_info.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet writes[9]{};
    // 按帧槽取用独立描述符集：上一帧的 set 可能仍被在飞命令缓冲引用。
    VkDescriptorSet set = set_[frame_slot % k_frame_slots];
    if (set == VK_NULL_HANDLE) return;
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &as_info;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &ubo_info;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[2].pImageInfo = &depth_info;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = set;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[3].pImageInfo = &lit_info;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = set;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].pImageInfo = &out_info;
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = set;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].pImageInfo = &env_desc;
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = set;
    writes[6].dstBinding = 6;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].pImageInfo = &prefilter_desc;
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = set;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[7].pBufferInfo = &params_info;
    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = set;
    writes[8].dstBinding = 13;
    writes[8].descriptorCount = 1;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[8].pImageInfo = &normal_info;
    vkUpdateDescriptorSets(dev, 9, writes, 0, nullptr);

    if (out_layout_ != VK_IMAGE_LAYOUT_GENERAL) {
        cmd_transition_image(cmd, out_image_, out_layout_, VK_IMAGE_LAYOUT_GENERAL, 0,
                             VK_ACCESS_SHADER_WRITE_BIT);
        out_layout_ = VK_IMAGE_LAYOUT_GENERAL;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0,
                            nullptr);
    const uint32_t gx = (width + 7) / 8;
    const uint32_t gy = (height + 7) / 8;
    vkCmdDispatch(cmd, gx, gy, 1);
}

} // namespace gryce_engine::render
