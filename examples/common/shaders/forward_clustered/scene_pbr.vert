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
out vec2 vScreenUV;

uniform mat4 uModelMatrix;
uniform mat4 uViewMatrix;
uniform mat4 uProjectionMatrix;
uniform mat4 uMVP;
uniform mat4 uNormalMatrix;

void main() {
    vec4 world_pos = uModelMatrix * vec4(aPos, 1.0);
    vFragPos = world_pos.xyz;
    vTexCoord = aTexCoord;
    vColor = aColor;

    // Normal/Tangent transform (mat3(uModelMatrix) handles non-uniform scale)
    vec3 N = normalize(mat3(uNormalMatrix) * aNormal);
    vec3 T = normalize(mat3(uModelMatrix) * aTangent);
    T = normalize(T - N * dot(T, N));
    vec3 B = cross(N, T);
    vTBN = mat3(T, B, N);

    gl_Position = uMVP * vec4(aPos, 1.0);
    vScreenUV = gl_Position.xy / gl_Position.w * 0.5 + 0.5;
}