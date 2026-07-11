#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aTangent;
layout(location = 3) in vec2 aTexCoord;
layout(location = 4) in vec3 aColor;

out vec3 vFragPos;
out vec2 vTexCoord;
out vec3 vColor;
out mat3 vTBN;
out vec4 vLightSpacePos;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
uniform mat4 uLightSpaceMatrix;

void main() {
    vec4 world_pos = uModel * vec4(aPos, 1.0);
    vFragPos = world_pos.xyz;
    vTexCoord = aTexCoord;
    vColor = aColor;

    vec3 N = normalize(mat3(uModel) * aNormal);
    vec3 T = normalize(mat3(uModel) * aTangent);
    // Gram-Schmidt 正交化
    T = normalize(T - N * dot(T, N));
    vec3 B = cross(N, T);
    vTBN = mat3(T, B, N);

    vLightSpacePos = uLightSpaceMatrix * world_pos;
    gl_Position = uProjection * uView * world_pos;
}
