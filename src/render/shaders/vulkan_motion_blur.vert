#version 450 core
// Motion blur pass 全屏四边形顶点着色器（stride 12 = vec3 position，无 uv）
layout(location = 0) in vec3 aPos;
layout(location = 0) out vec2 vUV;
void main() {
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
    vUV = aPos.xy * 0.5 + 0.5;
}