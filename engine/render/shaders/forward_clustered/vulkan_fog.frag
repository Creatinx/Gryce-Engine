#version 450 core

// 体积雾渲染 shader：从深度缓冲重建世界位置，逐体素计算雾密度。
// 输出到 fog 3D 体积（切片排列为 2D 纹理）。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FogColor;

// 深度纹理。binding 必须与 VulkanShader::post_process_binding 保持一致：
// fog.cpp 用 TextureSlots::kPBRShadowDepth 绑定，该槽位映射到 binding 11
//（后处理固定描述符集里"深度"占用的位置）。
layout(binding = 11) uniform sampler2D uDepthTex;

// 对应 C++ VulkanShader::FogPushData（192 字节）。两边的字段偏移必须严格一致：
// std430 下 vec3 按 16 字节对齐（camera_pos 后需要 4 字节填充），若按 C++ 自然
// 布局摆放，着色器读到的 density/height/fog_range 会整体错位一格，雾参数全部
// 变成别的字段（表现为整屏被雾色淹没）。
layout(push_constant) uniform PushConstants {
    mat4 inv_view_proj; // +0
    mat4 view_matrix;   // +64
    vec3 camera_pos;    // +128
    vec3 fog_color;     // +144
    float density;      // +156
    float height;       // +160
    vec2 fog_range;     // +168
    vec2 screen_size;   // +176
    int slice_count;    // +184
    int slice_index;    // +188
} pc;

// 从深度重建世界位置。
// Vulkan 的 NDC z 已经是 [0,1]，深度缓冲里存的直接就是 NDC z，必须原样使用
// depth（GL 版才需要 depth*2-1 的窗口深度 → NDC 换算）。
vec3 world_from_depth(float depth, vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = pc.inv_view_proj * ndc;
    return world.xyz / world.w;
}

float compute_fog_density(vec3 world_pos, float step_size) {
    float height = world_pos.y;
    float height_factor = exp(-abs(height) / max(pc.height, 0.01));
    float dist = length(world_pos - pc.camera_pos);
    float dist_factor = clamp((dist - pc.fog_range.x) / (pc.fog_range.y - pc.fog_range.x), 0.0, 1.0);
    return pc.density * height_factor * dist_factor * step_size;
}

void main() {
    float depth = texture(uDepthTex, vUV).r;
    if (depth >= 1.0) {
        FogColor = vec4(0.0);
        return;
    }

    vec3 world_pos = world_from_depth(depth, vUV);
    vec3 view_dir = normalize(world_pos - pc.camera_pos);
    float view_dist = length(world_pos - pc.camera_pos);

    float slice_count = float(pc.slice_count);
    float slice_idx = float(pc.slice_index);
    float slice_start = (slice_idx / slice_count) * pc.fog_range.y;
    float slice_end = ((slice_idx + 1.0) / slice_count) * pc.fog_range.y;

    float start_t = max(0.0, (slice_start - pc.fog_range.x) / max(view_dist - pc.fog_range.x, 0.001));
    float end_t = min(1.0, (slice_end - pc.fog_range.x) / max(view_dist - pc.fog_range.x, 0.001));

    const int k_steps = 8;
    float step_size = (end_t - start_t) / float(k_steps);
    vec3 accum_color = vec3(0.0);
    float transmittance = 1.0;

    for (int i = 0; i < k_steps; ++i) {
        float t = start_t + (float(i) + 0.5) * step_size;
        vec3 sample_pos = pc.camera_pos + view_dir * (pc.fog_range.x + t * (view_dist - pc.fog_range.x));
        float density = compute_fog_density(sample_pos, step_size * view_dist);
        if (density > 0.0) {
            // 与 GL 版一致的标准单次散射累积（详见 fog.frag 的说明）。
            float sample_trans = exp(-density);
            accum_color += pc.fog_color * (1.0 - sample_trans) * transmittance;
            transmittance *= sample_trans;
        }
    }

    FogColor = vec4(accum_color, 1.0 - transmittance);
}