#version 450 core

layout(location = 0) in vec3 aPos;

// 颜色 pass：C++ 推送 { model, view, projection, lightspace } 4 个 mat4
layout(push_constant) uniform PushConstants {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpaceMatrix;
} pc;

layout(location = 0) out vec3 vWorldPos;

void main() {
    vec4 world_pos = pc.uModel * vec4(aPos, 1.0);
    vWorldPos = world_pos.xyz / world_pos.w;
    gl_Position = pc.uProjection * pc.uView * world_pos;
}