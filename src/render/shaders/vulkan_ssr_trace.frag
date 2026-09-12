#version 450 core

// SSR 主 pass：层级 Z 缓冲加速的屏幕空间光线步进（参考 Godot）。
// - 从 depth / normal_roughness 重建世界位置 / 法线 / 粗糙度
// - 计算镜面反射方向，在 HiZ 中从粗层向细层步进
// - 命中后采样该屏幕位置的颜色作为反射色

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

// descriptor binding 映射（post_process_binding(slot)）：
//   0 = uColorTex (kTonemapHDR)
//   11 = uDepthTex (kPBRShadowDepth)
//   12 = uNormalRoughTex (kPBRShadowDepth1)
//   6..9 = uHiZ0..uHiZ3 (kSSRHiZ+0..3)
layout(binding = 0)  uniform sampler2D uColorTex;
layout(binding = 11) uniform sampler2D uDepthTex;
layout(binding = 12) uniform sampler2D uNormalRoughTex;
layout(binding = 6)  uniform sampler2D uHiZ0;
layout(binding = 7)  uniform sampler2D uHiZ1;
layout(binding = 8)  uniform sampler2D uHiZ2;
layout(binding = 9)  uniform sampler2D uHiZ3;

// 与 C++ VulkanShader::SSRPushData 严格对齐（std430，128 字节）
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

const int kHiZLevels = 4;

float linearize_depth(float d) {
    return (2.0 * pc.near_plane * pc.far_plane) /
           (pc.far_plane + pc.near_plane - d * (pc.far_plane - pc.near_plane));
}

float sample_hiz_depth(int level, vec2 uv) {
    if (level == 0) return texture(uHiZ0, uv).r;
    if (level == 1) return texture(uHiZ1, uv).r;
    if (level == 2) return texture(uHiZ2, uv).r;
    return texture(uHiZ3, uv).r;
}

// 世界坐标 → 屏幕 uv；相机后方返回 false，同时输出视图深度（正）
bool project_world(vec3 world_pos, out vec2 uv, out float view_depth) {
    vec4 v = pc.view * vec4(world_pos, 1.0);
    if (v.z >= -0.001) return false;
    float neg_z = -v.z;
    vec2 ndc = vec2(v.x / neg_z / pc.tan_half_fov / pc.aspect,
                    v.y / neg_z / pc.tan_half_fov);
    uv = ndc * 0.5 + 0.5;
    view_depth = neg_z;
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

void main() {
    vec4 nr = texture(uNormalRoughTex, vUV);
    vec3 N = normalize(nr.rgb * 2.0 - 1.0);
    float roughness = nr.a;
    if (roughness > pc.max_roughness) {
        FragColor = vec4(0.0);
        return;
    }

    float d = texture(uDepthTex, vUV).r;
    float lin = linearize_depth(d);
    if (lin < pc.near_plane * 0.99 || lin > pc.far_plane * 0.99) {
        FragColor = vec4(0.0);
        return;
    }
    vec2 ndc = vUV * 2.0 - 1.0;
    vec3 view_pos = vec3(ndc.x * pc.tan_half_fov * pc.aspect * lin,
                         ndc.y * pc.tan_half_fov * lin,
                         -lin);
    mat4 inv_view = inverse(pc.view);
    vec4 world_pos = inv_view * vec4(view_pos, 1.0);
    vec3 P = world_pos.xyz / world_pos.w;

    vec3 V = normalize(pc.camera_pos - P);
    if (dot(N, V) <= 0.02) {
        FragColor = vec4(0.0);
        return;
    }
    vec3 R = reflect(-V, N);

    float px_len = lin * pc.tan_half_fov * 2.0 / pc.screen_size.y;

    int level = kHiZLevels - 1;
    float t = 0.0;
    bool hit = false;
    vec2 hit_uv = vec2(0.0);
    float hit_dist = 0.0;

    for (int i = 0; i < pc.max_steps; ++i) {
        float step = px_len * exp2(float(level));
        t += step;
        if (t > pc.far_plane) break;

        vec3 Q = P + R * t;
        vec2 uv;
        float view_depth;
        if (!project_world(Q, uv, view_depth)) break;

        float hiz_lin = linearize_depth(sample_hiz_depth(level, uv));
        if (view_depth > hiz_lin - pc.thickness) {
            if (level > 0) {
                level--;
                t -= step * 0.5;
            } else {
                hit = true;
                hit_uv = uv;
                hit_dist = t;
                break;
            }
        }
    }

    if (!hit) {
        FragColor = vec4(0.0);
        return;
    }

    vec3 color = texture(uColorTex, hit_uv).rgb;

    float rough_fade = 1.0 - clamp(roughness / pc.max_roughness, 0.0, 1.0);
    rough_fade = rough_fade * rough_fade;
    float dist_fade = 1.0 - clamp(hit_dist / (pc.far_plane * 0.5), 0.0, 1.0);
    float edge = 1.0 - clamp(distance(hit_uv, vec2(0.5)) * 2.0 - 0.7, 0.0, 1.0);
    float fade = rough_fade * dist_fade * edge * edge;

    FragColor = vec4(color * fade, fade);
}