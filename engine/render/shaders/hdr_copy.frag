#version 330 core

// [Shader 阶段] Fragment Shader
// [功能] 单输入全屏拷贝：原样输出 uSource。
// SSR 合成结果（场景色 + 反射）用这个 pass 整体写回 HDR 目标，
// 避免"既当采样输入又当颜色附件"的自反馈，也不依赖后端的混合状态。

in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

uniform sampler2D uSource;

void main() {
    FragColor = texture(uSource, vTexCoord);
}
