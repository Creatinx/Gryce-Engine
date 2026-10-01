#version 330 core

// Fog 合成 pass 全屏四边形顶点着色器。
// 顶点流只有 location 0 的 vec3 position（stride 12），没有独立 uv 属性，
// 因此这里与 vulkan 变体保持一致，由 position.xy 推导 uv。
layout(location = 0) in vec3 aPos;

out vec2 vUV;

void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    vUV = aPos.xy * 0.5 + 0.5;
}