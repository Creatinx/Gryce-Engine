#version 330 core

// [Shader 阶段] Vertex Shader
// [功能] 全屏 pass 顶点着色器：把一张纹理整体拷贝到目标（SSR 合成结果落回 HDR 目标）。
// 注意：这里只给顶点属性写 location（GLSL 330 允许），输出 varying 不能写
// location —— 330 需要 GL_ARB_separate_shader_objects/#version 410 才支持，
// Vulkan 侧由 vulkan_hdr_copy.vert 提供带 location 的版本。

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
