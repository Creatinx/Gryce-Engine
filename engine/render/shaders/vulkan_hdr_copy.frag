#version 450 core

// [Shader 阶段] Fragment Shader（Vulkan 变体）
// [功能] 单输入全屏拷贝：绑定 0 固定为后处理描述符集的"主输入"槽位。

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

layout(binding = 0) uniform sampler2D uSource;

void main() {
    FragColor = texture(uSource, vTexCoord);
}
