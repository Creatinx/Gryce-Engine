#version 330 core

// [Shader 阶段] Vertex Shader
// [功能] SSIL（屏幕空间间接光）采样 pass 的全屏顶点着色器。

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
