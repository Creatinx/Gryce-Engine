// vk_ray_query_pass.cpp — Vulkan 硬件光追反射（P2.2，ray query）实现
//
// 通道自包含：编译内联 GLSL → SPIR-V、建计算管线、描述符集、uniform 缓冲与
// 内部目标图。render() 在给定命令缓冲上做两段派发：
//   trace（半分辨率）：逐像素反射射线，输出 (反射色, 混合系数)；
//   composite（全分辨率）：双线性上采样 trace 结果并与 HDR 颜色混合。
// 光线数按分辨率档平方下降，画面其余部分保持全分辨率锐度；TLAS 构建命令由
// 后端先记录进同一命令缓冲，本通道只消费（无主机端同步）。
#include "vk_ray_query_pass.h"

#include <vulkan/vulkan.h>
#include <vma/vk_mem_alloc.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "render/glsl_compiler.h"
#include "render/vulkan/vk_device.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

// 内联 ray query 计算着色器（GLSL，Vulkan 语义 + GL_EXT_ray_query）。
// 同一着色器包含两个模式（UBO 分支）：
//   模式 0 trace —— 从深度重建世界坐标与法线，沿反射方向做 ray query；命中按
//   实例材质（roughness/metallic，经 instanceCustomIndex 索引 SSBO）做菲涅尔
//   衰减，光滑命中重投影回屏幕双线性采样 HDR，粗糙命中走 prefilter 环境 mip，
//   未命中采样环境立方体贴图；输出 (反射色 RGB, 混合系数 A)。
//   模式 1 composite —— 双线性上采样 trace 结果，与全分辨率 HDR 颜色混合。
static const char* k_ray_query_glsl = R"GLSL(
#version 460
#extension GL_EXT_ray_query : require
// 直接对 texture2D 做 texelFetch（不带 sampler）需要此扩展；缺了它 shaderc 会
// 报 "required extension not requested: GL_EXT_samplerless_texture_functions"。
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(binding = 0) uniform accelerationStructureEXT uTlas;
layout(std140, binding = 1) uniform UBO {
    mat4 invViewProj;
    mat4 viewProj;
    vec4 camPosMaxDist;         // xyz = 相机世界坐标, w = 最大追踪距离
    vec4 resolutionStrength;    // xy = 本段派发分辨率, z = 反射强度, w = 模式(0 trace / 1 composite)
    vec4 envParams;             // x = 深度纹素缩放(全/半), y = prefilter 最大 mip,
                                // z = 环境强度(0 = 无环境), w = 保留
};
layout(binding = 2) uniform texture2D uDepth;
layout(binding = 3) uniform texture2D uLit;
layout(binding = 4, rgba16f) uniform image2D uOut;
layout(binding = 5) uniform samplerCube uEnv;
layout(binding = 6) uniform samplerCube uPrefilter;
layout(std430, binding = 7) readonly buffer MatParams { vec2 mat_params[]; };
layout(binding = 8) uniform texture2D uRefl;

vec3 reconstructWorld(vec2 uv, float depth) {
    vec4 clip = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    vec4 world = invViewProj * clip;
    return world.xyz / world.w;
}

