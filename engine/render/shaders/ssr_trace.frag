#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// SSR 主 pass：屏幕空间光线步进（OpenGL 版；vulkan_ssr_trace.frag 是完全相同的算法）。
//
// 深度约定（两个后端完全一致）：
//   投影矩阵的 z 行把视图深度映射到 [0,1]，因此深度纹理里存的是
//       w = far * (z - near) / ((far - near) * z)，  z 为视图深度（正）
//   正确的反线性化是
//       z = near * far / (far - w * (far - near))
//
// 步进：默认屏幕空间 DDA（1/z 按屏幕距离线性插值，掠射反射下误差可忽略）；
// 当射线深度跨度超过 3x（反射指向相机的极端场景）时切换到射线参数空间精确步进，
// 用投影公式从射线参数 t 直接算 uv 和 1/z，避免插值误差。

uniform sampler2D uColorTex;       // HDR 场景颜色
uniform sampler2D uDepthTex;       // 原始深度（[0,1]，见上）
uniform sampler2D uNormalRoughTex; // RGB = N*0.5+0.5, A = roughness
uniform mat4 uView;       // 世界 → 视图
uniform vec3 uCameraPos;  // 世界空间相机位置
uniform vec2 uScreenSize; // 视口尺寸（像素）

// 反射探针图集：6 个面按 3x2 排布（+X,-X,+Y,-Y,+Z,-Z），每面 1024x1024。
// 未绑定/未提供时采样返回 0（兜底分支此时输出黑色覆盖度，不影响命中区）。
uniform sampler2D uProbeAtlas;
// 1 = 探针图集可用且本帧已完成捕获（否则退化射线兜底不启用，保持旧的"黑色"行为）
uniform int uSSRProbeValid;
const float kProbeFaceSize = 1024.0;
const vec2  kProbeAtlasGrid = vec2(3.0, 2.0);

// 按方向从图集里取"反射探针"颜色。面的朝向与离线烘焙时用的
// look_at(eye, eye+forward, up) 一一对应（见 RenderPipeline::capture_reflection_probe），
// 因此这里的 (u,v) 组合与那 6 组 forward/up 必须保持一致。
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
    // 内缩半个纹素，避免跨面采到相邻格子
    const float inset = 0.5 / kProbeFaceSize;
    uv = clamp(uv, vec2(inset), vec2(1.0 - inset));
    return texture(uProbeAtlas, (cell + uv) / kProbeAtlasGrid).rgb;
}

// 由管线通过 set_post_process_params 同步
uniform float uSSRNear;
uniform float uSSRFar;
uniform float uSSRTanHalfFov;
uniform float uSSRAspect;
uniform float uSSRMaxRoughness;
uniform int   uSSRMaxSteps;
uniform float uSSRThickness;

// 退化/未命中射线的探针兜底：按反射方向取相机锚定探针的环境色。
// ray_depth 沿用命中区的距离衰减，保证与命中区覆盖度连续；探针不可用时返回
// 零覆盖（等价旧行为，交给 IBL），不会把场景压暗。
vec4 probe_fallback(vec3 R, float ray_depth) {
    if (uSSRProbeValid == 0) return vec4(0.0);
    vec3 c = probe_sample(R);
    float cov = 1.0 - smoothstep(uSSRFar * 0.25, uSSRFar * 0.75, max(ray_depth, 1e-4));
    return vec4(c, cov);
}

float linearize_depth(float w) {
    return (uSSRNear * uSSRFar) / max(uSSRFar - w * (uSSRFar - uSSRNear), 1e-6);
}

ivec2 self_texel(vec2 src_size) {
    vec2 img = floor(vec2(vTexCoord.x, 1.0 - vTexCoord.y) * src_size);
    return ivec2(int(img.x), int(src_size.y - 1.0 - img.y));
}

float scene_depth(vec2 uv) {
    vec2 src = vec2(textureSize(uDepthTex, 0));
    vec2 img = (floor(vec2(uv.x, 1.0 - uv.y) * src) + vec2(0.5)) / src;
    return linearize_depth(texture(uDepthTex, vec2(img.x, 1.0 - img.y)).r);
}

// 射线参数空间精确步进（用于深度跨度大的场景）
// 给定射线参数 t，精确计算：1/z 和屏幕 UV
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
    vec2 uv = clamp(ndc * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    return RaySample(invz_exact, uv, z_view);
}

