#version 330 core

// [Shader 阶段] Vertex Shader
// [功能] VSM 阴影模糊的全屏 pass 顶点着色器（与 bloom_*.vert 同范式）。
// 说明：该文件此前缺失，导致 GL 后端加载 'vsm_blur' 程序失败、VSM 阴影被禁用；
// Vulkan 变体一直存在（vulkan_vsm_blur.vert），这里补齐 GL 侧。

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