// 手写双线性：对无 sampler 的 sampled image 做 4 tap 插值，消除最近邻取色的块状感。
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
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    ivec2 res = ivec2(resolutionStrength.xy);
    if (px.x >= res.x || px.y >= res.y) return;
    float strength = resolutionStrength.z;

    if (resolutionStrength.w < 0.5) {
        // ---- 模式 0：trace（半分辨率光线查询）----
        ivec2 fullRes = ivec2(vec2(res) * envParams.x + 0.5);
        int si = int(envParams.x + 0.5);
        vec2 uv = (vec2(px) + 0.5) / vec2(res);

        // 深度按缩放映射到全分辨率纹素；法线邻域取相邻 trace 纹素（= si 个全纹素），
        // 倾斜面上比逐像素差分更平滑。
        ivec2 dp = min(ivec2(vec2(px) * envParams.x), fullRes - ivec2(1));
        float depth = texelFetch(uDepth, dp, 0).r;
        // 远处（天空 / 远平面）不产生反射：系数 0，composite 时保留原色。
        if (depth >= 1.0) {
            imageStore(uOut, px, vec4(0.0, 0.0, 0.0, 0.0));
            return;
        }

        vec3 world = reconstructWorld(uv, depth);
        ivec2 dpR = min(dp + ivec2(si, 0), fullRes - ivec2(1));
        ivec2 dpU = min(dp + ivec2(0, si), fullRes - ivec2(1));
        float dR = texelFetch(uDepth, dpR, 0).r;
        float dU = texelFetch(uDepth, dpU, 0).r;
        vec2 uvR = (vec2(px + ivec2(1, 0)) + 0.5) / vec2(res);
        vec2 uvU = (vec2(px + ivec2(0, 1)) + 0.5) / vec2(res);
        vec3 wR = reconstructWorld(uvR, dR);
        vec3 wU = reconstructWorld(uvU, dU);
        vec3 n = normalize(cross(wR - world, wU - world));
        vec3 vdir = normalize(world - camPosMaxDist.xyz);
        // 法线一律朝向相机：重建出的叉积方向取决于屏幕坐标手性，不统一的话
        // 反射射线会扎进表面自相交，命中恒为自身、反射等于原色，等于没效果。
        if (dot(n, vdir) > 0.0) n = -n;
        vec3 rdir = reflect(vdir, n);

        // 沿法线外推一点起点，避免与发射像素所在三角形自相交。
        vec3 origin = world + n * 0.02;
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, uTlas, gl_RayFlagsOpaqueEXT, 0xFF,
                              origin, 0.02, rdir, camPosMaxDist.w);
        while (rayQueryProceedEXT(rq)) {}

        vec3 reflCol = vec3(0.0);
        float k = 0.0;
        if (rayQueryGetIntersectionTypeEXT(rq, true) !=
            gl_RayQueryCommittedIntersectionNoneEXT) {
            float t = rayQueryGetIntersectionTEXT(rq, true);
            vec3 hit = origin + rdir * t;

            // 命中实例的材质参数（instanceCustomIndex 由后端填实例序号）：
            // roughness 决定反射强度衰减与环境模糊档位，metallic 决定菲涅尔基准。
            uint idx = rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true);
            vec2 mp = mat_params[idx];
            float rough = clamp(mp.x, 0.0, 1.0);
            float metal = clamp(mp.y, 0.0, 1.0);

            // 菲涅尔-史利克：掠射角反射增强；绝缘体基准 0.04，金属趋近 1。
            float ndv = clamp(dot(n, -vdir), 0.0, 1.0);
            float f0 = mix(0.04, 1.0, metal);
            float fres = f0 + (1.0 - f0) * pow(1.0 - ndv, 5.0);
            k = clamp(strength * fres * (1.0 - rough * 0.75), 0.0, 1.0);

            // 反射内容三选一：光滑命中 → 屏幕空间 HDR（真实场景色）；
            // 粗糙命中 → prefilter 环境 mip（模糊环境光）；命中点在屏幕外 → 环境。
            vec3 envHit = vec3(0.0);
            if (envParams.z > 0.0) {
                envHit = textureLod(uPrefilter, rdir, rough * envParams.y).rgb;
            }
            vec3 ssCol = envHit;
            vec4 hc = viewProj * vec4(hit, 1.0);
            if (hc.w > 0.0) {
                vec2 huv = (hc.xy / hc.w) * 0.5 + 0.5;
                huv.y = 1.0 - huv.y;
                if (huv.x >= 0.0 && huv.x <= 1.0 && huv.y >= 0.0 && huv.y <= 1.0) {
                    vec3 ss = bilinear(uLit, huv * vec2(fullRes), fullRes).rgb;
                    // 越粗糙越信任环境贴图（屏幕空间只有一个平面的信息）
                    ssCol = mix(envHit, ss, smoothstep(0.15, 0.45, rough));
                }
            }
            reflCol = ssCol;
        } else if (envParams.z > 0.0) {
            // 未命中 = 反射方向射向环境（天空）：直接采样环境立方体贴图。
            // 这是屏幕空间方案永远给不出的内容（P2.3 环境反射）。
            reflCol = texture(uEnv, rdir).rgb;
            k = clamp(strength * 0.9, 0.0, 1.0);
        }
        imageStore(uOut, px, vec4(reflCol, k));
    } else {
        // ---- 模式 1：composite（全分辨率合成）----
        vec2 uv = (vec2(px) + 0.5) / vec2(res);
        ivec2 tRes = textureSize(uRefl, 0);
        vec4 r = bilinear(uRefl, uv * vec2(tRes), tRes);
        vec3 lit = texelFetch(uLit, px, 0).rgb;
        imageStore(uOut, px, vec4(mix(lit, r.rgb, r.a), 1.0));
    }
}
)GLSL";

