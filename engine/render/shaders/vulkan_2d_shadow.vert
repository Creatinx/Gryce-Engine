#version 450

layout(location = 0) in vec2 aPos;

layout(push_constant, std430) uniform PushConstants {
    mat4 uLightSpaceMatrix;
} pc;

void main() {
    gl_Position = pc.uLightSpaceMatrix * vec4(aPos, 0.0, 1.0);
}
