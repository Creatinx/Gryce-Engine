#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// GTAO（Ground-Truth Ambient Occlusion）：
//   视图空间 horizon 搜索，使用表面法线将 diff 投影到切平面上，
//   避免斜面被误判为自我遮蔽（旧版只用屏幕 z 轴，45 度斜面会产生 22%+ 的假 AO）。
//
//   采样半径以世界单位（米）定义，按当前像素深度换算屏幕 texel 步长：
//       texel_world     = (2 * tan_fov * lin) / screen_width    每 texel 覆盖的世界宽度
//       screen_texels   = uSSAORadius / texel_world             该深度下世界半径对应的屏幕像素数
//   这样 AO 的世界空间尺度远近一致（近景小半径 / 远景自动缩小屏幕范围）。

uniform sampler2D uDepthTexture;
uniform sampler2D uNormalRoughTex;  // RGB = 法线（*0.5+0.5 编码），A = 粗糙度
uniform mat4 uView;                 // 世界 → 视图（用于法线变换）
uniform float uSSAONear;
uniform float uSSAOFar;
uniform float uSSAOTanHalfFov;
uniform float uSSAOAspect;
uniform float uSSAORadius;  // 世界单位（米），AO 的几何尺度
uniform int uSSAOEnabled;

float interleaved_gradient_noise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float linearize_depth(float d) {
    // 深度纹理存的是 w = far*(z-near)/((far-near)*z)，反线性化：
    // z = near*far/(far - w*(far-near))。
    return (uSSAONear * uSSAOFar) / max(uSSAOFar - d * (uSSAOFar - uSSAONear), 1e-6);
}

vec3 reconstruct_view_pos(vec2 uv, float lin) {
    vec2 ndc = uv * 2.0 - 1.0;
    return vec3(ndc.x * uSSAOTanHalfFov * uSSAOAspect * lin,
                ndc.y * uSSAOTanHalfFov * lin,
                -lin);
}

