#version 450 core
// VSM Blur: 单 pass 3x3 双线性合并高斯模糊（与 GL 版算法一致）。
// 原实现是 H/V 两个全屏 pass（各 5 tap）且读写同一张纹理（原地 blur，GL 下是
// 未定义行为）。现改为单 pass 3x3（双线性合并的 5-tap 高斯），并由调用方提供
// 独立的模糊输出目标，既少一个 pass 也消除了原地读写。

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uVSMTexture;

// 1D 5-tap 高斯 [0.06136, 0.24477, 0.38774, 0.24477, 0.06136] 的双线性合并结果：
// 偏移 ∓1.20043 权重 0.30613，中心 0.38774；权重和为 1，无需再归一化。
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