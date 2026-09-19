#version 450 core

// SSIL 主 pass：半球重要性采样 + 屏幕空间步进（参考 Godot SSIL）。
// - 从深度 / 法线重建世界位置与法线（半分辨率）
// - 以 N 为轴撒 cos 加权的半球方向，屏幕空间步进找遮挡物
// - 命中后采样该屏幕位置的 HDR 颜色作为间接光贡献
// 逻辑与 GL 版 ssil_trace.frag 一致，仅替换为 Vulkan 方言：
// descriptor binding + push constants。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

// descriptor binding 映射（post_process_binding(slot)）：
//   0  = uColorTex (kTonemapHDR)
//   11 = uDepthTex (kPBRShadowDepth)
//   12 = uNormalRoughTex (kPBRShadowDepth1)
layout(binding = 0)  uniform sampler2D uColorTex;
layout(binding = 11) uniform sampler2D uDepthTex;
layout(binding = 12) uniform sampler2D uNormalRoughTex;

// 与 C++ VulkanShader::SSILPushData 严格对齐（std430，128 字节）
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

const int kMaxRays = 8;        // 每像素光线数（半球方向）

float linearize_depth(float d) {
    return (2.0 * pc.near_plane * pc.far_plane) /
           (pc.far_plane + pc.near_plane - d * (pc.far_plane - pc.near_plane));
}

// 世界坐标 → 屏幕 uv；相机后方返回 false
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

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

void main() {
    float raw = texture(uDepthTex, vUV).r;
    float lin = linearize_depth(raw);
    // 天空/远平面：没有几何，不产生间接光
    if (lin < pc.near_plane * 0.99 || lin >= pc.far_plane * 0.99) {
        FragColor = vec4(0.0);
        return;
    }

    vec4 nr = texture(uNormalRoughTex, vUV);
    vec3 N = normalize(nr.rgb * 2.0 - 1.0);

    // 重建世界坐标（视图空间反投影）
    vec2 ndc = vUV * 2.0 - 1.0;
    vec3 view_pos = vec3(ndc.x * pc.tan_half_fov * pc.aspect * lin,
                         ndc.y * pc.tan_half_fov * lin,
                         -lin);
    vec4 world_pos = inverse(pc.view) * vec4(view_pos, 1.0);
    vec3 P = world_pos.xyz / world_pos.w;

    // 以 N 为轴的切线基（避免与 N 平行时退化）
    vec3 up_axis = abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up_axis, N));
    vec3 B = cross(N, T);

    // 逐像素随机旋转，避免条带
    float ang = hash12(vUV * pc.screen_size) * 6.2831853;
    const float kGoldenAngle = 2.39996323;

    float step_len = pc.radius / float(pc.steps);
    vec3 acc = vec3(0.0);
    float wsum = 0.0;

    for (int r = 0; r < kMaxRays; ++r) {
        // cos(theta) 均匀分布 + 黄金角旋转 → 低差异半球采样
        float ct = float(r + 1) / float(kMaxRays + 1);
        float st = sqrt(max(0.0, 1.0 - ct * ct));
        float phi = ang + float(r) * kGoldenAngle;
        vec3 L = T * (st * cos(phi)) + B * (st * sin(phi)) + N * ct;

        float t = step_len;
        for (int s = 0; s < pc.steps; ++s, t += step_len) {
            vec3 Q = P + L * t;
            vec2 uv;
            float ray_depth;
            if (!project_world(Q, uv, ray_depth)) break;

            float sample_lin = linearize_depth(texture(uDepthTex, uv).r);
            // 射线穿到几何后方（且没跑出采样半径）→ 这里存在遮挡物，
            // 取它的颜色作为该方向的间接光来源。
            if (ray_depth > sample_lin + 0.02 && ray_depth < sample_lin + pc.radius) {
                vec3 c = texture(uColorTex, uv).rgb;
                // cos 权重（能量）+ 距离衰减（越远贡献越弱）
                float w = ct / (1.0 + t * t * 4.0);
                acc += c * w;
                wsum += w;
                break;
            }
        }
    }

    // 归一化时按"实际命中比例"补偿：命中越少的方向，说明该处越开阔，
    // 间接光本来就应该弱，不回填固定值以免整体变亮。
    vec3 gi = wsum > 0.0 ? (acc / wsum) * pc.intensity * (wsum / float(kMaxRays)) : vec3(0.0);
    FragColor = vec4(gi, 1.0);
}
