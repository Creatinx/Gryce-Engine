#version 450 core

// 体积雾合成 shader：将 fog 体积纹理合成到场景颜色。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

// binding 必须与 VulkanShader::post_process_binding 的映射一致：
// 0 = uSceneColor（fog.cpp 用 kTonemapHDR）、10 = uFogTex（kSSRTexture）、
// 11 = uDepthTex（kPBRShadowDepth，后处理固定描述符集里"深度"的位置）。
layout(binding = 0) uniform sampler2D uSceneColor;
layout(binding = 10) uniform sampler2D uFogTex;
layout(binding = 11) uniform sampler2D uDepthTex;

layout(push_constant) uniform PushConstants {
    mat4 inv_view_proj; // +0
    mat4 view_matrix;   // +64
    vec3 camera_pos;    // +128
    vec3 fog_color;     // +144 (std430)
    float density;      // +156
    float height;       // +160
    vec2 fog_range;     // +168
    vec2 screen_size;   // +176
    int slice_count;    // +184
    int slice_index;    // +188
} pc;

// 从深度重建世界位置。
// Vulkan 的 NDC z 已经是 [0,1]，深度缓冲里存的直接就是 NDC z，因此这里必须
// 原样使用 depth；沿用 GL 的 depth*2-1 会把输入压到 [-1,1]，反解出的距离只有
// 真实值的一半（A/B 实测：GL 与 Vulkan 的 dist 均值从差一倍变为完全一致）。
vec3 world_from_depth(float depth, vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = pc.inv_view_proj * ndc;
    return world.xyz / world.w;
}

void main() {
    vec3 scene_color = texture(uSceneColor, vUV).rgb;
    float depth = texture(uDepthTex, vUV).r;

    if (depth >= 1.0) {
        FragColor = vec4(scene_color, 1.0);
        return;
    }

    vec3 world_pos = world_from_depth(depth, vUV);
    float dist = length(world_pos - pc.camera_pos);

    // 计算切片索引。fog.frag 的切片 i 覆盖距离 [(i/N)*far, ((i+1)/N)*far]，
    // 因此必须把视图距离按 far 归一化后再乘切片数（直接用 dist/N 会让索引恒为 0）。
    float slice_f = clamp(dist / max(pc.fog_range.y, 0.0001), 0.0, 1.0) * float(pc.slice_count);
    float slice_idx = clamp(floor(slice_f), 0.0, float(pc.slice_count - 1));

    float slice_w = 1.0 / float(pc.slice_count);
    vec2 fog_uv = vec2(vUV.x * slice_w + slice_idx * slice_w, vUV.y);
    vec4 fog_sample = texture(uFogTex, fog_uv);

    // 与 GL 版一致：fog.frag 输出预乘结果（rgb=累积散射色，a=1-透射率）。
    vec3 final_color = scene_color * (1.0 - fog_sample.a) + fog_sample.rgb;
    FragColor = vec4(final_color, 1.0);
}