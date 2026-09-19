#version 330 core

// [Shader 阶段] Vertex Shader
// [功能] 运动向量生成的全屏 pass 顶点着色器。
// 输出名与 motion_vectors.frag 的 `in vec2 vUV` 对齐（同 vulkan 变体）。
// 说明：该文件此前缺失，使 GL 后端 motion_vectors / motion_blur 程序加载失败、
// 运动模糊被禁用；Vulkan 变体一直存在，这里补齐 GL 侧。

layout(location = 0) in vec2 aPos;

out vec2 vUV;

void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    vUV = aPos.xy * 0.5 + 0.5;
}
