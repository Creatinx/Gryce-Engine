#version 450 core

// HiZ 构建：精确 2x2 最小值下采样（Vulkan 版，与 ssr_hiz.frag 同一套算法）。
// 用 texelFetch + 整数纹素坐标，过滤/环绕模式不参与计算，结果与 GL 逐像素一致。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uInput; // 上一级：depth(level 0) 或 HiZ[i-1]

// 与 C++ VulkanShader::SSRPushData 严格对齐（std430，128 字节）。本 pass 不使用
// 其中的任何字段（尺寸从 textureSize 取），但块声明必须与 C++ 一致。
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
    ivec2 in_size = textureSize(uInput, 0);
    if (in_size.x <= 0 || in_size.y <= 0) {
        FragColor = vec4(1.0);
        return;
    }
    ivec2 dst = ivec2(gl_FragCoord.xy);
    ivec2 base = dst * 2;
    ivec2 max_xy = in_size - ivec2(1);
    ivec2 c00 = clamp(base, ivec2(0), max_xy);
    ivec2 c10 = clamp(base + ivec2(1, 0), ivec2(0), max_xy);
    ivec2 c01 = clamp(base + ivec2(0, 1), ivec2(0), max_xy);
    ivec2 c11 = clamp(base + ivec2(1, 1), ivec2(0), max_xy);

    float d00 = texelFetch(uInput, c00, 0).r;
    float d10 = texelFetch(uInput, c10, 0).r;
    float d01 = texelFetch(uInput, c01, 0).r;
    float d11 = texelFetch(uInput, c11, 0).r;
    FragColor = vec4(min(min(d00, d10), min(d01, d11)));
}
