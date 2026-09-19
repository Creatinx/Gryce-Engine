#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// SSR 双边滤波：深度感知的边缘保持模糊，平滑步进产生的噪点。
uniform sampler2D uTexture;      // SSR 反射颜色（RGBA）
uniform sampler2D uDepthTexture; // 深度（用于深度权重）
uniform float uSSRNear;
uniform float uSSRFar;
uniform float uSSRBilateralFilter; // 深度权重系数（越大越保守，边缘保持越强）

float linearize_depth(float d) {
    return (2.0 * uSSRNear * uSSRFar) /
           (uSSRFar + uSSRNear - d * (uSSRFar - uSSRNear));
}

void main() {
    vec4 center = texture(uTexture, vTexCoord);
    float center_lin = linearize_depth(texture(uDepthTexture, vTexCoord).r);
    vec2 texel = 1.0 / vec2(textureSize(uTexture, 0));
    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 suv = vTexCoord + vec2(float(x), float(y)) * texel;
            vec3 s = texture(uTexture, suv).rgb;
            float d = linearize_depth(texture(uDepthTexture, suv).r);
            float depth_w = exp(-abs(d - center_lin) * max(uSSRBilateralFilter, 0.01));
            float spatial_w = exp(-(float(x * x) + float(y * y)) / 2.0);
            float w = depth_w * spatial_w;
            acc += s * w;
            wsum += w;
        }
    }
    FragColor = vec4(acc / max(wsum, 1e-4), center.a);
}
