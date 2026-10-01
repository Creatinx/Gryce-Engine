#version 450

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vTexCoord;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uFontAtlas;

void main() {
    // 覆盖率在字体图集的 **alpha** 通道（与 GL 版 renderer2d_impl.cpp 的
    // 2D 文本 shader 一致）。之前这里读 .r —— 图集的 R 恒为 1（白色字形），
    // 于是所有字形都渲染成不透明实心块（Vulkan 下 FPS 显示成白方块的原因）。
    float alpha = texture(uFontAtlas, vTexCoord).a;
    float a = smoothstep(0.45, 0.55, alpha);
    if (a < 0.01) discard;
    outColor = vec4(vColor.rgb, vColor.a * a);
}
