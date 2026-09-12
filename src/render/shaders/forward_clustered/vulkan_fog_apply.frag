#version 450 core

// 体积雾合成 shader：将 fog 体积纹理合成到场景颜色。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

// 0 = uSceneColor, 1 = uFogTex, 2 = uDepthTex（fog.cpp render_apply slot 0/1/2）
layout(binding = 0) uniform sampler2D uSceneColor;
layout(binding = 1) uniform sampler2D uFogTex;
layout(binding = 2) uniform sampler2D uDepthTex;

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

vec3 world_from_depth(float depth, vec2 uv) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
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

    float slice_idx = dist / float(pc.slice_count);
    slice_idx = clamp(slice_idx, 0.0, float(pc.slice_count - 1));

    float slice_w = 1.0 / float(pc.slice_count);
    vec2 fog_uv = vec2(vUV.x * slice_w + slice_idx * slice_w, vUV.y);
    vec4 fog_sample = texture(uFogTex, fog_uv);

    vec3 final_color = mix(scene_color, fog_sample.rgb, fog_sample.a);
    FragColor = vec4(final_color, 1.0);
}