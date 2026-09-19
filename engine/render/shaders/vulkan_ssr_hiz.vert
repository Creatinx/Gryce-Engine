#version 450 core

// SSR HiZ 构建 pass 全屏三角形顶点着色器。
// 顶点布局：stride 16 = vec2 position + vec2 uv（与 SSR_RD fullscreen_mesh_ 一致）。

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

layout(location = 0) out vec2 vUV;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}