#version 450 core

// 计算屏幕空间运动向量（RG16F）。
// 当前 C++ 侧未喂入投影/视图矩阵（旧 GL 路径同样缺失，退化为近 0 运动），
// 因此本 pass 输出近零运动向量，运动模糊因此退化为无操作（与 GL 行为一致）。

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec2 FragColor;

// 4 = uDepthTex (kTAAHistory)
layout(binding = 4) uniform sampler2D uDepthTex;

// 与 C++ VulkanShader::MotionPushData 对齐（std430，16 字节）
layout(push_constant) uniform PushConstants {
    vec2 screen_size; // +0
    float amount;     // +8
    float _pad;       // +12
} pc;

void main() {
    float depth = texture(uDepthTex, vUV).r;
    // 钳制到合理范围的运动向量（当前无矩阵，恒为 0）
    vec2 motion = clamp(vec2(0.0), -0.1, 0.1);
    FragColor = motion;
}