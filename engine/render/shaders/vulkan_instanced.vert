#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aTangent;
layout(location = 3) in vec2 aTexCoord;
layout(location = 4) in vec3 aColor;

// 每实例的 model 矩阵（binding 1，VK_VERTEX_INPUT_RATE_INSTANCE）。
layout(location = 5) in vec4 aInstanceM0;
layout(location = 6) in vec4 aInstanceM1;
layout(location = 7) in vec4 aInstanceM2;
layout(location = 8) in vec4 aInstanceM3;

// push constant 布局与 vulkan_pbr 保持一致（uModel 未使用，保留以共享同一
// push range 与 set_uniform_* 路由）。
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
layout(location = 7) out vec2 vScreenUV;

void main() {
    mat4 model = mat4(aInstanceM0, aInstanceM1, aInstanceM2, aInstanceM3);

    vec4 world_pos = model * vec4(aPos, 1.0);
    vFragPos = world_pos.xyz;
    vTexCoord = aTexCoord;
    vColor = aColor;

    vec3 N = normalize(mat3(model) * aNormal);
    vec3 T = normalize(mat3(model) * aTangent);
    T = normalize(T - N * dot(T, N));
    vec3 B = cross(N, T);
    vTBN = mat3(T, B, N);

    vLightSpacePos = pc.uLightSpaceMatrix * world_pos;
    gl_Position = pc.uProjection * pc.uView * world_pos;
    vScreenUV = (gl_Position.xy / gl_Position.w) * 0.5 + 0.5;
    vScreenUV.y = 1.0 - vScreenUV.y;
}