#version 450 core

// 体积光散射（丁达尔 / 神光 / 光柱）—— Vulkan 变体。
// 与 volumetric_scatter.frag 同一算法，差异有两处：
//   1. GLSL 方言：Vulkan 不允许裸 uniform，参数全部走 push_constant 块；纹理走
//      固定描述符集（binding 由 vk_shader.cpp 的 post_process_binding() 折算：
//      kTonemapHDR→0、深度→11、阴影贴图→13）。
//   2. 阴影 UV 的 Y 轴：Vulkan 的裁剪空间 Y 向下，阴影贴图纵向与 GL 相反，
//      需要 y = 1-(y*0.5+0.5)（与 vulkan_pbr.frag 的 to_shadow_uv 一致）。

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0)  uniform sampler2D uSceneTex;
layout(binding = 11) uniform sampler2D uDepthTex;
layout(binding = 13) uniform sampler2D uShadowMap;

// 与 C++ VulkanShader::VolumetricPushData 严格对齐（std430，224 字节）。
// 注意 std430 下 vec3 占满 16 字节：紧跟在 vec3 之后的标量必须能落进它的
// padding（如 uSunIntensity 落在 uSunDirection 的 +12），否则整块偏移错位。
// uLightSpaceMatrix 由 C++ 侧做 NDC z 重映射后推入（相机投影矩阵同理），
// 因此这里可以按 [0,1] 的 z 直接比较。
layout(push_constant) uniform PushConstants {
    mat4  uInvViewProj;      // +0
    mat4  uLightSpaceMatrix; // +64
    vec3  uCameraPos;        // +128
    float uPadCamera;        // +140
    vec3  uSunDirection;     // +144
    float uSunIntensity;     // +156
    vec3  uSunColor;         // +160
    float uIntensity;        // +172
    vec3  uTint;             // +176
    float uDensity;          // +188
    float uScattering;       // +192
    float uPhaseG;           // +196
    int   uSteps;            // +200
    float uMaxDistance;      // +204
    float uNDCZScale;        // +208
    float uNDCZBias;         // +212
    float uShadowBias;       // +216
    int   uShadowEnabled;    // +220
} pc;

const int kMaxSteps = 64;

vec3 world_from_depth(float depth, vec2 uv) {
    float ndc_z = depth * pc.uNDCZScale + pc.uNDCZBias;
    vec4 ndc = vec4(uv * 2.0 - 1.0, ndc_z, 1.0);
    vec4 world = pc.uInvViewProj * ndc;
    return world.xyz / world.w;
}

float hg_phase(float cos_theta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    // 与 GL 版一致：去掉 1/(4π)，让前向峰值落在 1~10 的量级。
    return (1.0 - g2) / pow(max(denom, 1e-4), 1.5);
}

bool sunlit(vec3 p) {
    if (pc.uShadowEnabled == 0) return true;
    vec4 lp = pc.uLightSpaceMatrix * vec4(p, 1.0);
    if (abs(lp.w) < 1e-6) return true;
    vec3 pcv = lp.xyz / lp.w;
    float sx = pcv.x * 0.5 + 0.5;
    float sy = 1.0 - (pcv.y * 0.5 + 0.5);   // Vulkan 阴影贴图纵向翻转
    float sz = pcv.z;                        // 矩阵已做 NDC z 重映射，[0,1]
    if (sx < 0.0 || sx > 1.0 || sy < 0.0 || sy > 1.0 || sz > 1.0) return true;
    return sz - pc.uShadowBias <= texture(uShadowMap, vec2(sx, sy)).r;
}

void main() {
    if (pc.uSteps <= 0 || pc.uDensity <= 0.0) {
        FragColor = vec4(texture(uSceneTex, vTexCoord).rgb, 1.0);
        return;
    }

    float depth = texture(uDepthTex, vTexCoord).r;
    vec3 ro = pc.uCameraPos;
    vec3 surface = world_from_depth(min(depth, 0.9999), vTexCoord);
    float dist = (depth >= 1.0) ? pc.uMaxDistance : length(surface - ro);
    vec3 rd = (depth >= 1.0)
                  ? normalize(world_from_depth(1.0, vTexCoord) - ro)
                  : normalize(surface - ro);

    vec3 sun_dir = normalize(pc.uSunDirection);
    float phase = hg_phase(dot(rd, sun_dir), pc.uPhaseG);

    int steps = min(pc.uSteps, kMaxSteps);
    float step_len = dist / float(steps);
    vec3 step_vec = rd * step_len;

    vec3 sun_col = pc.uSunColor * pc.uSunIntensity;
    float sigma_t = pc.uDensity;
    float sigma_s = pc.uDensity * pc.uScattering;

    float transmittance = 1.0;
    vec3 scatter = vec3(0.0);

    vec3 P = ro + step_vec * 0.5;
    for (int i = 0; i < kMaxSteps; ++i) {
        if (i >= steps) break;

        if (sunlit(P)) {
            vec3 S = sun_col * pc.uTint * phase * sigma_s;
            scatter += S * transmittance * step_len;
        }
        transmittance *= exp(-sigma_t * step_len);

        P += step_vec;
    }

    vec3 scene = texture(uSceneTex, vTexCoord).rgb;
    FragColor = vec4(scene + scatter * pc.uIntensity, 1.0);
}
