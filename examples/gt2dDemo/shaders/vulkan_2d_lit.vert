#version 450

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec2 aNormalCoord;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vTexCoord;
layout(location = 2) out vec2 vNormalCoord;
layout(location = 3) out vec2 vWorldPos;

layout(push_constant, std430) uniform PushConstants {
    mat4 uViewProj;
} pc;

void main() {
    gl_Position = pc.uViewProj * vec4(aPos, 0.0, 1.0);
    vColor = aColor;
    vTexCoord = aTexCoord;
    vNormalCoord = aNormalCoord;
    vWorldPos = aPos;
}
