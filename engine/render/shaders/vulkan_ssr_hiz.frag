#version 450 core

// SSR HiZ 金字塔构建：2x2 取最小深度下采样。
// 深度缓冲存的是非线性 [0,1] 深度，min 在非线性空间与线性空间等价
// （线性化是单调函数），因此直接对原始值取 min 即可。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uInput; // 上一级：depth_tex(level 0) 或 HiZ[i-1]

// 与 C++ VulkanShader::SSRPushData 严格对齐（std430，128 字节）。HiZ pass 只用到
// texel_size（由 shader 用 textureSize 推导，不依赖 C++ 侧填充）。
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

void main() {
    // 输入尺寸在逐级下采样时变化，用 textureSize 精确推导，避免依赖未填充的 texel_size
    vec2 texel = 1.0 / max(vec2(textureSize(uInput, 0)), vec2(1.0));
    vec2 c = vUV;
    float d00 = texture(uInput, c + vec2(-texel.x, -texel.y)).r;
    float d10 = texture(uInput, c + vec2( texel.x, -texel.y)).r;
    float d01 = texture(uInput, c + vec2(-texel.x,  texel.y)).r;
    float d11 = texture(uInput, c + vec2( texel.x,  texel.y)).r;
    FragColor = vec4(min(min(d00, d10), min(d01, d11)));
}