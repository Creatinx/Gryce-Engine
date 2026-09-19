#version 450 core

// [Shader 阶段] Vertex Shader（Vulkan 变体）
// [功能] 全屏 pass：把一张纹理整体拷贝到目标（SSR 合成结果落回 HDR 目标）。

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
