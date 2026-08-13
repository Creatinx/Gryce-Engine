#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uLightSpaceMatrix;
uniform mat4 uModel;
uniform float uNormalOffset;

void main() {
    // Normal Offset Shadow Mapping：沿法线把几何推向光源（替代纯深度 bias，减少悬浮）
    vec3 world_pos = (uModel * vec4(aPos, 1.0)).xyz;
    vec3 N = normalize(mat3(uModel) * aNormal);
    world_pos += N * uNormalOffset;
    gl_Position = uLightSpaceMatrix * vec4(world_pos, 1.0);
}