VkRayQueryPass::VkRayQueryPass(VkRayQueryPass&& other) noexcept { *this = std::move(other); }

VkRayQueryPass& VkRayQueryPass::operator=(VkRayQueryPass&& other) noexcept {
    if (this != &other) {
        device_ = other.device_;
        valid_ = other.valid_;
        module_ = other.module_;
        layout_ = other.layout_;
        trace_pipeline_ = other.trace_pipeline_;
        composite_pipeline_ = other.composite_pipeline_;
        desc_layout_ = other.desc_layout_;
        pool_ = other.pool_;
        trace_set_ = other.trace_set_;
        composite_set_ = other.composite_set_;
        ubo_ = other.ubo_;
        one_shot_pool_ = other.one_shot_pool_;
        refl_image_ = other.refl_image_;
        refl_alloc_ = other.refl_alloc_;
        refl_view_ = other.refl_view_;
        refl_w_ = other.refl_w_;
        refl_h_ = other.refl_h_;
        refl_layout_ = other.refl_layout_;
        out_image_ = other.out_image_;
        out_alloc_ = other.out_alloc_;
        out_view_ = other.out_view_;
        out_w_ = other.out_w_;
        out_h_ = other.out_h_;
        out_layout_ = other.out_layout_;

        other.device_ = nullptr;
        other.valid_ = false;
        other.module_ = VK_NULL_HANDLE;
        other.layout_ = VK_NULL_HANDLE;
        other.trace_pipeline_ = VK_NULL_HANDLE;
        other.composite_pipeline_ = VK_NULL_HANDLE;
        other.desc_layout_ = VK_NULL_HANDLE;
        other.pool_ = VK_NULL_HANDLE;
        other.trace_set_ = VK_NULL_HANDLE;
        other.composite_set_ = VK_NULL_HANDLE;
        other.ubo_ = nullptr;
        other.one_shot_pool_ = VK_NULL_HANDLE;
        other.refl_image_ = VK_NULL_HANDLE;
        other.refl_alloc_ = nullptr;
        other.refl_view_ = VK_NULL_HANDLE;
        other.refl_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        other.out_image_ = VK_NULL_HANDLE;
        other.out_alloc_ = nullptr;
        other.out_view_ = VK_NULL_HANDLE;
        other.out_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
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

    // 描述符集布局：TLAS / uniform / 深度 / HDR / 存储输出 / 环境 cube /
    // prefilter cube / 材质参数 SSBO / trace 结果采样。
    VkDescriptorSetLayoutBinding bindings[9]{};
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
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[7].binding = 7;
    bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[7].descriptorCount = 1;
    bindings[7].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[8].binding = 8;
    bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bindings[8].descriptorCount = 1;
    bindings[8].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = 9;
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

    // 两条计算管线共用同一 shader module 与布局，仅 uniform 的模式位不同：
    // trace（光线查询）与 composite（全分辨率合成）。
    const auto make_pipeline = [&](VkPipeline& out) -> bool {
        VkComputePipelineCreateInfo pipe_info{};
        pipe_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipe_info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipe_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipe_info.stage.module = module_;
        pipe_info.stage.pName = "main";
        pipe_info.layout = layout_;
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pipe_info, nullptr, &out) !=
            VK_SUCCESS) {
            GLOG_ERROR("VkRayQueryPass: vkCreateComputePipelines failed");
            return false;
        }
        return true;
    };
    if (!make_pipeline(trace_pipeline_) || !make_pipeline(composite_pipeline_)) return false;

    // 双槽位描述符集：槽 0 供 trace（uOut = trace 结果图），槽 1 供 composite
    //（uOut = 全分辨率输出图，uRefl = trace 结果图）。
    constexpr uint32_t k_slots = 2;
    VkDescriptorPoolSize pool_sizes[6]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    pool_sizes[0].descriptorCount = k_slots;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = k_slots;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[2].descriptorCount = k_slots * 3;
    pool_sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[3].descriptorCount = k_slots;
    pool_sizes[4].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_sizes[4].descriptorCount = k_slots * 2;
    pool_sizes[5].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    pool_sizes[5].descriptorCount = k_slots;

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = k_slots;
    pool_info.poolSizeCount = 6;
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
    trace_set_ = sets[0];
    composite_set_ = sets[1];

    // uniform 缓冲双区域（槽 0 = trace 参数，槽 1 = composite 参数）。
    // 区域步长必须按 minUniformBufferOffsetAlignment 取整（NVIDIA 桌面为 256）：
    // 结构体本身只有 176 字节，直接拿 176 当偏移会造出未对齐描述符 —— 描述符
    // 非法时驱动静默返回全零，composite 一进来就被 `px >= res` 早退，输出图
    // 恒为未写入的黑。HOST_VISIBLE + COHERENT，每帧 memcpy 上传。
    constexpr VkDeviceSize k_ubo_stride = 256;
    static_assert(sizeof(RayQueryUniforms) <= k_ubo_stride,
                  "RayQueryUniforms exceeded UBO region stride");
    ubo_ = new VulkanBuffer();
    if (!ubo_->init(device, k_ubo_stride * 2,
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        GLOG_ERROR("VkRayQueryPass: uniform buffer init failed");
        return false;
    }

    // 一次性命令池（仅用于目标图创建时的初始布局转置）
    VkCommandPoolCreateInfo cp_info{};
    cp_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp_info.queueFamilyIndex = device->graphics_queue_family();
    cp_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (vkCreateCommandPool(dev, &cp_info, nullptr, &one_shot_pool_) != VK_SUCCESS) {
        GLOG_ERROR("VkRayQueryPass: vkCreateCommandPool failed");
        return false;
    }

    valid_ = true;
    GLOG_INFO("VkRayQueryPass: ray-query reflection pipelines ready (trace + composite)");
    return true;
}

