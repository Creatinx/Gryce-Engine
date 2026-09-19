#version 330 core

// [Shader 阶段] Fragment Shader
// [功能] SSIL 结果的深度感知双边模糊：平滑半球采样的噪声，
//        同时用深度差做权重，避免跨物体边缘把间接光糊出去。

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uSSILTex;    // 上一步的 SSIL 结果（半分辨率）
uniform sampler2D uDepthTex;   // 原始深度
uniform float uSSILNear;
uniform float uSSILFar;

float linearize_depth(float d) {
    return (2.0 * uSSILNear * uSSILFar) /
           (uSSILFar + uSSILNear - d * (uSSILFar - uSSILNear));
}

void main() {
    float center_lin = linearize_depth(texture(uDepthTex, vTexCoord).r);
    vec2 texel = 1.0 / vec2(textureSize(uSSILTex, 0));

    vec3 acc = vec3(0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 uv = vTexCoord + vec2(float(x), float(y)) * texel;
            float d = linearize_depth(texture(uDepthTex, uv).r);
            // 深度差异越大权重越小（边缘保持）
            float depth_w = exp(-abs(d - center_lin) * 8.0);
            float spatial_w = exp(-(float(x * x) + float(y * y)) / 2.0);
            float w = depth_w * spatial_w;
            acc += texture(uSSILTex, uv).rgb * w;
            wsum += w;
        }
    }
    FragColor = vec4(acc / max(wsum, 1e-4), 1.0);
}