void main() {
    if (uSSAOEnabled == 0) {
        FragColor = vec4(1.0);
        return;
    }

    vec2 depth_res = vec2(textureSize(uDepthTexture, 0));
    vec2 texel = 1.0 / depth_res;

    // ---- UV 对齐修正 ----
    // GTAO 在半分辨率 FBO 上运行，采样全分辨率 depth 纹理。
    // 半分辨率 fragment 的 UV 中心 = (i+0.5)/(W/2) = (2i+1)/W，
    // 正好落在两个 depth 纹素（2i 和 2i+1）的正中间 → Nearest filter tie！
    // tie 时选择哪个纹素由驱动决定 → 相邻 fragment 选不同 depth → 深度不连续
    // → linearize_depth 得到的值跳变 → horizon 计算错误 → banding。
    // 加 0.25 texel 偏移让 UV 偏向 depth 纹素 2i，稳定避开 tie。
    vec2 depth_uv_offset = 0.25 * texel;
    vec2 uv0 = vTexCoord + depth_uv_offset;

    float lin = linearize_depth(texture(uDepthTexture, uv0).r);
    if (lin < 0.01 || lin >= uSSAOFar * 0.99) {
        FragColor = vec4(1.0);
        return;
    }

    vec3 P = reconstruct_view_pos(vTexCoord, lin);

    // ---- 深度缩放：把世界半径换算为当前深度下的屏幕 texel 步长 ----
    // 深度 lin 处可见高度 = 2*tan(half_fov)*lin，垂直覆盖 depth_res.y 个纹素，
    // 故单个纹素的世界尺寸（高/宽相同，正方形像素）= 2*tan*lin/depth_res.y。
    // 早前误除以 depth_res.x，使 texel_world 偏小 1/aspect，
    // screen_radius_texels 被放大 aspect 倍（1920x1080 下约 1.78 倍），
    // AO 实际世界半径远大于配置值（0.5m 变成约 0.89m），遮蔽范围整体外溢。
    float texel_world = (2.0 * uSSAOTanHalfFov * lin) / depth_res.y;
    float screen_radius_texels = uSSAORadius / max(texel_world, 1e-6);

    // 屏幕采样半径上限：世界半径换算出的屏幕步长必须限制在屏幕范围内。
    // 相机贴近物体时 lin 很小，半径会失控（0.5m 半径在 1m 视距下约 425 texel，
    // 已接近半屏），采样点越界后被下面 clamp 到屏幕边缘，边缘像素被反复采样，
    // 表现为"只有靠近时才出现的奇怪阴影"。按视口高度比例收敛后，近距离 AO
    // 退化为较小的世界尺度，但不会跨屏扩散。
    screen_radius_texels = min(screen_radius_texels, 0.1 * depth_res.y);

    // 采样跨度（世界单位）：屏幕半径被上限召回后 AO 的实际世界尺度随之缩小，
    // 后面的"采样距离拒绝"必须用这个实际跨度，而不是配置的 uSSAORadius。
    float sample_span_world = screen_radius_texels * texel_world;

    // ---- 法线：从 G-buffer / depth prepass 取世界法线，转到视图空间 ----
    vec3 N_view = vec3(0.0, 0.0, -1.0);
    if (textureSize(uNormalRoughTex, 0).x > 0) {
        // 法线纹理同样加偏移对齐：depth_normal_tex 是 full-res RGBA16F + Nearest，
        // 半分辨率 UV 正好落在两个纹素中点时 Nearest 的 tie-break 由驱动决定，
        // 加偏移可让选中的纹素确定下来。
        vec2 normal_res = vec2(textureSize(uNormalRoughTex, 0));
        vec2 normal_uv_offset = 0.25 / normal_res;
        vec3 N_world = normalize(texture(uNormalRoughTex, vTexCoord + normal_uv_offset).rgb * 2.0 - 1.0);
        N_view = normalize((uView * vec4(N_world, 0.0)).xyz);
        if (length(N_view) < 1e-4) {
            N_view = vec3(0.0, 0.0, -1.0);
        }
    }

    float noise = interleaved_gradient_noise(gl_FragCoord.xy);
    float ao = 0.0;

    for (int d = 0; d < 4; ++d) {
        float ang = noise * 6.2831853 + float(d) * 1.5707963;
        vec2 dir = vec2(cos(ang), sin(ang));
        float max_h = -1e3;

        for (int s = 1; s <= 4; ++s) {
            float t = float(s) / 4.0;
            // suv 从偏移后的 uv0 出发采样，避免每个采样点都落入 tie
            vec2 suv = uv0 + dir * screen_radius_texels * t * texel;
            suv = clamp(suv, 0.001, 0.999);

            float d2 = linearize_depth(texture(uDepthTexture, suv).r);
            if (d2 < 0.01 || d2 >= uSSAOFar * 0.99) continue;

            vec3 Q = reconstruct_view_pos(suv - depth_uv_offset, d2);
            vec3 diff = Q - P;

            // ---- 采样距离拒绝（跨几何）----
            // 屏幕步长是按"当前像素深度"换算的，因此落在同一平面上的采样点其
            // 世界距离必然不超过 sample_span_world；一旦采样点落到别的几何上
            //（轮廓外的地面、背景墙），深度差会把世界距离推远。竖立面的近轮廓
            // 像素正是这样被轮廓外的地面"遮蔽"：AO 直落到 0，而金属面 kD≈0，
            // 环境光的镜面项再乘这个因子就变成贴着轮廓的硬边黑带。
            // 超过 1.5 倍实际跨度即判为跨几何，丢弃该采样点。
            if (length(diff) > sample_span_world * 1.5) continue;

            vec3 diff_parallel = diff - dot(diff, N_view) * N_view;
            float tangent_len = length(diff_parallel);
            float z_parallel = dot(diff, N_view);

            float horizon;
            if (tangent_len < 1e-4) {
                horizon = -1e3;
            } else {
                horizon = z_parallel / tangent_len;
            }
            max_h = max(max_h, horizon);
        }

        if (max_h > -1e2) {
            float sin_h = max_h / sqrt(1.0 + max_h * max_h);
            ao += clamp(sin_h, 0.0, 1.0);
        }
    }

    // 强度只在 pbr.frag 由 mix(1.0, ssao, uSSAOStrength) 应用一次；
    // 这里输出纯 AO。旧版两端各乘一次强度，等效 s²，s>1 时会把整面压成全黑。
    float result = 1.0 - ao / 4.0;
    FragColor = vec4(vec3(clamp(result, 0.0, 1.0)), 1.0);
}
