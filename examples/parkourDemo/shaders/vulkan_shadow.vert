#version 450 core

layout(location = 0) in vec3 aPos;

layout(push_constant) uniform PushConstants {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpaceMatrix;
} pc;

void main() {
    gl_Position = pc.uLightSpaceMatrix * pc.uModel * vec4(aPos, 1.0);
}
