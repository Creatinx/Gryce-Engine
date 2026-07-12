#version 450

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aTexCoord;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vTexCoord;

struct Light {
    vec2 pos;
    float radius;
    float intensity;
    vec3 color;
};

layout(push_constant, std430) uniform PushConstants {
    mat4 uOrtho;
    vec2 uScreenSize;
    vec3 uAmbient;
    int uLightCount;
    Light uLights[4];
} pc;

void main() {
    gl_Position = pc.uOrtho * vec4(aPos, 0.0, 1.0);
    vColor = aColor;
    vTexCoord = aTexCoord;
}
