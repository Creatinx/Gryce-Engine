#version 450 core

layout(location = 0) in vec4 vColor;

layout(location = 0) out vec4 FragColor;

void main() {
    // 全透明的段直接丢弃，避免写进深度缓冲（与粒子 pass 同样的阈值）
    if (vColor.a <= 0.002) discard;
    FragColor = vColor;
}