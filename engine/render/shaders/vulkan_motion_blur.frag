#version 450 core

// 屏幕空间运动模糊：沿运动向量方向采样 N 个像素并平均。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

// 0 = uColorTex (kTonemapHDR), 13 = uMotionVecTex (kMotionVectors)
layout(binding = 0)  uniform sampler2D uColorTex;
layout(binding = 13) uniform sampler2D uMotionVecTex;

// 与 C++ VulkanShader::MotionPushData 对齐（std430，16 字节）
layout(push_constant) uniform PushConstants {
    vec2 screen_size; // +0
    float amount;     // +8
    float _pad;       // +12
} pc;

const int kMaxSamples = 16;

void main() {
    vec2 motion = texture(uMotionVecTex, vUV).rg;

    float motion_len = length(motion);
    if (motion_len < 0.001) {
        FragColor = texture(uColorTex, vUV);
        return;
    }

    int samples = int(clamp(motion_len * 80.0 * pc.amount, 3.0, float(kMaxSamples)));

    vec2 dir = motion / motion_len;

    vec3 color = vec3(0.0);
    float total_weight = 0.0;

    float half_len = motion_len * pc.amount * 0.5;
    vec2 pixel_step = dir * half_len;

    for (int i = 0; i < samples; ++i) {
        float t = (float(i) / float(samples - 1)) - 0.5;
        vec2 sample_uv = vUV + pixel_step * t;
        sample_uv = clamp(sample_uv, 0.0, 1.0);
        color += texture(uColorTex, sample_uv).rgb;
    }

    color /= float(samples);
    FragColor = vec4(color, 1.0);
}