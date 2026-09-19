#version 330 core

// [Shader 阶段] Vertex Shader
// [功能] 屏幕空间运动模糊的全屏 pass 顶点着色器。
// 输出名与 motion_blur.frag 的 `in vec2 vUV` 对齐（同 vulkan 变体）。

layout(location = 0) in vec2 aPos;

out vec2 vUV;

void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    vUV = aPos.xy * 0.5 + 0.5;
}
