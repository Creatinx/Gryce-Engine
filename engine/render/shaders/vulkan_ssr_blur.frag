#version 450 core

// SSR 双边滤波：深度感知的边缘保持模糊，平滑步进产生的噪点。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

// 10 = uTexture (kSSRTexture), 11 = uDepthTexture (kPBRShadowDepth)
layout(binding = 10) uniform sampler2D uTexture;
layout(binding = 11) uniform sampler2D uDepthTexture;
// 12 = uNormalRoughTex (kPBRShadowDepth1)：A 通道是粗糙度，决定模糊程度
layout(binding = 12) uniform sampler2D uNormalRoughTex;

// 与 C++ VulkanShader::SSRPushData 严格对齐（std430，128 字节）；blur 用到 near/far/bilateral
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

float linearize_depth(float d) {
    return (pc.near_plane * pc.far_plane) /
           max(pc.far_plane - d * (pc.far_plane - pc.near_plane), 1e-6);
}

// 深度采样对齐到源纹素中心（见 GL 版说明）
vec2 snap_to_texel(vec2 uv, vec2 src_size) {
    return (floor(uv * src_size) + vec2(0.5)) / src_size;
}

void main() {
    vec4 center = texture(uTexture, vUV);
    // 未命中像素没有反射（与 GL 版一致）：直接输出 0，不让邻域命中像素把
    // 颜色/覆盖度填进来，避免反射区越过真实命中边界向外膨胀。
    if (center.a <= 0.0) {
        FragColor = vec4(0.0);
        return;
    }
    vec2 dsize = vec2(textureSize(uDepthTexture, 0));
    float center_lin = linearize_depth(texture(uDepthTexture, snap_to_texel(vUV, dsize)).r);
    // 粗糙度感知模糊（与 GL 版一致）：镜面不糊、越粗糙核越大
    float rough = texture(uNormalRoughTex, vUV).a;
    float blur_w = smoothstep(0.02, 0.30, rough);
    vec2 texel = (1.0 + 5.0 * clamp(rough, 0.0, 1.0)) /
                 max(vec2(textureSize(uTexture, 0)), vec2(1.0));
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 suv = vUV + vec2(float(x), float(y)) * texel;
            vec4 s4 = texture(uTexture, suv);
            float d = linearize_depth(texture(uDepthTexture, snap_to_texel(suv, dsize)).r);
            float depth_w = exp(-abs(d - center_lin) * max(pc.bilateral_filter, 0.01));
            float spatial_w = exp(-(float(x * x) + float(y * y)) / 2.0);
            // 颜色按命中覆盖率加权：未命中像素的 rgb 是 0，不乘 alpha 会把 0
            // 混进邻近命中像素，反射边缘出现一圈假暗边。
            float w = depth_w * spatial_w * s4.a;
            acc += s4.rgb * w;
            wsum += w;
        }
    }
    // 覆盖度用 5x5 单独平滑（见 GL 版说明）：命中边界原本是二值硬切，
    // 摊成几像素渐变后，屏幕空间反射与环境反射的交界不再像"贴上去的一块"。
    float acc_cov = 0.0;
    float cov_wsum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 suv = vUV + vec2(float(x), float(y)) * texel;
            float d = linearize_depth(texture(uDepthTexture, snap_to_texel(suv, dsize)).r);
            float depth_w = exp(-abs(d - center_lin) * max(pc.bilateral_filter, 0.01));
            float spatial_w = exp(-(float(x * x) + float(y * y)) / 8.0);
            float w = depth_w * spatial_w;
            acc_cov += texture(uTexture, suv).a * w;
            cov_wsum += w;
        }
    }
    vec3 blurred = wsum > 1e-4 ? acc / wsum : center.rgb;
    float cov_blur = acc_cov / max(cov_wsum, 1e-4);
    // 覆盖度以逐像素值为下界（与 GL 版一致）：min(., center.a) 会把稀疏命中区
    // 的覆盖度压到 1/25 附近，合成时反射只剩百分之几、其余仍是最初的 IBL 镜面，
    // 整片反射因此发白变淡。改成 max(., center.a) 保留满覆盖，只在邻域更密时
    // 抬高；未命中像素开头已 early-out，覆盖率不会外溢到命中区之外。
    // 孤点抑制：邻域几乎没有别的命中（cov_blur 低于 2/25）时，本像素大概率是
    // 步进在薄物体/掠射边缘的临界误命中。这类像素保留下来就是反射区里散落的
    // 1px 亮点（"满天星"），远看比漏掉这点反射更刺眼，所以只有在邻域给出
    // 支撑时才允许把覆盖度抬到满值。
    float cov = clamp(max(cov_blur, center.a * step(0.08, cov_blur)), 0.0, 1.0);
    FragColor = vec4(mix(center.rgb, blurred, blur_w), cov);
}
