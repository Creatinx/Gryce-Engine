#version 330 core

// [Shader 阶段] Vertex Shader
// [功能] 全屏三角形（覆盖整个 NDC），用于延迟光照 Pass
// [输入] 无（硬编码三个顶点覆盖 NDC）

out vec2 vUV;

void main() {
    // 全屏三角形：覆盖 [-1,1] 范围，无需顶点缓冲
    float x = float((gl_VertexID & 1) == 0 ? -1 : 1);
    float y = float((gl_VertexID & 2) == 0 ? -1 : 1);
    vUV = vec2(x * 0.5 + 0.5, y * 0.5 + 0.5);
    gl_Position = vec4(x, y, 0.0, 1.0);
}