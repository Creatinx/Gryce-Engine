#version 330 core

// [Shader 阶段] Vertex Shader
// [功能] 延迟渲染 GBuffer 填充 Pass：输出位置/法线/UV/颜色到片元阶段
// [输入] aPos/aNormal/aTexCoord/aColor（顶点属性）
// [输出] vFragPos/vTexCoord/vColor/vNormal（片元阶段的世界空间值）

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec4 aColor;

out vec3 vFragPos;
out vec2 vTexCoord;
out vec3 vColor;
out vec3 vNormal;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main() {
    vec4 world_pos = uModel * vec4(aPos, 1.0);
    vFragPos = world_pos.xyz;
    vTexCoord = aTexCoord;
    vColor = aColor.rgb;
    vNormal = normalize(mat3(uModel) * aNormal);
    gl_Position = uProjection * uView * world_pos;
}