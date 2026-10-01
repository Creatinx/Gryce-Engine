#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// SSR 主 pass（Vulkan 版；与 ssr_trace.frag 算法一致）。
// 深度跨度 >3x 时切换到射线参数空间精确步进，避免 1/z 插值误差。

layout(binding = 0)  uniform sampler2D uColorTex;
layout(binding = 11) uniform sampler2D uDepthTex;
layout(binding = 12) uniform sampler2D uNormalRoughTex;
// 反射探针图集（与 GL 版共用同一个 slot，见 post_process_binding）。
layout(binding = 13) uniform sampler2D uProbeAtlas;

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
    float env_fallback;     // +128（composite 使用；trace 不读，占位对齐偏移）
    int probe_valid;        // +132（=0 时退化射线探针兜底关闭）
} pc;

// 反射探针图集：6 个面按 3x2 排布（+X,-X,+Y,-Y,+Z,-Z），每面 1024x1024。
// 面的朝向与 RenderPipeline::capture_reflection_probe 的 6 组
// look_at(eye, eye+forward, up) 一一对应；这里的 (u,v) 组合必须与之一致。
//
// Vulkan 差异：VK 用负 viewport height，离屏目标里"屏幕上沿"落在 v=0
//（GL 落在 v=1）。图集格子（3x2 的行）排布两端一致，但格子内部的 v 相反，
// 故只在面内翻转 v，不动 cell。
const float kProbeFaceSize = 1024.0;
const vec2  kProbeAtlasGrid = vec2(3.0, 2.0);

vec3 probe_sample(vec3 dir) {
    vec3 a = abs(dir);
    vec2 uv;
    vec2 cell;
    if (a.x >= a.y && a.x >= a.z) {
        if (dir.x > 0.0) { uv = vec2(-dir.z, -dir.y) / a.x; cell = vec2(0.0, 0.0); }
        else             { uv = vec2( dir.z, -dir.y) / a.x; cell = vec2(1.0, 0.0); }
    } else if (a.y >= a.z) {
        if (dir.y > 0.0) { uv = vec2( dir.x,  dir.z) / a.y; cell = vec2(2.0, 0.0); }
        else             { uv = vec2( dir.x, -dir.z) / a.y; cell = vec2(0.0, 1.0); }
    } else {
        if (dir.z > 0.0) { uv = vec2( dir.x, -dir.y) / a.z; cell = vec2(1.0, 1.0); }
        else             { uv = vec2(-dir.x, -dir.y) / a.z; cell = vec2(2.0, 1.0); }
    }
    uv = uv * 0.5 + 0.5;
    // VK：屏幕(cell 内)上沿在 v=0，与 GL 相反 -> 面内翻转（必需，去掉后采样错位）
    uv.y = 1.0 - uv.y;
    // 内缩半个纹素，避免跨面采到相邻格子
    const float inset = 0.5 / kProbeFaceSize;
    uv = clamp(uv, vec2(inset), vec2(1.0 - inset));
    return texture(uProbeAtlas, (cell + uv) / kProbeAtlasGrid).rgb;
}

// 退化/未命中射线的探针兜底：按反射方向取相机锚定探针的环境色。
// ray_depth 沿用命中区的距离衰减，保证与命中区覆盖度连续；探针不可用时返回
// 零覆盖（等价旧行为，交给 IBL），不会把场景压暗。
vec4 probe_fallback(vec3 R, float ray_depth) {
    if (pc.probe_valid == 0) return vec4(0.0);
    vec3 c = probe_sample(R);
    float cov = 1.0 - smoothstep(pc.far_plane * 0.25, pc.far_plane * 0.75,
                                 max(ray_depth, 1e-4));
    return vec4(c, cov);
}

float linearize_depth(float w) {
    return (pc.near_plane * pc.far_plane) /
           max(pc.far_plane - w * (pc.far_plane - pc.near_plane), 1e-6);
}

ivec2 self_texel(vec2 src_size) {
    return ivec2(floor(vec2(vTexCoord.x, vTexCoord.y) * src_size));
}

float scene_depth(vec2 uv) {
    vec2 src = vec2(textureSize(uDepthTex, 0));
    vec2 img = (floor(vec2(uv.x, uv.y) * src) + vec2(0.5)) / src;
    return linearize_depth(texture(uDepthTex, img).r);
}