void main() {
    vec2 nd = vec2(textureSize(uDepthTex, 0));
    ivec2 self = self_texel(nd);
    vec4 nr = texelFetch(uNormalRoughTex, self, 0);
    vec3 N = normalize(nr.rgb * 2.0 - 1.0);
    float roughness = nr.a;
    if (roughness >= uSSRMaxRoughness) {
        FragColor = vec4(0.0);
        return;
    }

    float lin = linearize_depth(texelFetch(uDepthTex, self, 0).r);
    if (lin <= uSSRNear || lin >= uSSRFar * 0.999) {
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

    vec3 V = normalize(uCameraPos - P);
    if (dot(N, V) <= 0.02) {
        FragColor = vec4(0.0);
        return;
    }
    vec3 R = reflect(-V, N);

    vec3 V0 = (uView * vec4(P, 1.0)).xyz;
    vec3 Dv = (uView * vec4(R, 0.0)).xyz;
    float invz0 = 1.0 / max(lin, 1e-4);

    vec2 uv0 = vTexCoord;
    float ta = uSSRTanHalfFov * uSSRAspect;
    float tb = uSSRTanHalfFov;

    // 端点参数 t_end
    float t_end = uSSRFar * 0.99;
    if (Dv.z < -1e-6) {
        t_end = min(t_end, (uSSRFar + V0.z) / -Dv.z);
    } else if (Dv.z > 1e-6) {
        t_end = min(t_end, (-V0.z - 1e-4) / Dv.z);
    }
    // plane_n 边界说明：
    //   ndc_y = +1（上边界）→ V.y + tb·V.z = 0 → 法向量 (0, 1, tb)
    //   ndc_y = -1（下边界）→ V.y - tb·V.z = 0 → 法向量 (0, 1, -tb)
    //   ndc_x = +1（右边界）→ V.x + ta·V.z = 0 → 法向量 (1, 0, ta)
    //   ndc_x = -1（左边界）→ V.x - ta·V.z = 0 → 法向量 (1, 0, -ta)
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
        // 射线在近平面处即刻终止（反射面几乎贴住相机）：屏幕空间没有任何可用
        // 数据，改走探针兜底，避免贴近物体时反射整片消失成黑色。
        FragColor = probe_fallback(R, lin);
        return;
    }

    // 端点（精确射线参数）
    RaySample end_sample = ray_sample(V0.z, Dv.z, V0.x, Dv.x, V0.y, Dv.y,
                                      ta, tb, t_end);
    float invz1 = end_sample.invz;

    vec2 uv0_exact = uv0; // 起点已经是精确的
    vec2 seg = end_sample.uv - uv0_exact;
    float px_total = length(seg * uScreenSize);
    if (px_total < 1.0) {
        // 反射方向几乎正对相机（贴近竖直镜面时反射折回相机）→ 屏幕投影位移不足
        // 1 像素，屏幕空间步进无意义。这类射线正是"贴近物体"的主场景，改走探针
        // 兜底，从而反射出相机周围（含相机背后、屏幕上完全看不见）的真实环境。
        FragColor = probe_fallback(R, end_sample.ray_depth);
        return;
    }

    // 判断：深度跨度超过 3x 时用射线参数空间精确步进
    // 否则用屏幕空间线性插值（掠射场景误差可忽略）
    float depth_ratio = end_sample.ray_depth / lin;
    bool use_t_space = (depth_ratio > 3.0) || (depth_ratio < 1.0/3.0);

    // 射线参数 t → 屏幕像素距离的换算系数（近似：取端点处的比率）
    float t_per_pixel = t_end / px_total; // 近似

    bool hit = false;
    vec2 hit_uv = vec2(0.0);
    float hit_dist = 0.0;

    vec2 uv_prev = uv0_exact;
    float invz_prev = invz0;
    float prev_ray_depth = lin;
    float prev_scene = scene_depth(uv0_exact);
    float px_acc = 0.0;
    float step_px = 1.0;

    for (int i = 0; i < uSSRMaxSteps; ++i) {
        if (px_acc >= px_total - 0.5) break;
        step_px = min(step_px * 1.35, 32.0);
        px_acc = min(px_acc + step_px, px_total);

        vec2 uv;
        float invz;
        float ray_depth;

        if (use_t_space) {
            // 射线参数空间精确步进
            float t = px_acc * t_per_pixel;
            RaySample smp = ray_sample(V0.z, Dv.z, V0.x, Dv.x, V0.y, Dv.y,
                                       ta, tb, t);
            uv = smp.uv;
            invz = smp.invz;
            ray_depth = smp.ray_depth;
        } else {
            // 屏幕空间线性插值（掠射场景误差可忽略）
            float s = px_acc / px_total;
            uv = mix(uv0_exact, end_sample.uv, s);
            invz = mix(invz0, invz1, s);
            ray_depth = 1.0 / invz;
        }

        float scene = scene_depth(uv);
        float eps = max(uSSRThickness, ray_depth * 2e-3);

        if (px_acc >= 1.5 && ray_depth > scene + eps &&
            prev_ray_depth <= prev_scene + eps) {
            // 二分细化（用同一空间：屏幕 s 或射线 t）
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
        // ---- 未命中兜底：相机锚定反射探针 ----
        // 未命中射线有两类，屏幕空间里都没有可用数据：
        //   1) 折回相机：竖直镜面正对相机时，反射方向指回相机背后，真实命中点
        //      落在近平面之后甚至相机身后；
        //   2) 侧向/远向出屏：反射方向落在视锥外。
        // 两类都按反射方向采样场景探针图集。探针捕获位置就是相机位置，方向
        // 查找的视差误差在"贴近反射物"时最小，因此能真正取到相机周围（包括
        // 相机背后、屏幕上完全看不见）的物体颜色；覆盖度沿用命中区的距离
        // 衰减，与命中区的覆盖度连续。
        //
        // 折回射线此前走的是"取射线出屏点的屏幕颜色"。那个取色只能拿到相机
        // 附近的地面色（出屏点被 clamp 到屏幕边缘），相机背后的物体一律不在
        // 屏幕上，所以贴近竖直镜面时反射里看不到周围物体——这正是本次要修的
        // 问题，故该分支已由探针取代。
        FragColor = probe_fallback(R, end_sample.ray_depth);
        return;
    }

    vec3 color = texture(uColorTex, hit_uv).rgb;

    // 命中点靠近屏幕边缘时原设计要淡出覆盖度（edge_cov），避免边界处硬切；
    // 但上方出屏回退已经接管了屏幕外侧的射线，淡出反而在命中/回退交界处留下
    // 一条 158 对 181 的窄带。这里只保留距离衰减。
    float dist_cov = 1.0 - smoothstep(uSSRFar * 0.25, uSSRFar * 0.75, hit_dist);
    float cov = dist_cov;

    FragColor = vec4(color, cov);
}
