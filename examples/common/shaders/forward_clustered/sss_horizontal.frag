#version 330 core

// SSS 水平方向模糊
// 对场景颜色进行深度加权的水平高斯模糊
// 仅在 SSS 强度 > 0 的区域生效

in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneColor;
uniform sampler2D uDepthTex;
uniform vec2 uScreenSize;

// SSS 参数（来自 PostProcessParams）
uniform float uSSSStrength;
uniform float uSSSScale;

// 从深度重建线性深度
float linearize_depth(float depth, float near, float far) {
    float z = depth * 2.0 - 1.0;
    return (2.0 * near * far) / (far + near - z * (far - near));
}

void main() {
    vec2 texel = 1.0 / uScreenSize;

    vec4 center = texture(uSceneColor, vUV);
    float sss_weight = center.a; // alpha 通道存储 SSS 强度
    if (sss_weight < 0.01) {
        FragColor = center;
        return;
    }

    float center_depth = texture(uDepthTex, vUV).r;

    // 水平高斯模糊（5-tap）
    vec4 result = center * 0.25;
    float total = 0.25;

    // 深度权重因子
    float depth_threshold = 0.01 * uSSSScale;

    for (int i = 1; i <= 4; ++i) {
        vec2 offset = vec2(texel.x * i, 0.0);

        vec4 sample_r = texture(uSceneColor, vUV + offset);
        vec4 sample_l = texture(uSceneColor, vUV - offset);

        float depth_r = texture(uDepthTex, vUV + offset).r;
        float depth_l = texture(uDepthTex, vUV - offset).r;

        float weight_r = exp(-abs(depth_r - center_depth) / depth_threshold);
        float weight_l = exp(-abs(depth_l - center_depth) / depth_threshold);

        float gauss = exp(-float(i * i) / (2.0 * uSSSStrength * uSSSStrength));

        result += sample_r * weight_r * gauss;
        result += sample_l * weight_l * gauss;
        total += (weight_r + weight_l) * gauss;
    }

    if (total > 0.001) {
        result /= total;
    }

    // 保留原始 SSS 强度
    result.a = sss_weight;

    FragColor = result;
}