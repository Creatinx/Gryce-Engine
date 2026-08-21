#version 330 core

// SSS 垂直方向模糊
// 对水平模糊结果进行深度加权的垂直高斯模糊

in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uSceneColor;
uniform sampler2D uDepthTex;
uniform vec2 uScreenSize;

uniform float uSSSStrength;
uniform float uSSSScale;

float linearize_depth(float depth, float near, float far) {
    float z = depth * 2.0 - 1.0;
    return (2.0 * near * far) / (far + near - z * (far - near));
}

void main() {
    vec2 texel = 1.0 / uScreenSize;

    vec4 center = texture(uSceneColor, vUV);
    float sss_weight = center.a;
    if (sss_weight < 0.01) {
        FragColor = center;
        return;
    }

    float center_depth = texture(uDepthTex, vUV).r;

    // 垂直高斯模糊（5-tap）
    vec4 result = center * 0.25;
    float total = 0.25;

    float depth_threshold = 0.01 * uSSSScale;

    for (int i = 1; i <= 4; ++i) {
        vec2 offset = vec2(0.0, texel.y * i);

        vec4 sample_t = texture(uSceneColor, vUV + offset);
        vec4 sample_b = texture(uSceneColor, vUV - offset);

        float depth_t = texture(uDepthTex, vUV + offset).r;
        float depth_b = texture(uDepthTex, vUV - offset).r;

        float weight_t = exp(-abs(depth_t - center_depth) / depth_threshold);
        float weight_b = exp(-abs(depth_b - center_depth) / depth_threshold);

        float gauss = exp(-float(i * i) / (2.0 * uSSSStrength * uSSSStrength));

        result += sample_t * weight_t * gauss;
        result += sample_b * weight_b * gauss;
        total += (weight_t + weight_b) * gauss;
    }

    if (total > 0.001) {
        result /= total;
    }

    result.a = sss_weight;

    FragColor = result;
}