#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uTextureA; // 更小的上一级（上采样模糊）
uniform sampler2D uTextureB; // 当前级（保留自身贡献）

// 彩色辉光（色散）：R/B 沿径向做极细微的 UV 反向偏移，G 保持不变，于是高亮
// 光源的辉光边缘会出现轻微彩色镶边。用常量而不是 uniform 是为了让 GL / Vulkan /
// D3D12 三份实现完全一致（Vulkan 变体没有裸 uniform 通道，走 push constant 要
// 复制整块后处理参数，代价远大于收益）。
const float kChromaticDispersion = 0.004;

void main() {
    vec2 dir = vTexCoord - 0.5;
    vec2 disp = dir * kChromaticDispersion;

    vec3 c;
    c.r = texture(uTextureA, vTexCoord + disp).r;
    c.g = texture(uTextureA, vTexCoord).g;
    c.b = texture(uTextureA, vTexCoord - disp).b;
    c += texture(uTextureB, vTexCoord).rgb;
    FragColor = vec4(c, 1.0);
}
