#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aTangent;
layout(location = 3) in vec2 aTexCoord;
layout(location = 4) in vec3 aColor;

layout(push_constant) uniform PushConstants {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpaceMatrix;
} pc;

layout(location = 0) out vec3 vFragPos;
layout(location = 1) out vec2 vTexCoord;
layout(location = 2) out vec3 vColor;
layout(location = 3) out mat3 vTBN;
layout(location = 6) out vec4 vLightSpacePos;

void main() {
    vec4 world_pos = pc.uModel * vec4(aPos, 1.0);
    vFragPos = world_pos.xyz;
    vTexCoord = aTexCoord;
    vColor = aColor;

    vec3 N = normalize(mat3(pc.uModel) * aNormal);
    vec3 T = normalize(mat3(pc.uModel) * aTangent);
    T = normalize(T - N * dot(T, N));
    vec3 B = cross(N, T);
    vTBN = mat3(T, B, N);

    vLightSpacePos = pc.uLightSpaceMatrix * world_pos;
    gl_Position = pc.uProjection * pc.uView * world_pos;
}
