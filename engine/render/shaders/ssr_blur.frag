#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// SSR 双边滤波：深度感知的边缘保持模糊，平滑步进产生的噪点。
uniform sampler2D uTexture;      // SSR 反射颜色（RGBA）
uniform sampler2D uDepthTexture; // 深度（用于深度权重）
uniform sampler2D uNormalRoughTex; // RGB=法线, A=粗糙度（决定模糊程度）
uniform float uSSRNear;
uniform float uSSRFar;
uniform float uSSRBilateralFilter; // 深度权重系数（越大越保守，边缘保持越强）

float linearize_depth(float d) {
    // 深度纹理存的是 w = far*(z-near)/((far-near)*z)（见 ssr_trace.frag 的说明）
    return (uSSRNear * uSSRFar) / max(uSSRFar - d * (uSSRFar - uSSRNear), 1e-6);
}

// 深度采样对齐到源纹素中心：SSR 缩放渲染时，vUV ± 1 个 SSR 纹素
// 会落在全分辨率深度图的纹素边界上，NEAREST 的取整在两端可能不同。
vec2 snap_to_texel(vec2 uv, vec2 src_size) {
    return (floor(uv * src_size) + vec2(0.5)) / src_size;
}

void main() {
    vec4 center = texture(uTexture, vTexCoord);
    // 未命中像素（center.a == 0）没有反射可言：直接输出 0。命中判定是几何
    // 二值量，不允许邻域命中像素把颜色/覆盖度"填"进来 —— 那会让反射区越过
    // 真实命中边界向外膨胀，视觉上就是反射被映射到了物体轮廓之外。
    if (center.a <= 0.0) {
        FragColor = vec4(0.0);
        return;
    }
    vec2 dsize = vec2(textureSize(uDepthTexture, 0));
    float center_lin = linearize_depth(texture(uDepthTexture, snap_to_texel(vTexCoord, dsize)).r);
    // 粗糙度感知：镜面（粗糙度≈0）几乎不糊，保留逐像素镜面细节；越粗糙的
    // 表面用越大的核（同时靠双边深度权重保持边缘）。旧实现只有 3x3、且给所有
    // 粗糙度用同一个半径，粗糙面看起来仍然"锐"，与 IBL 预过滤的模糊不匹配。
    float rough = texture(uNormalRoughTex, vTexCoord).a;
    float blur_w = smoothstep(0.02, 0.30, rough);
    // 采样间距随粗糙度放大到 5x5 = 最多 ±3 像素（粗糙度 1 时等效半宽 ≈ 6 像素）
    vec2 texel = (1.0 + 5.0 * clamp(rough, 0.0, 1.0)) / vec2(textureSize(uTexture, 0));
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 suv = vTexCoord + vec2(float(x), float(y)) * texel;
            vec4 s4 = texture(uTexture, suv);
            float d = linearize_depth(texture(uDepthTexture, snap_to_texel(suv, dsize)).r);
            float depth_w = exp(-abs(d - center_lin) * max(uSSRBilateralFilter, 0.01));
            float spatial_w = exp(-(float(x * x) + float(y * y)) / 2.0);
            // 关键：颜色按"命中覆盖率"加权。未命中的像素 rgb 是 0，若不乘 alpha
            // 就会把 0 混进邻近的命中像素，反射边缘出现一圈发暗的描边。
            float w = depth_w * spatial_w * s4.a;
            acc += s4.rgb * w;
            wsum += w;
        }
    }
    // ---- 覆盖度单独用更大的核（5x5）平滑 ----
    // 命中/未命中在一条表面上本来是二值硬切（实测相邻像素 0 <-> 13~31 级亮度突变），
    // 所以屏幕空间反射与环境反射的交界看起来像"贴上去的一块"。
    // 颜色保持 3x3（保锐利），只把"命中置信度"摊开成几像素的渐变。
    float acc_cov = 0.0;
    float cov_wsum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 suv = vTexCoord + vec2(float(x), float(y)) * texel;
            float d = linearize_depth(texture(uDepthTexture, snap_to_texel(suv, dsize)).r);
            float depth_w = exp(-abs(d - center_lin) * max(uSSRBilateralFilter, 0.01));
            float spatial_w = exp(-(float(x * x) + float(y * y)) / 8.0);
            float w = depth_w * spatial_w;
            acc_cov += texture(uTexture, suv).a * w;
            cov_wsum += w;
        }
    }
    vec3 blurred = wsum > 1e-4 ? acc / wsum : center.rgb;
    float cov_blur = acc_cov / max(cov_wsum, 1e-4);
    // 覆盖度只允许向命中区"内侧"软化：min(., center.a) 把上界夹在逐像素值上，
    // 未命中像素在函数开头已经 early-out，因此 5x5 平滑绝不会把覆盖率摊到命中
    // 区之外。效果是命中区边缘的 cover 从 1 平滑降到 0，消除"贴上去一块"的
    // 硬切，而反射区边界严格落在真实命中边界之内。
    // 颜色与覆盖度同源（都只在命中像素非零），合成时不会出现"减了 IBL 镜面项
    // 却补不回反射"的黑边，也就不需要再把邻域 RGB 灌进未命中像素。
    float cov = min(cov_blur, center.a);
    FragColor = vec4(mix(center.rgb, blurred, blur_w), cov);
}
