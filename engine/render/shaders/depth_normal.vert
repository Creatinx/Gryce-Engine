#version 330 core

// [Shader 阶段] Vertex Shader
// [功能] Depth+Normal 预通道：为 SSR/SSIL 提供每像素世界空间法线与粗糙度。
//        前向渲染路径没有 G-buffer，这个通道补上 SSR 需要的那一半数据
//        （深度仍由主通道的 hdr_depth_ 提供）。

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 3) in vec2 aTexCoord;   // 顶点布局：0=pos 1=normal 2=tangent 3=uv 4=color

out vec3 vNormal;
out vec2 vTexCoord;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main() {
    vec4 world_pos = uModel * vec4(aPos, 1.0);
    // 与 pbr.vert 用同样的法线变换（mat3(uModel) 近似），保证预通道写入的法线
    // 与主通道着色时用的法线同源；否则 SSR 的反射方向会和实际着色不一致。
    vNormal = normalize(mat3(uModel) * aNormal);
    vTexCoord = aTexCoord;
    gl_Position = uProjection * uView * world_pos;
}
