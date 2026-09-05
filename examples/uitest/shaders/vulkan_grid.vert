#version 450 core

layout(location = 0) in vec3 aPos;

layout(location = 0) out vec3 vWorldPos;

// 与 C++ VulkanShader 非后处理管线的 push constant（4 个 mat4）对齐
layout(push_constant) uniform PushConstants {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpaceMatrix;
} pc;

void main() {
    vec4 world_pos = pc.uModel * vec4(aPos, 1.0);
    vWorldPos = world_pos.xyz;
    gl_Position = pc.uProjection * pc.uView * world_pos;
}
