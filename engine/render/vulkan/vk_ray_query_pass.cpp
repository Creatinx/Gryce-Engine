// vk_ray_query_pass.cpp — Vulkan 硬件光追反射（P2.2 起，P2.4 时域降噪）实现
//
// 通道自包含：编译内联 GLSL → SPIR-V、建计算管线、描述符集、uniform 缓冲与
// 内部目标图。render() 在给定命令缓冲上做三段派发：
//   trace（追踪分辨率）：逐像素反射射线，写反射色 + 混合系数 + 法线/线性深度；
//   temporal（追踪分辨率）：重投影到历史帧 + 方差裁剪累积，压掉单帧噪声；
//   composite（全分辨率）：深度引导双边上采样，与 HDR 颜色混合。
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
// 同一着色器包含三个模式（UBO 的 resolutionStrength.w 分支）：
//   0 trace —— 从深度重建世界坐标与法线，沿反射方向做 ray query；写"看见什么"
//     （命中 → 屏幕 HDR 双线性 / 屏幕外与环境未命中 → 环境立方体贴图）与覆盖率，
//     并由后续时域累积收敛抖动噪声。
//   1 temporal —— 用上一帧 view-proj 把当前像素重投影到历史帧，按线性深度做
//     几何校验、按 3×3 邻域做方差裁剪，再按系数混合，输出累积结果。
//   2 composite —— 以线性深度为引导做双边上采样（深度断开的邻域不参与插值），
//     再按反射面自身的 BRDF 镜面占比与 HDR 颜色混合。
// 反射强度（谁该像镜子）只在 composite 决定：取前向着色器写进 HDR alpha 的 IBL
// 镜面亮度 / 总亮度。trace 只给覆盖率，这样金属镜面整块替换、绝缘体只加一丝，
// 与屏幕空间 SSR 的合成口径完全一致。
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
    mat4 prevViewProj;
    vec4 camPosMaxDist;         // xyz = 相机世界坐标, w = 最大追踪距离
    vec4 resolutionStrength;    // xy = 本段派发分辨率, z = 反射强度,
                                // w = 模式(0 trace / 1 temporal / 2 composite)
    vec4 envParams;             // x = 深度纹素缩放(全/追踪), y = prefilter 最大 mip,
                                // z = 环境强度(0 = 无环境), w = 帧序号
    vec4 temporalParams;        // x = 时域混合系数(1 = 纯当前帧), yz = 亚像素抖动,
                                // w = 保留
};
layout(binding = 2) uniform texture2D uDepth;    // 全分辨率 HDR 深度
layout(binding = 3) uniform texture2D uLit;      // 全分辨率 HDR 颜色
layout(binding = 4, rgba16f) uniform image2D uOut;
layout(binding = 5) uniform samplerCube uEnv;
layout(binding = 6) uniform samplerCube uPrefilter;
layout(std430, binding = 7) readonly buffer MatParams { vec2 mat_params[]; };
layout(binding = 8) uniform texture2D uRefl;     // 本帧原始反射 / 累积结果
layout(binding = 9) uniform texture2D uAux;      // 本帧辅助(法线+线性深度)
layout(binding = 10) uniform texture2D uHistCol; // 历史累积反射
layout(binding = 11) uniform texture2D uHistAux; // 历史辅助
layout(binding = 12, rgba16f) uniform image2D uAuxOut;  // trace 写辅助

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

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// 与 rdir 正交的一组基（Duff 等人的无分支构造）。
void onb(vec3 n, out vec3 t, out vec3 b) {
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float bb = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * bb, -s * n.x);
    b = vec3(bb, s + n.y * n.y * a, -n.y);
}

