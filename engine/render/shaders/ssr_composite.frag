#version 330 core

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

// SSR 合成：把反射颜色叠加回 HDR 场景颜色。
// 输出到独立的 composite 目标（不是场景色附件），因此读取场景色是安全的；
// 结果再由调用方整体拷回 HDR 目标。这样 GL / Vulkan 行为完全一致：
// 不依赖 draw 前的混合状态（Vulkan 的混合在管线创建时定死），
// 也不依赖目标是否会被重新绑定清空（Vulkan 的 render pass 是 loadOp=CLEAR）。
uniform sampler2D uColorTex; // HDR 场景颜色（合成前）
uniform sampler2D uSSRTex;   // SSR 反射颜色（模糊后）

void main() {
    vec3 color = texture(uColorTex, vTexCoord).rgb;
    vec3 refl = texture(uSSRTex, vTexCoord).rgb;
    FragColor = vec4(color + refl, 1.0);
}
