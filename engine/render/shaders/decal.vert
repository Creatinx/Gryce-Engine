#version 330 core

// [Shader 阶段] Vertex Shader
// [功能] 贴花包围盒顶点着色器：将 unit box 顶点变换到 clip space
// [输入] aPos (本地坐标), uModel/uView/uProjection

layout(location = 0) in vec3 aPos;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vWorldPos;

void main() {
    vec4 world_pos = uModel * vec4(aPos, 1.0);
    vWorldPos = world_pos.xyz / world_pos.w;
    gl_Position = uProjection * uView * world_pos;
}