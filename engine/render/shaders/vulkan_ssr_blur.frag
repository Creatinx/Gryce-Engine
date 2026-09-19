#version 450 core

// SSR 双边滤波：深度感知的边缘保持模糊，平滑步进产生的噪点。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

// 10 = uTexture (kSSRTexture), 11 = uDepthTexture (kPBRShadowDepth)
layout(binding = 10) uniform sampler2D uTexture;
layout(binding = 11) uniform sampler2D uDepthTexture;

// 与 C++ VulkanShader::SSRPushData 严格对齐（std430，128 字节）；blur 用到 near/far/bilateral
layout(push_constant) uniform PushConstants {
    mat4 view;              // +0
    vec3 camera_pos;        // +64
    vec2 screen_size;       // +80
    float near_plane;       // +88
    float far_plane;        // +92
    float tan_half_fov;     // +96
    float aspect;           // +100
    float max_roughness;    // +104
    int max_steps;          // +108
    float thickness;        // +112
    float bilateral_filter; // +116
    vec2 texel_size;        // +120
} pc;

float linearize_depth(float d) {
    return (2.0 * pc.near_plane * pc.far_plane) /
           (pc.far_plane + pc.near_plane - d * (pc.far_plane - pc.near_plane));
}

void main() {
    vec4 center = texture(uTexture, vUV);
    float center_lin = linearize_depth(texture(uDepthTexture, vUV).r);
    vec2 texel = 1.0 / max(vec2(textureSize(uTexture, 0)), vec2(1.0));
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 suv = vUV + vec2(float(x), float(y)) * texel;
            vec3 s = texture(uTexture, suv).rgb;
            float d = linearize_depth(texture(uDepthTexture, suv).r);
            float depth_w = exp(-abs(d - center_lin) * max(pc.bilateral_filter, 0.01));
            float spatial_w = exp(-(float(x * x) + float(y * y)) / 2.0);
            float w = depth_w * spatial_w;
            acc += s * w;
            wsum += w;
        }
    }
    FragColor = vec4(acc / max(wsum, 1e-4), center.a);
}