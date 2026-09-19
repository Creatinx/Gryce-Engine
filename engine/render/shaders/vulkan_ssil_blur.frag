#version 450 core

// SSIL 结果的深度感知双边模糊：平滑半球采样的噪声，
// 同时用深度差做权重，避免跨物体边缘把间接光糊出去。
// 逻辑与 GL 版 ssil_blur.frag 一致，仅替换为 Vulkan 方言。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

// 7 = uSSILTex (kSSILTexture), 11 = uDepthTex (kPBRShadowDepth)
layout(binding = 7)  uniform sampler2D uSSILTex;
layout(binding = 11) uniform sampler2D uDepthTex;

// 与 C++ VulkanShader::SSILPushData 严格对齐（std430，128 字节）；blur 只用到 near/far
layout(push_constant) uniform PushConstants {
    mat4 view;              // +0
    vec3 camera_pos;        // +64
    vec2 screen_size;       // +80
    float near_plane;       // +88
    float far_plane;        // +92
    float tan_half_fov;     // +96
    float aspect;           // +100
    float radius;           // +104
    float intensity;        // +108
    int steps;              // +112
} pc;

float linearize_depth(float d) {
    return (2.0 * pc.near_plane * pc.far_plane) /
           (pc.far_plane + pc.near_plane - d * (pc.far_plane - pc.near_plane));
}

void main() {
    float center_lin = linearize_depth(texture(uDepthTex, vUV).r);
    vec2 texel = 1.0 / max(vec2(textureSize(uSSILTex, 0)), vec2(1.0));

    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = vUV + vec2(float(x), float(y)) * texel;
            float d = linearize_depth(texture(uDepthTex, uv).r);
            // 深度差异越大权重越小（边缘保持）
            float depth_w = exp(-abs(d - center_lin) * 8.0);
            float spatial_w = exp(-(float(x * x) + float(y * y)) / 2.0);
            float w = depth_w * spatial_w;
            acc += texture(uSSILTex, uv).rgb * w;
            wsum += w;
        }
    }
    FragColor = vec4(acc / max(wsum, 1e-4), 1.0);
}