void main() {
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    ivec2 res = ivec2(resolutionStrength.xy);
    if (px.x >= res.x || px.y >= res.y) return;
    float strength = resolutionStrength.z;
    int mode = int(resolutionStrength.w + 0.5);

    if (mode == 0) {
        // ---- 模式 0：trace（追踪分辨率光线查询）----
        float texelScale = envParams.x;
        ivec2 fullRes = ivec2(vec2(res) * texelScale + 0.5);
        // 亚像素抖动：连续帧采样不同子位置，时域累积后等效超采样（消边缘阶梯）。
        vec2 uv = (vec2(px) + 0.5 + temporalParams.yz) / vec2(res);
        uv = clamp(uv, vec2(0.0), vec2(0.999999));

        ivec2 dp = clamp(ivec2(vec2(px) * texelScale), ivec2(0), fullRes - ivec2(1));
        float depth = texelFetch(uDepth, dp, 0).r;
        // 远处（天空 / 远平面）不产生反射：系数 0，composite 时保留原色。
        if (depth >= 1.0) {
            imageStore(uOut, px, vec4(0.0));
            imageStore(uAuxOut, px, vec4(0.0));
            return;
        }

        vec3 world = reconstructWorld(uv, depth);
        vec3 camPos = camPosMaxDist.xyz;

        // 法线：全分辨率 1 纹素的对称中心差分（基线 = 2 纹素 ≈ 一个追踪纹素）。
        // 不用「逐轴挑深度更连续的一侧」——在斜面/透视渐变下这种选择会随亚像素
        // 位置周期翻转，重建法线跟着抖，镜面地板上表现为规则的横向条纹。
        // 邻居落在背景或跨轮廓（深度跳变）时用中心点顶替，退化为半基线但不跳向。
        const ivec2 offs[4] = {ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1)};
        vec3 nb[4];
        for (int i = 0; i < 4; ++i) {
            ivec2 q = clamp(dp + offs[i], ivec2(0), fullRes - ivec2(1));
            float dn = texelFetch(uDepth, q, 0).r;
            bool ok = dn < 1.0 && abs(dn - depth) <= depth * 0.05;
            nb[i] = ok ? reconstructWorld((vec2(q) + 0.5) / vec2(fullRes), dn) : world;
        }
        vec3 n = normalize(cross(nb[0] - nb[1], nb[2] - nb[3]));
        vec3 vdir = normalize(world - camPos);
        // 法线一律朝向相机：重建出的叉积方向取决于屏幕坐标手性，不统一的话
        // 反射射线会扎进表面自相交，命中恒为自身、反射等于原色，等于没效果。
        if (dot(n, vdir) > 0.0) n = -n;
        if (any(isnan(n))) n = -vdir;

        vec3 rdir = reflect(vdir, n);

        // 沿法线外推一点起点，避免与发射像素所在三角形自相交。
        vec3 origin = world + n * 0.02;
        rayQueryEXT rq;
        rayQueryInitializeEXT(rq, uTlas, gl_RayFlagsOpaqueEXT, 0xFF,
                              origin, 0.02, rdir, camPosMaxDist.w);
        while (rayQueryProceedEXT(rq)) {}

        // 反射内容只回答"这个方向上看见什么"，不再决定"这个表面反射多强"：
        // 强度由合成段的 BRDF 占比（IBL 镜面亮度 / 总亮度）给出，与 SSR 合成同一
        // 口径。此处用命中实例的材质做权重会在物理上说不通 —— 反射强弱取决于
        // *反射面* 的 F0/粗糙度，而不是被反射物体的材质；旧实现据此把掠射角下的
        // 绝缘体（f0≈0.04）按命中金属（f0≈1）算成整块镜面，画面因此整体发灰、
        // 每个盒子的下半截都被"替换"成地面颜色。
        vec3 reflCol = vec3(0.0);
        float cover = 0.0;
        if (rayQueryGetIntersectionTypeEXT(rq, true) !=
            gl_RayQueryCommittedIntersectionNoneEXT) {
            float t = rayQueryGetIntersectionTEXT(rq, true);
            vec3 hit = origin + rdir * t;

            // 命中点在屏幕内 → 取该点的 HDR 颜色（与主画面同分辨率）；
            // 命中点在屏幕外（相机背后 / 视锥外）→ 退回环境贴图，避免采到被
            // clamp 到屏幕边缘的地面色。
            vec4 hc = viewProj * vec4(hit, 1.0);
            bool on_screen = false;
            if (hc.w > 0.0) {
                vec2 huv = (hc.xy / hc.w) * 0.5 + 0.5;
                huv.y = 1.0 - huv.y;
                if (all(greaterThanEqual(huv, vec2(0.0))) &&
                    all(lessThanEqual(huv, vec2(1.0)))) {
                    reflCol = bilinear(uLit, huv * vec2(fullRes), fullRes).rgb;
                    on_screen = true;
                }
            }
            if (!on_screen && envParams.z > 0.0) reflCol = texture(uEnv, rdir).rgb;
            cover = 1.0;
        } else if (envParams.z > 0.0) {
            // 未命中 = 反射方向射向环境（天空）：直接采样环境立方体贴图。
            // 这是屏幕空间方案永远给不出的内容（P2.3 环境反射）。
            reflCol = texture(uEnv, rdir).rgb;
            cover = 1.0;
        }
        imageStore(uOut, px, vec4(reflCol, cover * strength));
        imageStore(uAuxOut, px, vec4(n, length(world - camPos)));
    } else if (mode == 1) {
        // ---- 模式 1：temporal（时域累积降噪）----
        float texelScale = envParams.x;
        ivec2 fullRes = ivec2(vec2(res) * texelScale + 0.5);
        ivec2 dp = clamp(ivec2(vec2(px) * texelScale), ivec2(0), fullRes - ivec2(1));
        float depth = texelFetch(uDepth, dp, 0).r;
        vec4 cur = texelFetch(uRefl, px, 0);
        if (depth >= 1.0) {
            imageStore(uOut, px, vec4(0.0));
            return;
        }

        vec2 uv = (vec2(px) + 0.5) / vec2(res);
        vec3 world = reconstructWorld(uv, depth);
        float linCur = length(world - camPosMaxDist.xyz);

        // 3×3 邻域方差裁剪：把历史限制在当前帧邻域色域内，抑制鬼影
        // （相机不动而反射内容变化时，历史不会把旧内容拖过来）。
        vec3 mn = vec3(1e9);
        vec3 mx = vec3(-1e9);
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                ivec2 q = clamp(px + ivec2(x, y), ivec2(0), res - ivec2(1));
                vec4 s = texelFetch(uRefl, q, 0);
                mn = min(mn, s.rgb);
                mx = max(mx, s.rgb);
            }
        }
        vec3 span = max(mx - mn, vec3(1e-4));

        vec4 outv = cur;
        float alphaTarget = clamp(temporalParams.x, 0.01, 1.0);
        if (alphaTarget < 0.999) {
            vec4 pc = prevViewProj * vec4(world, 1.0);
            if (pc.w > 1e-6) {
                vec2 puv = (pc.xy / pc.w) * 0.5 + 0.5;
                puv.y = 1.0 - puv.y;
                if (all(greaterThanEqual(puv, vec2(0.0))) &&
                    all(lessThanEqual(puv, vec2(1.0)))) {
                    vec4 h = bilinear(uHistCol, puv * vec2(res), res);
                    vec4 ha = bilinear(uHistAux, puv * vec2(res), res);
                    float linHist = ha.a;
                    // 几何校验：重投影位置的线性深度与当前不一致 → 遮挡/去遮挡，
                    // 历史不可信（用相对深度而不是绝对距离，透视下更稳）。
                    float rel = linHist > 0.0 ? abs(linHist - linCur) / max(linCur, 1e-3)
                                              : 1.0;
                    float w = 1.0 - smoothstep(0.04, 0.18, rel);
                    if (w > 0.0) {
                        h.rgb = clamp(h.rgb, mn - span * 0.15, mx + span * 0.15);
                        h.a = clamp(h.a, 0.0, 1.0);
                        float alpha = mix(1.0, alphaTarget, w);
                        outv = mix(h, cur, alpha);
                    }
                }
            }
        }
        imageStore(uOut, px, outv);
    } else {
        // ---- 模式 2：composite（全分辨率双边上采样合成）----
        ivec2 fullRes = res;
        ivec2 tRes = textureSize(uRefl, 0);
        vec3 lit = texelFetch(uLit, px, 0).rgb;
        float depth = texelFetch(uDepth, px, 0).r;
        if (depth >= 1.0) {
            imageStore(uOut, px, vec4(lit, 1.0));
            return;
        }

        vec2 uv = (vec2(px) + 0.5) / vec2(fullRes);
        vec3 world = reconstructWorld(uv, depth);
        float lin = length(world - camPosMaxDist.xyz);

        // 2×2 双线性权重 × 深度相似权重：轮廓两侧（深度断开）的纹素权重塌陷，
        // 于是物体边缘不会被追踪分辨率的方块拖糊 —— 这是"像素感"的主要来源。
        vec2 c = uv * vec2(tRes) - 0.5;
        ivec2 base = ivec2(floor(c));
        vec2 f = c - vec2(base);
        vec4 sum = vec4(0.0);
        float wsum = 0.0;
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                float bw = (i == 0 ? 1.0 - f.x : f.x) * (j == 0 ? 1.0 - f.y : f.y);
                if (bw <= 1e-5) continue;
                ivec2 q = clamp(base + ivec2(i, j), ivec2(0), tRes - ivec2(1));
                float guide = texelFetch(uAux, q, 0).a;
                float rel = guide > 0.0 ? abs(guide - lin) / max(lin, 1e-3) : 1.0;
                float dw = 1.0 / (1e-3 + rel * 60.0);
                float w = bw * dw;
                sum += texelFetch(uRefl, q, 0) * w;
                wsum += w;
            }
        }
        vec4 r = wsum > 1e-6 ? sum / wsum : vec4(0.0);
        float cover = clamp(r.a, 0.0, 1.0);
        if (cover <= 0.0) {
            imageStore(uOut, px, vec4(lit, 1.0));
            return;
        }
        // 与 SSR 合成分解同一套 BRDF 权重：uLit.a 是前向着色器写入的 IBL 镜面项
        // 亮度（见 vulkan_pbr.frag / pbr.frag），镜面占比 = 镜面亮度 / 总亮度。
        // 金属 ≈ 1（整块被反射替换，并按表面色相着色），绝缘体 ≈ 0.04（只加一丝）。
        // 旧实现直接 mix(lit, refl, cover)，等于把所有材质一律当镜面，画面发灰、
        // 盒子/球下半截被反射内容整块覆盖。
        float lum = dot(lit, vec3(0.2126, 0.7152, 0.0722));
        float spec_share = lum > 1e-4 ? clamp(texelFetch(uLit, px, 0).a / lum, 0.0, 1.0) : 0.0;
        vec3 tint = lit / max(lum, 1e-4);
        float w = cover * spec_share;
        vec3 color = lit * (1.0 - w) + tint * r.rgb * w;
        imageStore(uOut, px, vec4(color, 1.0));
    }
}
)GLSL";

