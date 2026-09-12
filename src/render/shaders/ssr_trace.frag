#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// SSR 主 pass：层级 Z 缓冲加速的屏幕空间光线步进（参考 Godot）。
// - 从 depth_normal 重建世界位置 / 法线 / 粗糙度
// - 计算镜面反射方向，在 HiZ 中从粗层向细层步进
// - 命中后采样该屏幕位置的颜色作为反射色

uniform sampler2D uColorTex;       // HDR 场景颜色
uniform sampler2D uDepthTex;       // 原始深度（非线性 [0,1]）
uniform sampler2D uNormalRoughTex; // RGB = N*0.5+0.5, A = roughness
uniform sampler2D uHiZ0;
uniform sampler2D uHiZ1;
uniform sampler2D uHiZ2;
uniform sampler2D uHiZ3;

uniform mat4 uView;       // 世界 → 视图
uniform vec3 uCameraPos;  // 世界空间相机位置
uniform vec2 uScreenSize; // 视口尺寸（像素）

// 由管线通过 set_post_process_params 同步
uniform float uSSRNear;
uniform float uSSRFar;
uniform float uSSRTanHalfFov;
uniform float uSSRAspect;
uniform float uSSRMaxRoughness;
uniform int   uSSRMaxSteps;
uniform float uSSRThickness;

const int kHiZLevels = 4;

float linearize_depth(float d) {
    // 深度纹理里存的是视图变换后的 [0,1] 深度（NDC z 已映射）
    return (2.0 * uSSRNear * uSSRFar) /
           (uSSRFar + uSSRNear - d * (uSSRFar - uSSRNear));
}

// 读取指定 HiZ 层的 2x2 最小深度（GLSL 330 不允许 sampler 作为函数返回值，
// 因此直接在函数内采样并返回深度值）
float sample_hiz_depth(int level, vec2 uv) {
    if (level == 0) return texture(uHiZ0, uv).r;
    if (level == 1) return texture(uHiZ1, uv).r;
    if (level == 2) return texture(uHiZ2, uv).r;
    return texture(uHiZ3, uv).r;
}

// 世界坐标 → 屏幕 uv；相机后方返回 false，同时输出视图深度（正）
bool project_world(vec3 world_pos, out vec2 uv, out float view_depth) {
    vec4 v = uView * vec4(world_pos, 1.0);
    if (v.z >= -0.001) return false;
    float neg_z = -v.z;
    vec2 ndc = vec2(v.x / neg_z / uSSRTanHalfFov / uSSRAspect,
                    v.y / neg_z / uSSRTanHalfFov);
    uv = ndc * 0.5 + 0.5;
    view_depth = neg_z;
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

void main() {
    vec4 nr = texture(uNormalRoughTex, vTexCoord);
    vec3 N = normalize(nr.rgb * 2.0 - 1.0);
    float roughness = nr.a;
    if (roughness > uSSRMaxRoughness) {
        FragColor = vec4(0.0);
        return;
    }

    // 重建世界位置：视图空间反投影 → 世界
    float d = texture(uDepthTex, vTexCoord).r;
    float lin = linearize_depth(d);
    if (lin < uSSRNear * 0.99 || lin > uSSRFar * 0.99) {
        FragColor = vec4(0.0);
        return;
    }
    vec2 ndc = vTexCoord * 2.0 - 1.0;
    vec3 view_pos = vec3(ndc.x * uSSRTanHalfFov * uSSRAspect * lin,
                         ndc.y * uSSRTanHalfFov * lin,
                         -lin);
    mat4 inv_view = inverse(uView);
    vec4 world_pos = inv_view * vec4(view_pos, 1.0);
    vec3 P = world_pos.xyz / world_pos.w;

    // 镜面反射方向（世界空间）
    vec3 V = normalize(uCameraPos - P);
    if (dot(N, V) <= 0.02) {
        FragColor = vec4(0.0);
        return;
    }
    vec3 R = reflect(-V, N);

    // P 处一个屏幕像素对应的世界距离（用于换算步长）
    float px_len = lin * uSSRTanHalfFov * 2.0 / uScreenSize.y;

    // 层级步进：从最粗层开始大步长，命中候选后回退并换更细层逼近
    int level = kHiZLevels - 1;
    float t = 0.0;
    bool hit = false;
    vec2 hit_uv = vec2(0.0);
    float hit_dist = 0.0;

    for (int i = 0; i < uSSRMaxSteps; ++i) {
        float step = px_len * exp2(float(level)); // 粗层大步长
        t += step;
        if (t > uSSRFar) break;

        vec3 Q = P + R * t;
        vec2 uv;
        float view_depth;
        if (!project_world(Q, uv, view_depth)) break;

        // 该层 2x2 最小深度（线性化）；天空/无几何处为远平面值，永不命中
        float hiz_lin = linearize_depth(sample_hiz_depth(level, uv));
        if (view_depth > hiz_lin - uSSRThickness) {
            // 光线穿入表面 → 命中候选：细化到更细层
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

    // 衰减：粗糙度越高越弱、距离越远越弱、屏幕边缘渐隐
    float rough_fade = 1.0 - clamp(roughness / uSSRMaxRoughness, 0.0, 1.0);
    rough_fade = rough_fade * rough_fade;
    float dist_fade = 1.0 - clamp(hit_dist / (uSSRFar * 0.5), 0.0, 1.0);
    float edge = 1.0 - clamp(distance(hit_uv, vec2(0.5)) * 2.0 - 0.7, 0.0, 1.0);
    float fade = rough_fade * dist_fade * edge * edge;

    FragColor = vec4(color * fade, fade);
}
