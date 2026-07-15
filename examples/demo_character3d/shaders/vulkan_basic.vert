#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

layout(location = 0) out vec3 vColor;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 view;
    mat4 projection;
} pc;

void main() {
    gl_Position = pc.projection * pc.view * pc.model * vec4(aPos, 1.0);
    vColor = aNormal * 0.5 + 0.5;
}
