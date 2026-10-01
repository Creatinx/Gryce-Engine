#version 330 core
// VSM Blur: 单 pass 3x3 双线性合并高斯模糊，对 VSM 的 depth/depth² 进行滤波。
// 降低 light bleeding 需要先模糊再采样。
//
// 原实现是 H/V 两个全屏 pass（各 5 tap）且**读写同一张纹理**（原地 blur）——
// 读写同一附件在 GL 中是未定义行为。这里改为：
//   1) 用双线性权重把 1D 的 5 个 tap 合并成 3 个（偏移 ±1.20043 与 0），
//      二维卷积即 3x3 = 9 tap，单 pass 完成，比"2 pass × 5 tap"更省；
//   2) 由调用方提供独立的模糊输出目标，消除原地读写。

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uVSMTexture;

// 1D 5-tap 高斯 [0.06136, 0.24477, 0.38774, 0.24477, 0.06136] 的双线性合并结果：
//   ±(w[1]+w[2]) 合并到偏移 ∓1.20043，权重 0.30613；中心权重 0.38774。
// 权重和恰为 1，卷积后无需再归一化。
const vec3 k_offset = vec3(-1.20043, 0.0, 1.20043);
const vec3 k_weight = vec3(0.30613, 0.38774, 0.30613);

void main() {
    vec2 texel = 1.0 / vec2(textureSize(uVSMTexture, 0));
    vec4 result = vec4(0.0);

    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            vec2 offset = vec2(k_offset[i], k_offset[j]) * texel;
            result += texture(uVSMTexture, vTexCoord + offset) * (k_weight[i] * k_weight[j]);
        }
    }

    FragColor = result;
}