// VK 版射线参数精确采样（vUV y 向下，端点 UV 需翻转 y）
struct RaySample { float invz; vec2 uv; float ray_depth; };
RaySample ray_sample(float V0z_local, float Dvz_local, float V0x_local, float Dvx_local,
                     float V0y_local, float Dvy_local,
                     float ta_local, float tb_local, float t) {
    float Vz = V0z_local + t * Dvz_local;
    float Vx = V0x_local + t * Dvx_local;
    float Vy = V0y_local + t * Dvy_local;
    float z_view = max(-Vz, 1e-4);
    float invz_exact = 1.0 / z_view;
    vec2 ndc = vec2(Vx / z_view / ta_local, Vy / z_view / tb_local);
    // VK vUV: y 向下，所以 uv.y = 0.5 - ndc.y * 0.5
    vec2 uv = clamp(vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5), vec2(0.0), vec2(1.0));
    return RaySample(invz_exact, uv, z_view);
}

void main() {
    ivec2 self = self_texel(vec2(textureSize(uDepthTex, 0)));
    vec4 nr = texelFetch(uNormalRoughTex, self, 0);
    vec3 N = normalize(nr.rgb * 2.0 - 1.0);
    float roughness = nr.a;
    if (roughness >= pc.max_roughness) {
        FragColor = vec4(0.0);
        return;
    }

    float lin = linearize_depth(texelFetch(uDepthTex, self, 0).r);
    if (lin <= pc.near_plane || lin >= pc.far_plane * 0.999) {
        FragColor = vec4(0.0);
        return;
    }
    // VK NDC: y 翻转（v=0 在顶端）
    vec2 ndc = vec2(vTexCoord.x * 2.0 - 1.0, 1.0 - vTexCoord.y * 2.0);
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

    vec3 V0 = (pc.view * vec4(P, 1.0)).xyz;
    vec3 Dv = (pc.view * vec4(R, 0.0)).xyz;
    float invz0 = 1.0 / max(lin, 1e-4);

    vec2 uv0 = vTexCoord;
    float ta = pc.tan_half_fov * pc.aspect;
    float tb = pc.tan_half_fov;

    // 端点参数 t_end
    float t_end = pc.far_plane * 0.99;
    if (Dv.z < -1e-6) {
        t_end = min(t_end, (pc.far_plane + V0.z) / -Dv.z);
    } else if (Dv.z > 1e-6) {
        t_end = min(t_end, (-V0.z - 1e-4) / Dv.z);
    }
    // 边界平面法向量（ndc_x = ±1, ndc_y = ±1 对应关系说明见 GL 版注释）
    vec3 plane_n[4];
    plane_n[0] = vec3(1.0, 0.0, -ta);  // 左边界 ndc_x = -1
    plane_n[1] = vec3(1.0, 0.0,  ta);  // 右边界 ndc_x = +1
    plane_n[2] = vec3(0.0, 1.0,  tb);  // 上边界 ndc_y = +1
    plane_n[3] = vec3(0.0, 1.0, -tb);  // 下边界 ndc_y = -1
    for (int i = 0; i < 4; ++i) {
        float den = dot(plane_n[i], Dv);
        if (abs(den) > 1e-9) {
            float tp = -dot(plane_n[i], V0) / den;
            if (tp > 1e-4) t_end = min(t_end, tp);
        }
    }
    if (t_end <= 1e-4) {
        // 射线在近平面处即刻终止（反射面几乎贴住相机）：屏幕空间无可用数据，
        // 改走探针兜底，避免贴近物体时反射整片消失成黑色。
        FragColor = probe_fallback(R, lin);
        return;
    }

    // 端点（精确射线参数）
    RaySample end_sample = ray_sample(V0.z, Dv.z, V0.x, Dv.x, V0.y, Dv.y,
                                      ta, tb, t_end);
    float invz1 = end_sample.invz;

    vec2 seg = end_sample.uv - uv0;
    float px_total = length(seg * pc.screen_size);
    if (px_total < 1.0) {
        // 反射方向几乎正对相机（贴近竖直镜面时反射折回相机）→ 屏幕投影位移不足
        // 1 像素，屏幕空间步进无意义。这类射线正是"贴近物体"的主场景，改走探针
        // 兜底，从而反射出相机周围（含相机背后、屏幕上完全看不见）的真实环境。
        FragColor = probe_fallback(R, end_sample.ray_depth);
        return;
    }

    // 深度跨度 >3x 时切换到射线参数空间精确步进
    float depth_ratio = end_sample.ray_depth / lin;
    bool use_t_space = (depth_ratio > 3.0) || (depth_ratio < 1.0/3.0);
    float t_per_pixel = t_end / px_total;

    bool hit = false;
    vec2 hit_uv = vec2(0.0);
    float hit_dist = 0.0;

    vec2 uv_prev = uv0;
    float invz_prev = invz0;
    float prev_ray_depth = lin;
    float prev_scene = scene_depth(uv0);
    float px_acc = 0.0;
    float step_px = 1.0;

    for (int i = 0; i < pc.max_steps; ++i) {
        if (px_acc >= px_total - 0.5) break;
        step_px = min(step_px * 1.35, 32.0);
        px_acc = min(px_acc + step_px, px_total);

        vec2 uv;
        float invz;
        float ray_depth;

        if (use_t_space) {
            float t = px_acc * t_per_pixel;
            RaySample smp = ray_sample(V0.z, Dv.z, V0.x, Dv.x, V0.y, Dv.y,
                                       ta, tb, t);
            uv = smp.uv;
            invz = smp.invz;
            ray_depth = smp.ray_depth;
        } else {
            float s = px_acc / px_total;
            uv = mix(uv0, end_sample.uv, s);
            invz = mix(invz0, invz1, s);
            ray_depth = 1.0 / invz;
        }

        float scene = scene_depth(uv);
        float eps = max(pc.thickness, ray_depth * 2e-3);

        if (px_acc >= 1.5 && ray_depth > scene + eps &&
            prev_ray_depth <= prev_scene + eps) {
            vec2 uv_lo = uv_prev, uv_hi = uv;
            float iz_lo = invz_prev, iz_hi = invz;
            for (int k = 0; k < 6; ++k) {
                vec2 uv_m = (uv_lo + uv_hi) * 0.5;
                float iz_m = (iz_lo + iz_hi) * 0.5;
                if (1.0 / iz_m > scene_depth(uv_m) + eps) {
                    uv_hi = uv_m;
                    iz_hi = iz_m;
                } else {
                    uv_lo = uv_m;
                    iz_lo = iz_m;
                }
            }
            hit_uv = uv_hi;
            hit_dist = 1.0 / iz_hi;
            hit = true;
            break;
        }

        uv_prev = uv;
        invz_prev = invz;
        prev_ray_depth = ray_depth;
        prev_scene = scene;
    }

    if (!hit) {
        // ---- 未命中兜底：相机锚定反射探针（与 GL 版 ssr_trace.frag 一致）----
        // 未命中射线分两类，屏幕空间里都没有可用数据：折回相机（竖直镜面正对
        // 相机时反射方向指回相机背后，真实命中点在近平面之后甚至相机身后）与
        // 侧向/远向出屏。两类都按反射方向采样探针图集：探针捕获位置即相机
        // 位置，方向查找的视差误差在贴近反射物时最小，能取到相机周围（含相机
        // 背后、屏幕上完全看不见）的物体颜色。
        //
        // 折回射线此前走"取射线出屏点屏幕颜色"，只能拿到相机附近被 clamp 到
        // 屏幕边缘的地面色，相机背后的物体一律不在屏幕上，所以贴近竖直镜面时
        // 反射里看不到周围物体；该分支已由探针取代。
        FragColor = probe_fallback(R, end_sample.ray_depth);
        return;
    }

    vec3 color = texture(uColorTex, hit_uv).rgb;

    // 与 GL 版一致：命中点的边缘淡出已由出屏回退接管，这里只保留距离衰减，
    // 否则会在命中/回退交界处留下一条窄带。
    float dist_cov = 1.0 - smoothstep(pc.far_plane * 0.25, pc.far_plane * 0.75, hit_dist);
    FragColor = vec4(color, dist_cov);
}
