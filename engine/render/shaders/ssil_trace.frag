#version 330 core

// [Shader 阶段] Fragment Shader
// [功能] SSIL（屏幕空间间接光）—— 半球重要性采样 + 屏幕空间步进。
//
// 参考 Godot 的 SSIL 思路做了工程化简化：
//   1. 从深度 + 法线重建世界坐标 P 与法线 N（半分辨率，降低开销）
//   2. 以 N 为轴建立局部坐标系，按黄金角撒 N 条半球方向（cos 加权）
//   3. 每条方向在屏幕空间步进：若某步的射线深度落在该处几何之后
//      （且在采样半径内），说明该方向的间接光被这块几何反射回来，
//      于是取该屏幕位置的 HDR 场景色作为间接光贡献
//   4. 累积结果由后续 ssil_blur 做深度感知模糊去噪，最后经 GI 通道
//      作为间接漫反射光参与 PBR 着色

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uColorTex;       // HDR 场景颜色
uniform sampler2D uDepthTex;       // 原始深度（非线性 [0,1]）
uniform sampler2D uNormalRoughTex; // RGB = N*0.5+0.5, A = roughness

uniform mat4  uView;       // 世界 → 视图
uniform vec3  uCameraPos;  // 世界空间相机位置
uniform vec2  uScreenSize; // 视口尺寸（像素）

uniform float uSSILNear;
uniform float uSSILFar;
uniform float uSSILTanHalfFov;
uniform float uSSILAspect;
uniform float uSSILRadius;     // 采样半径（世界单位）
uniform float uSSILIntensity;  // 间接光强度
uniform int   uSSILSteps;      // 每条光线的步进次数

const int kMaxRays = 8;        // 每像素光线数（半球方向）

float linearize_depth(float d) {
    // 深度纹理存的是 w = far*(z-near)/((far-near)*z)（见 SSR 的同名函数）
    return (uSSILNear * uSSILFar) / max(uSSILFar - d * (uSSILFar - uSSILNear), 1e-6);
}

// 世界坐标 → 屏幕 uv；相机后方返回 false
bool project_world(vec3 world_pos, out vec2 uv, out float view_depth) {
    vec4 v = uView * vec4(world_pos, 1.0);
    if (v.z >= -0.001) return false;
    float neg_z = -v.z;
    vec2 ndc = vec2(v.x / neg_z / uSSILTanHalfFov / uSSILAspect,
                    v.y / neg_z / uSSILTanHalfFov);
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
    float raw = texture(uDepthTex, vTexCoord).r;
    float lin = linearize_depth(raw);
    // 天空/远平面：没有几何，不产生间接光
    if (lin < uSSILNear * 0.99 || lin >= uSSILFar * 0.99) {
        FragColor = vec4(0.0);
        return;
    }

    vec4 nr = texture(uNormalRoughTex, vTexCoord);
    vec3 N = normalize(nr.rgb * 2.0 - 1.0);

    // 重建世界坐标（视图空间反投影）
    vec2 ndc = vTexCoord * 2.0 - 1.0;
    vec3 view_pos = vec3(ndc.x * uSSILTanHalfFov * uSSILAspect * lin,
                         ndc.y * uSSILTanHalfFov * lin,
                         -lin);
    vec4 world_pos = inverse(uView) * vec4(view_pos, 1.0);
    vec3 P = world_pos.xyz / world_pos.w;

    // 以 N 为轴的切线基（避免与 N 平行时退化）
    vec3 up_axis = abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(up_axis, N));
    vec3 B = cross(N, T);

    // 逐像素随机旋转，避免条带
    float ang = hash12(vTexCoord * uScreenSize) * 6.2831853;
    const float kGoldenAngle = 2.39996323;

    float step_len = uSSILRadius / float(uSSILSteps);
    vec3 acc = vec3(0.0);
    float wsum = 0.0;

    for (int r = 0; r < kMaxRays; ++r) {
        // cos(theta) 均匀分布 + 黄金角旋转 → 低差异半球采样
        float ct = float(r + 1) / float(kMaxRays + 1);
        float st = sqrt(max(0.0, 1.0 - ct * ct));
        float phi = ang + float(r) * kGoldenAngle;
        vec3 L = T * (st * cos(phi)) + B * (st * sin(phi)) + N * ct;

        float t = step_len;
        for (int s = 0; s < uSSILSteps; ++s, t += step_len) {
            vec3 Q = P + L * t;
            vec2 uv;
            float ray_depth;
            if (!project_world(Q, uv, ray_depth)) break;

            float sample_lin = linearize_depth(texture(uDepthTex, uv).r);
            // 射线穿到几何后方（且没跑出采样半径）→ 这里存在遮挡物，
            // 取它的颜色作为该方向的间接光来源。
            // 下限阈值用 max(lin * 0.02, 0.005) 做相对深度阈值：
            //   近景 lin=0.5 → 下限 0.01m；远景 lin=100 → 下限 2m
            // 避免硬编码 0.02m 在远景下太小导致每步都被误判为命中，
            // 同时避免在近景下太大导致真正的遮挡被漏掉。
            float eps = max(lin * 0.02, 0.005);
            if (ray_depth > sample_lin + eps && ray_depth < sample_lin + uSSILRadius) {
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
    vec3 gi = wsum > 0.0 ? (acc / wsum) * uSSILIntensity * (wsum / float(kMaxRays)) : vec3(0.0);
    FragColor = vec4(gi, 1.0);
}