void VkRayQueryPass::shutdown() {
    if (!device_ || !device_->is_valid()) {
        device_ = nullptr;
        valid_ = false;
        module_ = VK_NULL_HANDLE;
        layout_ = VK_NULL_HANDLE;
        trace_pipeline_ = VK_NULL_HANDLE;
        composite_pipeline_ = VK_NULL_HANDLE;
        desc_layout_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE;
        trace_set_ = VK_NULL_HANDLE;
        composite_set_ = VK_NULL_HANDLE;
        ubo_ = nullptr;
        one_shot_pool_ = VK_NULL_HANDLE;
        refl_image_ = VK_NULL_HANDLE;
        refl_alloc_ = nullptr;
        refl_view_ = VK_NULL_HANDLE;
        out_image_ = VK_NULL_HANDLE;
        out_alloc_ = nullptr;
        out_view_ = VK_NULL_HANDLE;
        return;
    }
    VkDevice dev = device_->device();
    if (trace_pipeline_) vkDestroyPipeline(dev, trace_pipeline_, nullptr);
    if (composite_pipeline_) vkDestroyPipeline(dev, composite_pipeline_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (desc_layout_) vkDestroyDescriptorSetLayout(dev, desc_layout_, nullptr);
    if (module_) vkDestroyShaderModule(dev, module_, nullptr);
    if (one_shot_pool_) vkDestroyCommandPool(dev, one_shot_pool_, nullptr);
    if (ubo_) { ubo_->shutdown(); delete ubo_; ubo_ = nullptr; }
    destroy_targets();
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

void VkRayQueryPass::cmd_transition_image(VkCommandBuffer cmd, VkImage image,
                                          VkImageLayout old_layout, VkImageLayout new_layout,
                                          VkAccessFlags src_access,
                                          VkAccessFlags dst_access) const {
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = old_layout;
    bar.newLayout = new_layout;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.srcAccessMask = src_access;
    bar.dstAccessMask = dst_access;
    bar.image = image;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1;
    bar.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
}

bool VkRayQueryPass::create_image(VkImage& image, VmaAllocation& alloc, VkImageView& view,
                                  uint32_t width, uint32_t height,
                                  VkImageUsageFlags extra_usage) {
    VkDevice dev = device_->device();

    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    info.extent.width = width;
    info.extent.height = height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | extra_usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(device_->allocator(), &info, &alloc_info, &image, &alloc,
                       nullptr) != VK_SUCCESS) {
        GLOG_ERROR("VkRayQueryPass: vmaCreateImage failed");
        return false;
    }

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(dev, &view_info, nullptr, &view) != VK_SUCCESS) {
        GLOG_ERROR("VkRayQueryPass: vkCreateImageView failed");
        return false;
    }
    return true;
}

