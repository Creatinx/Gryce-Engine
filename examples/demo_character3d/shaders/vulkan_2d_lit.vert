#version 450

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aTexCoord;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vTexCoord;

layout(push_constant, std430) uniform PushConstants {
    mat4 uOrtho;
    vec2 uScreenSize;
    float uAmbient[3];
    float uPad0;
    vec2 uLightPos;
    float uLightRadius;
    float uLightIntensity;
    float uLightColor[3];
    float uPad1;
    float uPad2[2];
} pc;

void main() {
    gl_Position = pc.uOrtho * vec4(aPos, 0.0, 1.0);
    vColor = aColor;
    vTexCoord = aTexCoord;
}