namespace {

// Halton 低差异序列：用于逐帧亚像素抖动（2/3 基，周期长、分布均匀）。
float halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    uint32_t i = index;
    while (i > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

constexpr VkDeviceSize k_ubo_stride = 256;  // 对齐 minUniformBufferOffsetAlignment(256)
constexpr uint32_t k_stage_count = 3;       // trace / temporal / composite

}  // namespace

VkRayQueryPass::VkRayQueryPass(VkRayQueryPass&& other) noexcept { *this = std::move(other); }

VkRayQueryPass& VkRayQueryPass::operator=(VkRayQueryPass&& other) noexcept {
    if (this != &other) {
        device_ = other.device_;
        valid_ = other.valid_;
        module_ = other.module_;
        layout_ = other.layout_;
        trace_pipeline_ = other.trace_pipeline_;
        temporal_pipeline_ = other.temporal_pipeline_;
        composite_pipeline_ = other.composite_pipeline_;
        desc_layout_ = other.desc_layout_;
        pool_ = other.pool_;
        trace_set_ = other.trace_set_;
        temporal_set_ = other.temporal_set_;
        composite_set_ = other.composite_set_;
        ubo_ = other.ubo_;
        one_shot_pool_ = other.one_shot_pool_;
        refl_image_ = other.refl_image_;
        refl_alloc_ = other.refl_alloc_;
        refl_view_ = other.refl_view_;
        refl_w_ = other.refl_w_;
        refl_h_ = other.refl_h_;
        refl_layout_ = other.refl_layout_;
        for (int i = 0; i < 2; ++i) {
            aux_image_[i] = other.aux_image_[i];
            aux_alloc_[i] = other.aux_alloc_[i];
            aux_view_[i] = other.aux_view_[i];
            aux_layout_[i] = other.aux_layout_[i];
            acc_image_[i] = other.acc_image_[i];
            acc_alloc_[i] = other.acc_alloc_[i];
            acc_view_[i] = other.acc_view_[i];
            acc_layout_[i] = other.acc_layout_[i];
            other.aux_image_[i] = VK_NULL_HANDLE;
            other.aux_alloc_[i] = nullptr;
            other.aux_view_[i] = VK_NULL_HANDLE;
            other.aux_layout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
            other.acc_image_[i] = VK_NULL_HANDLE;
            other.acc_alloc_[i] = nullptr;
            other.acc_view_[i] = VK_NULL_HANDLE;
            other.acc_layout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        frame_parity_ = other.frame_parity_;
        frame_index_ = other.frame_index_;
        history_valid_ = other.history_valid_;
        prev_view_proj_ = other.prev_view_proj_;
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
        other.temporal_pipeline_ = VK_NULL_HANDLE;
        other.composite_pipeline_ = VK_NULL_HANDLE;
        other.desc_layout_ = VK_NULL_HANDLE;
        other.pool_ = VK_NULL_HANDLE;
        other.trace_set_ = VK_NULL_HANDLE;
        other.temporal_set_ = VK_NULL_HANDLE;
        other.composite_set_ = VK_NULL_HANDLE;
        other.ubo_ = nullptr;
        other.one_shot_pool_ = VK_NULL_HANDLE;
        other.refl_image_ = VK_NULL_HANDLE;
        other.refl_alloc_ = nullptr;
        other.refl_view_ = VK_NULL_HANDLE;
        other.refl_w_ = other.refl_h_ = 0;
        other.refl_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        other.out_image_ = VK_NULL_HANDLE;
        other.out_alloc_ = nullptr;
        other.out_view_ = VK_NULL_HANDLE;
        other.out_w_ = other.out_h_ = 0;
        other.out_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    return *this;
}

bool VkRayQueryPass::init(VulkanDevice* device) {
    if (valid_) return true;
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
    // prefilter cube / 材质参数 SSBO / 反射采样 / 辅助图 / 历史色 / 历史辅助 /
    // 辅助输出。
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
        {8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
        {9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
        {10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
        {11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
        {12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
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

    // 三条计算管线共用同一 shader module 与布局，仅 uniform 的模式位不同：
    // trace（光线查询）、temporal（时域累积）、composite（全分辨率合成）。
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
    if (!make_pipeline(trace_pipeline_) || !make_pipeline(temporal_pipeline_) ||
        !make_pipeline(composite_pipeline_)) {
        return false;
    }

    // 三槽位描述符集：槽 0 trace（uOut=原始反射, uAuxOut=本帧辅助）、
    // 槽 1 temporal（uOut=累积结果）、槽 2 composite（uOut=全分辨率输出）。
    constexpr uint32_t k_slots = k_stage_count;
    VkDescriptorPoolSize pool_sizes[6]{};
    pool_sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    pool_sizes[0].descriptorCount = k_slots;
    pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pool_sizes[1].descriptorCount = k_slots;
    pool_sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pool_sizes[2].descriptorCount = k_slots * 6;
    pool_sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_sizes[3].descriptorCount = k_slots * 2;
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

    VkDescriptorSet sets[3]{};
    VkDescriptorSetLayout layouts[3] = {desc_layout_, desc_layout_, desc_layout_};
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
    temporal_set_ = sets[1];
    composite_set_ = sets[2];

    // uniform 缓冲三个区域（trace / temporal / composite）。
    // 区域步长必须按 minUniformBufferOffsetAlignment 取整（NVIDIA 桌面为 256）：
    // 结构体本身只有 256 字节，若步长不是对齐值，第二、三段会造出未对齐描述符 ——
    // 描述符非法时驱动静默返回全零，composite 一进来就被 `px >= res` 早退，输出图
    // 恒为未写入的黑。HOST_VISIBLE + COHERENT，每帧 memcpy 上传。
    static_assert(sizeof(RayQueryUniforms) <= k_ubo_stride,
                  "RayQueryUniforms exceeded UBO region stride");
    ubo_ = new VulkanBuffer();
    if (!ubo_->init(device, k_ubo_stride * k_stage_count,
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        GLOG_ERROR("VkRayQueryPass: uniform buffer init failed");
        delete ubo_;
        ubo_ = nullptr;
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
    GLOG_INFO("VkRayQueryPass: ray-query reflection pipelines ready (trace + temporal + composite)");
    return true;
}

void VkRayQueryPass::shutdown() {
    if (!device_ || !device_->is_valid()) {
        device_ = nullptr;
        valid_ = false;
        module_ = VK_NULL_HANDLE;
        layout_ = VK_NULL_HANDLE;
        trace_pipeline_ = VK_NULL_HANDLE;
        temporal_pipeline_ = VK_NULL_HANDLE;
        composite_pipeline_ = VK_NULL_HANDLE;
        desc_layout_ = VK_NULL_HANDLE;
        pool_ = VK_NULL_HANDLE;
        trace_set_ = VK_NULL_HANDLE;
        temporal_set_ = VK_NULL_HANDLE;
        composite_set_ = VK_NULL_HANDLE;
        ubo_ = nullptr;
        one_shot_pool_ = VK_NULL_HANDLE;
        refl_image_ = VK_NULL_HANDLE;
        refl_alloc_ = nullptr;
        refl_view_ = VK_NULL_HANDLE;
        for (int i = 0; i < 2; ++i) {
            aux_image_[i] = VK_NULL_HANDLE;
            aux_alloc_[i] = nullptr;
            aux_view_[i] = VK_NULL_HANDLE;
            aux_layout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
            acc_image_[i] = VK_NULL_HANDLE;
            acc_alloc_[i] = nullptr;
            acc_view_[i] = VK_NULL_HANDLE;
            acc_layout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        out_image_ = VK_NULL_HANDLE;
        out_alloc_ = nullptr;
        out_view_ = VK_NULL_HANDLE;
        return;
    }
    VkDevice dev = device_->device();
    vkDeviceWaitIdle(dev);
    destroy_targets();
    if (composite_pipeline_) vkDestroyPipeline(dev, composite_pipeline_, nullptr);
    if (temporal_pipeline_) vkDestroyPipeline(dev, temporal_pipeline_, nullptr);
    if (trace_pipeline_) vkDestroyPipeline(dev, trace_pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (desc_layout_) vkDestroyDescriptorSetLayout(dev, desc_layout_, nullptr);
    if (pool_) vkDestroyDescriptorPool(dev, pool_, nullptr);
    if (module_) vkDestroyShaderModule(dev, module_, nullptr);
    if (one_shot_pool_) vkDestroyCommandPool(dev, one_shot_pool_, nullptr);
    if (ubo_) {
        ubo_->shutdown();
        delete ubo_;
        ubo_ = nullptr;
    }
    device_ = nullptr;
    valid_ = false;
}

void VkRayQueryPass::transition_image(VkImage image, VkImageLayout old_layout,
                                      VkImageLayout new_layout, VkAccessFlags src_access,
                                      VkAccessFlags dst_access, VkImageAspectFlags aspect) {
    if (!device_ || !device_->is_valid() || image == VK_NULL_HANDLE) return;
    VkDevice dev = device_->device();

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = one_shot_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
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
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &bar);
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
    for (int i = 0; i < 2; ++i) {
        if (!create_image(aux_image_[i], aux_alloc_[i], aux_view_[i], trace_width, trace_height,
                          VK_IMAGE_USAGE_SAMPLED_BIT) ||
            !create_image(acc_image_[i], acc_alloc_[i], acc_view_[i], trace_width, trace_height,
                          VK_IMAGE_USAGE_SAMPLED_BIT)) {
            return false;
        }
    }
    if (!create_image(out_image_, out_alloc_, out_view_, width, height,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        return false;
    }
    // 初始布局：UNDEFINED → GENERAL（存储图可直接写入）
    transition_image(refl_image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    refl_layout_ = VK_IMAGE_LAYOUT_GENERAL;
    for (int i = 0; i < 2; ++i) {
        transition_image(aux_image_[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        aux_layout_[i] = VK_IMAGE_LAYOUT_GENERAL;
        transition_image(acc_image_[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        acc_layout_[i] = VK_IMAGE_LAYOUT_GENERAL;
    }
    transition_image(out_image_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    out_layout_ = VK_IMAGE_LAYOUT_GENERAL;

    refl_w_ = static_cast<int>(trace_width);
    refl_h_ = static_cast<int>(trace_height);
    out_w_ = static_cast<int>(width);
    out_h_ = static_cast<int>(height);
    // 尺寸变了（含首次创建）：历史内容与当前帧不对应，累积从头开始。
    frame_parity_ = 0;
    history_valid_ = false;
    return true;
}

void VkRayQueryPass::destroy_targets() {
    if (!device_ || !device_->is_valid()) {
        refl_image_ = VK_NULL_HANDLE;
        refl_alloc_ = nullptr;
        refl_view_ = VK_NULL_HANDLE;
        for (int i = 0; i < 2; ++i) {
            aux_image_[i] = VK_NULL_HANDLE;
            aux_alloc_[i] = nullptr;
            aux_view_[i] = VK_NULL_HANDLE;
            aux_layout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
            acc_image_[i] = VK_NULL_HANDLE;
            acc_alloc_[i] = nullptr;
            acc_view_[i] = VK_NULL_HANDLE;
            acc_layout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        out_image_ = VK_NULL_HANDLE;
        out_alloc_ = nullptr;
        out_view_ = VK_NULL_HANDLE;
        refl_w_ = refl_h_ = out_w_ = out_h_ = 0;
        refl_layout_ = out_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
        history_valid_ = false;
        return;
    }
    VkDevice dev = device_->device();
    if (refl_view_) vkDestroyImageView(dev, refl_view_, nullptr);
    if (refl_image_) vmaDestroyImage(device_->allocator(), refl_image_, refl_alloc_);
    for (int i = 0; i < 2; ++i) {
        if (aux_view_[i]) vkDestroyImageView(dev, aux_view_[i], nullptr);
        if (aux_image_[i]) vmaDestroyImage(device_->allocator(), aux_image_[i], aux_alloc_[i]);
        if (acc_view_[i]) vkDestroyImageView(dev, acc_view_[i], nullptr);
        if (acc_image_[i]) vmaDestroyImage(device_->allocator(), acc_image_[i], acc_alloc_[i]);
        aux_view_[i] = VK_NULL_HANDLE;
        aux_image_[i] = VK_NULL_HANDLE;
        aux_alloc_[i] = nullptr;
        aux_layout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
        acc_view_[i] = VK_NULL_HANDLE;
        acc_image_[i] = VK_NULL_HANDLE;
        acc_alloc_[i] = nullptr;
        acc_layout_[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }
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
    history_valid_ = false;
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

    const bool denoise = pp.rt_reflection_denoise != 0;

    // 布局自愈：正常流中恒为 GENERAL（不经 TRANSFER 往返），防御性处理历史状态，
    // 避免 oldLayout 不匹配时存储写入被丢弃。
    const auto heal = [&](VkImage img, VkImageLayout& layout) {
        if (layout != VK_IMAGE_LAYOUT_GENERAL) {
            cmd_transition_image(cmd, img, layout, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_ACCESS_NONE, VK_ACCESS_SHADER_WRITE_BIT);
            layout = VK_IMAGE_LAYOUT_GENERAL;
        }
    };
    heal(refl_image_, refl_layout_);
    heal(aux_image_[0], aux_layout_[0]);
    heal(aux_image_[1], aux_layout_[1]);
    heal(acc_image_[0], acc_layout_[0]);
    heal(acc_image_[1], acc_layout_[1]);
    heal(out_image_, out_layout_);

    const uint32_t cur = frame_parity_;
    const uint32_t prev = 1u - cur;

    // 本帧要写的两张图上一轮是读取方（历史/累积），先做一次「读→写」依赖，
    // 否则与两帧前的读取形成 WAR 冒险（驱动可能在不同帧间重排）。
    if (history_valid_) {
        cmd_transition_image(cmd, aux_image_[cur], VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT);
        cmd_transition_image(cmd, acc_image_[cur], VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                             VK_ACCESS_SHADER_WRITE_BIT);
    }

    // ---- uniform：三个区域，分别给 trace / temporal / composite ----
    const bool env_valid = env_view != VK_NULL_HANDLE && env_sampler != VK_NULL_HANDLE &&
                           prefilter_view != VK_NULL_HANDLE &&
                           prefilter_sampler != VK_NULL_HANDLE;
    const float texel_scale = static_cast<float>(width) / static_cast<float>(trace_w);
    const float jitter_x = halton(static_cast<uint32_t>(frame_index_ % 64u) + 1u, 2u) - 0.5f;
    const float jitter_y = halton(static_cast<uint32_t>(frame_index_ % 64u) + 1u, 3u) - 0.5f;

    RayQueryUniforms base{};
    base.inv_view_proj = inv_view_proj;
    base.view_proj = view_proj;
    base.prev_view_proj = history_valid_ ? prev_view_proj_ : view_proj;
    base.cam_pos_max_dist = math::Vector4f(camera_pos.x, camera_pos.y, camera_pos.z,
                                           pp.rt_reflection_max_distance);
    base.env_params = math::Vector4f(
        texel_scale,
        env_valid ? static_cast<float>(prefilter_mip_levels > 0 ? prefilter_mip_levels - 1 : 0)
                  : 0.0f,
        env_valid ? 1.0f : 0.0f,
        static_cast<float>(frame_index_ % 4096u));

    RayQueryUniforms u_trace = base;
    u_trace.resolution_strength =
        math::Vector4f(static_cast<float>(trace_w), static_cast<float>(trace_h), 1.0f, 0.0f);
    u_trace.temporal_params = math::Vector4f(1.0f, jitter_x, jitter_y, 0.0f);

    // 时域混合系数：默认 0.12（约 8 帧收敛）；首帧 / 关降噪时为 1（纯当前帧）。
    constexpr float k_temporal_blend = 0.12f;
    RayQueryUniforms u_temporal = base;
    u_temporal.resolution_strength =
        math::Vector4f(static_cast<float>(trace_w), static_cast<float>(trace_h), 1.0f, 1.0f);
    u_temporal.temporal_params = math::Vector4f(history_valid_ && denoise ? k_temporal_blend : 1.0f,
                                               0.0f, 0.0f, 0.0f);

    RayQueryUniforms u_composite = base;
    u_composite.resolution_strength =
        math::Vector4f(static_cast<float>(width), static_cast<float>(height), 1.0f, 2.0f);

    ubo_->upload(&u_trace, sizeof(u_trace), 0);
    ubo_->upload(&u_temporal, sizeof(u_temporal), k_ubo_stride);
    ubo_->upload(&u_composite, sizeof(u_composite), k_ubo_stride * 2);

    VkDescriptorBufferInfo buf_infos[k_stage_count]{};
    for (uint32_t i = 0; i < k_stage_count; ++i) {
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

    VkDescriptorImageInfo env_info{};
    env_info.imageView = env_view;
    env_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    env_info.sampler = env_sampler;

    VkDescriptorImageInfo prefilter_info{};
    prefilter_info.imageView = prefilter_view;
    prefilter_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    prefilter_info.sampler = prefilter_sampler;

    // composite 读的是累积图（关降噪时退回本帧原始反射）。
    const VkImageView composite_src = denoise ? acc_view_[cur] : refl_view_;

    // 三段的图像视图：uOut（存储写）、uRefl（采样）、uAux（采样）、uHistCol、
    // uHistAux、uAuxOut（存储写）。
    const VkImageView out_views[k_stage_count] = {refl_view_, acc_view_[cur], out_view_};
    const VkImageView refl_views[k_stage_count] = {refl_view_, refl_view_, composite_src};
    const VkImageView hist_col_views[k_stage_count] = {acc_view_[prev], acc_view_[prev],
                                                       acc_view_[prev]};
    const VkImageView hist_aux_views[k_stage_count] = {aux_view_[prev], aux_view_[prev],
                                                       aux_view_[prev]};
    // uAuxOut 只有 trace 段写；其余段绑一个合法但不会被写的图（累积图的前一帧），
    // 避免同一集合里同时出现同图的可写与可读描述符。
    const VkImageView aux_out_views[k_stage_count] = {aux_view_[cur], acc_view_[prev],
                                                      acc_view_[prev]};

    VkDescriptorImageInfo out_infos[k_stage_count]{};
    VkDescriptorImageInfo refl_infos[k_stage_count]{};
    VkDescriptorImageInfo hist_col_infos[k_stage_count]{};
    VkDescriptorImageInfo hist_aux_infos[k_stage_count]{};
    VkDescriptorImageInfo aux_in_infos{};
    aux_in_infos.imageView = aux_view_[cur];
    aux_in_infos.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo aux_out_infos[k_stage_count]{};
    for (uint32_t s = 0; s < k_stage_count; ++s) {
        out_infos[s].imageView = out_views[s];
        out_infos[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        refl_infos[s].imageView = refl_views[s];
        refl_infos[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        hist_col_infos[s].imageView = hist_col_views[s];
        hist_col_infos[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        hist_aux_infos[s].imageView = hist_aux_views[s];
        hist_aux_infos[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        aux_out_infos[s].imageView = aux_out_views[s];
        aux_out_infos[s].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }

    VkWriteDescriptorSetAccelerationStructureKHR accel_write{};
    accel_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    accel_write.accelerationStructureCount = 1;
    accel_write.pAccelerationStructures = &tlas;

    const VkDescriptorSet sets[k_stage_count] = {trace_set_, temporal_set_, composite_set_};
    // 每段最多写 13 个描述符（binding 0..12）。
    constexpr uint32_t k_writes = 13;
    VkWriteDescriptorSet writes[k_stage_count][k_writes]{};

    const auto set_write = [](VkWriteDescriptorSet& w, VkDescriptorSet set, uint32_t binding,
                              VkDescriptorType type) {
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = type;
    };

    for (uint32_t s = 0; s < k_stage_count; ++s) {
        auto& w = writes[s];
        set_write(w[0], sets[s], 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
        w[0].pNext = &accel_write;
        set_write(w[1], sets[s], 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        w[1].pBufferInfo = &buf_infos[s];
        set_write(w[2], sets[s], 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        w[2].pImageInfo = &depth_info;
        set_write(w[3], sets[s], 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        w[3].pImageInfo = &lit_info;
        set_write(w[4], sets[s], 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        w[4].pImageInfo = &out_infos[s];
        set_write(w[5], sets[s], 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        w[5].pImageInfo = &env_info;
        set_write(w[6], sets[s], 6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        w[6].pImageInfo = &prefilter_info;
        set_write(w[7], sets[s], 7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        w[7].pBufferInfo = &params_info;
        set_write(w[8], sets[s], 8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        w[8].pImageInfo = &refl_infos[s];
        set_write(w[9], sets[s], 9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        w[9].pImageInfo = &aux_in_infos;
        set_write(w[10], sets[s], 10, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        w[10].pImageInfo = &hist_col_infos[s];
        set_write(w[11], sets[s], 11, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        w[11].pImageInfo = &hist_aux_infos[s];
        set_write(w[12], sets[s], 12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        w[12].pImageInfo = &aux_out_infos[s];
    }
    for (uint32_t s = 0; s < k_stage_count; ++s) {
        vkUpdateDescriptorSets(device_->device(), k_writes, writes[s], 0, nullptr);
    }

    // ---- 派发 1：trace（追踪分辨率光线查询）----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, trace_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1,
                            &trace_set_, 0, nullptr);
    vkCmdDispatch(cmd, (trace_w + 7) / 8, (trace_h + 7) / 8, 1);

    // trace 写出 → temporal/composite 采样（着色器写 → 着色器读写）
    cmd_transition_image(cmd, refl_image_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    cmd_transition_image(cmd, aux_image_[cur], VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_ACCESS_SHADER_READ_BIT);

    if (denoise) {
        // ---- 派发 2：temporal（时域累积）----
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, temporal_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1,
                                &temporal_set_, 0, nullptr);
        vkCmdDispatch(cmd, (trace_w + 7) / 8, (trace_h + 7) / 8, 1);
        cmd_transition_image(cmd, acc_image_[cur], VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                             VK_ACCESS_SHADER_READ_BIT);
    }

    // ---- 派发 3：composite（全分辨率双边合成）----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, composite_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1,
                            &composite_set_, 0, nullptr);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);

    // 输出图写完成 → 准备被后端拷回 HDR 颜色（TRANSFER_SRC）
    cmd_transition_image(cmd, out_image_, VK_IMAGE_LAYOUT_GENERAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT);
    out_layout_ = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // 翻页：本帧结果成为下一帧的历史。
    prev_view_proj_ = view_proj;
    frame_parity_ = prev;
    frame_index_++;
    history_valid_ = true;
}

} // namespace gryce_engine::render
