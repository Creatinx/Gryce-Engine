#version 330 core

// 屏幕空间运动模糊
// 沿运动向量方向采样 N 个像素并平均

in vec2 vUV;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uColorTex;
uniform sampler2D uMotionVecTex;
uniform vec2 uScreenSize = vec2(1280.0, 720.0);
uniform float uMotionBlurAmount = 0.5;

// 最大采样数
const int kMaxSamples = 16;

void main() {
    vec2 motion = texture(uMotionVecTex, vUV).rg;

    // 运动向量长度作为模糊强度缩放
    float motion_len = length(motion);
    if (motion_len < 0.001) {
        FragColor = texture(uColorTex, vUV);
        return;
    }

    // 根据运动强度决定采样数
    int samples = int(clamp(motion_len * 80.0 * uMotionBlurAmount, 3.0, float(kMaxSamples)));

    // 归一化运动方向
    vec2 dir = motion / motion_len;

    // 沿运动方向采样
    vec3 color = vec3(0.0);
    float total_weight = 0.0;

    // 在半长度范围内均匀采样（从 -0.5 到 +0.5，中心在当前位置）
    float half_len = motion_len * uMotionBlurAmount * 0.5;
    vec2 pixel_step = dir * half_len;

    for (int i = 0; i < samples; ++i) {
        float t = (float(i) / float(samples - 1)) - 0.5;
        vec2 sample_uv = vUV + pixel_step * t;
        // 边界钳制
        sample_uv = clamp(sample_uv, 0.0, 1.0);
        color += texture(uColorTex, sample_uv).rgb;
    }

    color /= float(samples);

    FragColor = vec4(color, 1.0);
}