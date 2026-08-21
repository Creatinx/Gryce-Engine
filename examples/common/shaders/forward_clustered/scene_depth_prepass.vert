#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aTangent;
layout(location = 3) in vec2 aTexCoord;

out vec3 vNormal;
out vec3 vTangent;
out vec2 vTexCoord;
out vec3 vFragPos;

uniform mat4 uModelMatrix;
uniform mat4 uNormalMatrix;
uniform mat4 uMVP;

void main() {
    vec4 world_pos = uModelMatrix * vec4(aPos, 1.0);
    vFragPos = world_pos.xyz;
    vNormal = normalize(mat3(uNormalMatrix) * aNormal);
    vTangent = normalize(mat3(uModelMatrix) * aTangent);
    vTexCoord = aTexCoord;
    gl_Position = uMVP * vec4(aPos, 1.0);
}