bool VkRayQueryPass::create_targets(uint32_t width, uint32_t height, uint32_t trace_width,
                                    uint32_t trace_height) {
    destroy_targets();
    if (!create_image(refl_image_, refl_alloc_, refl_view_, trace_width, trace_height,
                      VK_IMAGE_USAGE_SAMPLED_BIT)) {
        return false;
    }
    if (!create_image(out_image_, out_alloc_, out_view_, width, height,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    // 初始布局：UNDEFINED → GENERAL（存储图可直接写入）
    transition_image(refl_image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    refl_layout_ = VK_IMAGE_LAYOUT_GENERAL;
    transition_image(out_image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                    0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    out_layout_ = VK_IMAGE_LAYOUT_GENERAL;

    refl_w_ = static_cast<int>(trace_width);
    refl_h_ = static_cast<int>(trace_height);
    out_w_ = static_cast<int>(width);
    out_h_ = static_cast<int>(height);
    return true;
}

void VkRayQueryPass::destroy_targets() {
    if (!device_ || !device_->is_valid()) {
        refl_image_ = VK_NULL_HANDLE;
        refl_alloc_ = nullptr;
        refl_view_ = VK_NULL_HANDLE;
        out_image_ = VK_NULL_HANDLE;
        out_alloc_ = nullptr;
        out_view_ = VK_NULL_HANDLE;
        refl_w_ = refl_h_ = out_w_ = out_h_ = 0;
        refl_layout_ = out_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        return;
    }
    VkDevice dev = device_->device();
    if (refl_view_) vkDestroyImageView(dev, refl_view_, nullptr);
    if (refl_image_) vmaDestroyImage(device_->allocator(), refl_image_, refl_alloc_);
    if (out_view_) vkDestroyImageView(dev, out_view_, nullptr);
    if (out_image_) vmaDestroyImage(device_->allocator(), out_image_, out_alloc_);
    refl_image_ = VK_NULL_HANDLE;
    refl_alloc_ = nullptr;
    refl_view_ = VK_NULL_HANDLE;
    out_image_ = VK_NULL_HANDLE;
    out_alloc_ = nullptr;
    out_view_ = VK_NULL_HANDLE;
    refl_w_ = refl_h_ = out_w_ = out_h_ = 0;
    refl_layout_ = out_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

bool VkRayQueryPass::ensure_targets(uint32_t width, uint32_t height, uint32_t trace_width,
                                    uint32_t trace_height) {
    if (refl_image_ != VK_NULL_HANDLE && out_image_ != VK_NULL_HANDLE &&
        out_w_ == static_cast<int>(width) && out_h_ == static_cast<int>(height) &&
        refl_w_ == static_cast<int>(trace_width) &&
        refl_h_ == static_cast<int>(trace_height)) {
        return true;
    }
    return create_targets(width, height, trace_width, trace_height);
}

void VkRayQueryPass::render(VkCommandBuffer cmd,
                            VkAccelerationStructureKHR tlas,
                            VkImageView depth_view,
                            VkImageView lit_view,
                            VkImageView env_view, VkSampler env_sampler,
                            VkImageView prefilter_view, VkSampler prefilter_sampler,
                            VkBuffer params_buffer, uint32_t prefilter_mip_levels,
                            uint32_t width, uint32_t height,
                            const math::Matrix4f& view_proj,
                            const math::Matrix4f& inv_view_proj,
                            const math::Vector3f& camera_pos,
                            const PostProcessParams& pp) {
    if (!valid_ || cmd == VK_NULL_HANDLE || tlas == VK_NULL_HANDLE) return;

    // 追踪分辨率档：光线数按 scale² 下降。trace 网格独立于输出分辨率。
    const float scale = math::clamp(pp.rt_reflection_resolution_scale, 0.25f, 1.0f);
    const uint32_t trace_w = std::max(1u, static_cast<uint32_t>(
        static_cast<float>(width) * scale + 0.5f));
    const uint32_t trace_h = std::max(1u, static_cast<uint32_t>(
        static_cast<float>(height) * scale + 0.5f));
    if (!ensure_targets(width, height, trace_w, trace_h)) return;

    // 反射图/输出图布局自愈：正常流中恒为 GENERAL（不再经 TRANSFER 往返），
    // 防御性处理历史状态，避免 oldLayout 不匹配时存储写入被丢弃。
    if (refl_layout_ != VK_IMAGE_LAYOUT_GENERAL) {
        cmd_transition_image(cmd, refl_image_, refl_layout_, VK_IMAGE_LAYOUT_GENERAL,
                            VK_ACCESS_NONE, VK_ACCESS_SHADER_WRITE_BIT);
        refl_layout_ = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (out_layout_ != VK_IMAGE_LAYOUT_GENERAL) {
        cmd_transition_image(cmd, out_image_, out_layout_, VK_IMAGE_LAYOUT_GENERAL,
                            VK_ACCESS_NONE, VK_ACCESS_SHADER_WRITE_BIT);
        out_layout_ = VK_IMAGE_LAYOUT_GENERAL;
    }

    // uniform 双区域：区域 0 = trace 参数（trace 分辨率 + 模式 0），
    // 区域 1 = composite 参数（全分辨率 + 模式 1）。
    const bool env_valid = env_view != VK_NULL_HANDLE && env_sampler != VK_NULL_HANDLE &&
                           prefilter_view != VK_NULL_HANDLE &&
                           prefilter_sampler != VK_NULL_HANDLE;
    RayQueryUniforms u{};
    u.inv_view_proj = inv_view_proj;
    u.view_proj = view_proj;
    u.cam_pos_max_dist = math::Vector4f(camera_pos.x, camera_pos.y, camera_pos.z,
                                        pp.rt_reflection_max_distance);
    u.resolution_strength =
        math::Vector4f(static_cast<float>(trace_w), static_cast<float>(trace_h),
                       1.0f, 0.0f);
    u.env_params = math::Vector4f(
        static_cast<float>(width) / static_cast<float>(trace_w),
        env_valid ? static_cast<float>(prefilter_mip_levels > 0 ? prefilter_mip_levels - 1
                                                                : 0)
                  : 0.0f,
        env_valid ? 1.0f : 0.0f, 0.0f);

    RayQueryUniforms uc = u;
    uc.resolution_strength = math::Vector4f(static_cast<float>(width),
                                            static_cast<float>(height), 1.0f, 1.0f);
    ubo_->upload(&u, sizeof(u), 0);
    ubo_->upload(&uc, sizeof(uc), 256);

    // 描述符写入（每帧重写，图像视图随尺寸/帧变化；集合 0 供 trace，集合 1 供 composite）
    constexpr VkDeviceSize k_ubo_stride = 256;  // 与 init() 的区域步长一致
    VkDescriptorBufferInfo buf_infos[2]{};
    for (int i = 0; i < 2; ++i) {
        buf_infos[i].buffer = ubo_->buffer();
        buf_infos[i].offset = static_cast<VkDeviceSize>(i) * k_ubo_stride;
        buf_infos[i].range = sizeof(RayQueryUniforms);
    }

    VkDescriptorBufferInfo params_info{};
    params_info.buffer = params_buffer;
    params_info.offset = 0;
    params_info.range = VK_WHOLE_SIZE;

    VkDescriptorImageInfo depth_info{};
    depth_info.imageView = depth_view;
    depth_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo lit_info{};
    lit_info.imageView = lit_view;
    lit_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo out_infos[2]{};
    out_infos[0].imageView = refl_view_;   // trace：写 trace 结果图
    out_infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    out_infos[1].imageView = out_view_;    // composite：写全分辨率输出图
    out_infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo env_info{};
    env_info.imageView = env_view;
    env_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    env_info.sampler = env_sampler;

    VkDescriptorImageInfo prefilter_info{};
    prefilter_info.imageView = prefilter_view;
    prefilter_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    prefilter_info.sampler = prefilter_sampler;

    VkDescriptorImageInfo refl_sample_info{};
    refl_sample_info.imageView = refl_view_;
    refl_sample_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSetAccelerationStructureKHR accel_write{};
    accel_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    accel_write.accelerationStructureCount = 1;
    accel_write.pAccelerationStructures = &tlas;

    VkDescriptorSet sets[2] = {trace_set_, composite_set_};
    VkWriteDescriptorSet writes[2][9]{};
    for (int s = 0; s < 2; ++s) {
        auto& w = writes[s];
        w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[0].pNext = &accel_write;
        w[0].dstSet = sets[s];
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1].dstSet = sets[s];
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[1].pBufferInfo = &buf_infos[s];

        w[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[2].dstSet = sets[s];
        w[2].dstBinding = 2;
        w[2].descriptorCount = 1;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[2].pImageInfo = &depth_info;

        w[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[3].dstSet = sets[s];
        w[3].dstBinding = 3;
        w[3].descriptorCount = 1;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[3].pImageInfo = &lit_info;

        w[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[4].dstSet = sets[s];
        w[4].dstBinding = 4;
        w[4].descriptorCount = 1;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[4].pImageInfo = &out_infos[s];

        w[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[5].dstSet = sets[s];
        w[5].dstBinding = 5;
        w[5].descriptorCount = 1;
        w[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[5].pImageInfo = &env_info;

        w[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[6].dstSet = sets[s];
        w[6].dstBinding = 6;
        w[6].descriptorCount = 1;
        w[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[6].pImageInfo = &prefilter_info;

        w[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[7].dstSet = sets[s];
        w[7].dstBinding = 7;
        w[7].descriptorCount = 1;
        w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[7].pBufferInfo = &params_info;

        w[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[8].dstSet = sets[s];
        w[8].dstBinding = 8;
        w[8].descriptorCount = 1;
        w[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w[8].pImageInfo = &refl_sample_info;
    }
    vkUpdateDescriptorSets(device_->device(), 9, writes[0], 0, nullptr);
    vkUpdateDescriptorSets(device_->device(), 9, writes[1], 0, nullptr);

    // ---- 派发 1：trace（半分辨率光线查询）----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1,
                            &trace_set_, 0, nullptr);
    vkCmdDispatch(cmd, (trace_w + 7) / 8, (trace_h + 7) / 8, 1);

    // trace 存储写完成 → composite 采样（着色器读）
    cmd_transition_image(cmd, refl_image_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    // ---- 派发 2：composite（全分辨率合成）----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1,
                            &composite_set_, 0, nullptr);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);

    // 输出图写完成 → 准备被后端拷回 HDR 颜色（TRANSFER_SRC）
    cmd_transition_image(cmd, out_image_, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
    out_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

} // namespace gryce_engine::render
