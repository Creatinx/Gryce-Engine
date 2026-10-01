#version 450 core

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// GTAO（Ground-Truth Ambient Occlusion）：
// 使用表面法线将 diff 投影到切平面上计算 horizon，
// 避免斜面被误判为自我遮蔽。
// 采样半径以世界单位（米）定义，按当前像素深度换算屏幕 texel 步长，
// 保持 AO 的世界空间尺度远近一致。
// 与 GL 版 gtao.frag 算法一致，只有 uniform/绑定方式不同。

layout(binding = 0) uniform sampler2D uDepthTexture;
layout(binding = 1) uniform sampler2D uNormalRoughTex;

// 与 C++ VulkanShader::GTAOPushData 严格对齐（std430，96 字节）
// 半径 pc.radius 为世界单位（米）
layout(push_constant) uniform PushConstants {
    mat4 view;                // +0   world -> view
    float near_plane;         // +64
    float far_plane;          // +68
    float tan_half_fov;       // +72
    float aspect;             // +76
    float strength;           // +80
    float radius;             // +84  世界单位（米）
    float _pad[2];            // +88  对齐到 16
} pc;

float interleaved_gradient_noise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

float linearize_depth(float d) {
    return (pc.near_plane * pc.far_plane) /
           max(pc.far_plane - d * (pc.far_plane - pc.near_plane), 1e-6);
}

vec3 reconstruct_view_pos(vec2 uv, float lin) {
    // VK vUV 与 GL 上下相反（v=0 在顶端），y 翻转后再反投影
    vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return vec3(ndc.x * pc.tan_half_fov * pc.aspect * lin,
                ndc.y * pc.tan_half_fov * lin,
                -lin);
}

void main() {
    // ---- UV 对齐修正 ----
    // GTAO 在半分辨率 FBO 上运行，采样全分辨率 depth 纹理。
    // 半分辨率 fragment 的 UV 中心正好落在两个 depth 纹素的正中间
    // （完美 2x 下采样：(i+0.5)/(W/2) = (2i+1)/W 正好在 j=2i 和 j=2i+1 中间）
    // → Nearest filter 永远遇到 tie，不同驱动选不同 → 深度值不连续
    // → linearize_depth 跳变 → horizon 计算错误 → banding。
    // 加 0.25 texel 偏移让 UV 偏向 depth 纹素 2i，稳定避开 tie。
    vec2 depth_res = vec2(textureSize(uDepthTexture, 0));
    vec2 texel = 1.0 / depth_res;
    vec2 depth_uv_offset = 0.25 * texel;
    vec2 uv0 = vTexCoord + depth_uv_offset;

    float lin = linearize_depth(texture(uDepthTexture, uv0).r);
    if (lin < 0.01 || lin >= pc.far_plane * 0.99) {
        FragColor = vec4(1.0);
        return;
    }

    vec3 P = reconstruct_view_pos(vTexCoord, lin);

    // ---- 深度缩放：把世界半径换算为当前深度下的屏幕 texel 步长 ----
    // 单个纹素的世界尺寸 = 2*tan*lin/depth_res.y（见 GL 版 gtao.frag 说明）。
    // 除以 depth_res.x 会让 AO 半径被放大 aspect 倍，遮蔽范围外溢。
    float texel_world = (2.0 * pc.tan_half_fov * lin) / depth_res.y;
    float screen_radius_texels = pc.radius / max(texel_world, 1e-6);

    // 屏幕采样半径上限：与 GL 版 gtao.frag 一致。近距离时世界半径换算出的
    // 屏幕步长会失控（1m 视距下约 425 texel），越界采样点被 clamp 到屏幕边缘，
    // 形成"只有靠近才出现的奇怪阴影"。
    screen_radius_texels = min(screen_radius_texels, 0.1 * depth_res.y);

    // 采样跨度（世界单位）：屏幕半径被上限召回后 AO 的实际世界尺度随之缩小，
    // 后面的"采样距离拒绝"必须用这个实际跨度，而不是配置的 pc.radius。
    float sample_span_world = screen_radius_texels * texel_world;

    // ---- 法线：从 G-buffer / depth prepass 取世界法线，转到视图空间 ----
    vec3 N_view = vec3(0.0, 0.0, -1.0);
    ivec2 tex_size = textureSize(uNormalRoughTex, 0);
    if (tex_size.x > 0) {
        // 法线纹理（RGBA16F + Nearest）同样加偏移对齐：半分辨率 UV 落在两个纹素
        // 中点时 Nearest 的 tie-break 由驱动决定，加偏移让选中的纹素确定下来
        vec2 normal_res = vec2(tex_size);
        vec2 normal_uv_offset = 0.25 / normal_res;
        vec3 N_world = normalize(texture(uNormalRoughTex, vTexCoord + normal_uv_offset).rgb * 2.0 - 1.0);
        N_view = normalize((pc.view * vec4(N_world, 0.0)).xyz);
        if (length(N_view) < 1e-4) {
            N_view = vec3(0.0, 0.0, -1.0);
        }
    }

    float noise = interleaved_gradient_noise(gl_FragCoord.xy);
    float ao = 0.0;

    for (int d = 0; d < 6; ++d) {
        // noise 同时用于：方向旋转 + 步长 jitter（打散半分辨率 banding）
        float ang = noise * 6.2831853 + float(d) * 1.0471976; // PI/3
        vec2 dir = vec2(cos(ang), sin(ang));
        float max_h = -1e3;

        for (int s = 1; s <= 4; ++s) {
            // 用 noise 对每步 t 做 ±0.5 步的 jitter
            float t = (float(s) + noise - 0.5) / 4.0;
            // suv 从偏移后的 uv0 出发采样，避免每个采样点都落入 tie
            vec2 suv = uv0 + dir * screen_radius_texels * t * texel;
            suv = clamp(suv, 0.001, 0.999);

            float d2 = linearize_depth(texture(uDepthTexture, suv).r);
            if (d2 < 0.01 || d2 >= pc.far_plane * 0.99) continue;

            vec3 Q = reconstruct_view_pos(suv - depth_uv_offset, d2);
            vec3 diff = Q - P;

            // ---- 采样距离拒绝（跨几何）----
            // 与 GL 版 gtao.frag 一致：屏幕步长按"当前像素深度"换算，同一平面上的
            // 采样点其世界距离必然不超过 sample_span_world；落到别的几何（轮廓外的
            // 地面、背景墙）时深度差会把世界距离推远。竖立面的近轮廓像素就是这样
            // 被轮廓外的地面"遮蔽"到 AO=0，金属面 kD≈0 时环境光镜面项再乘这个
            // 因子就成了硬边黑带。超过 1.5 倍实际跨度判为跨几何，丢弃该采样点。
            if (length(diff) > sample_span_world * 1.5) continue;

            // 用法线投影 diff 到切平面，避免斜面误判为自我遮蔽
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

    // 强度只在 pbr 侧由 mix(1.0, ssao, uSSAOStrength) 应用一次；
    // 这里输出纯 AO。旧版两端各乘一次强度，等效 s²，s>1 时会把整面压成全黑。
    float result = 1.0 - ao / 6.0;
    FragColor = vec4(vec3(clamp(result, 0.0, 1.0)), 1.0);
}
