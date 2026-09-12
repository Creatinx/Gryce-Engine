#version 450 core

// 体积雾渲染 shader：从深度缓冲重建世界位置，逐体素计算雾密度。
// 输出到 fog 3D 体积（切片排列为 2D 纹理）。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FogColor;

// 0 = uDepthTex（fog.cpp 以 slot 0 绑定深度）
layout(binding = 0) uniform sampler2D uDepthTex;

// 对应 C++ VulkanShader::FogPushData（192 字节）。注意 std430 下 vec3 为 16 字节对齐，
// 与 C++ 自然布局存在偏移差异；体积雾默认关闭，不影响加载。
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
    float _pad;         // +192
} pc;

// 从深度重建世界位置
vec3 world_from_depth(float depth, vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
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
            float sample_trans = exp(-density);
            accum_color += pc.fog_color * density * transmittance * step_size * view_dist;
            transmittance *= sample_trans;
        }
    }

    FogColor = vec4(accum_color, 1.0 - transmittance);
}