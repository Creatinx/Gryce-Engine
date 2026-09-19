#version 330 core
// VSM Blur: 5x5 高斯模糊，对 VSM 的 depth/depth² 进行滤波
// 降低 light bleeding 需要先模糊再采样

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uVSMTexture;
uniform vec2 uBlurDirection; // (1,0) 水平 blur, (0,1) 垂直 blur

// 5x5 高斯权重
const float k_weights[5] = float[](
    0.06136, 0.24477, 0.38774, 0.24477, 0.06136
);

void main() {
    vec2 texel = 1.0 / vec2(textureSize(uVSMTexture, 0));
    vec4 result = vec4(0.0);
    float total_weight = 0.0;

    for (int i = -2; i <= 2; ++i) {
        vec2 offset = vec2(float(i)) * uBlurDirection * texel;
        float w = k_weights[i + 2];
        result += texture(uVSMTexture, vTexCoord + offset) * w;
        total_weight += w;
    }

    FragColor = result / max(total_weight, 1e-6);
}