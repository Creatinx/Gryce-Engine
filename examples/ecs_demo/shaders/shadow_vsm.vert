#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoord;

uniform mat4 uModel;
uniform mat4 uLightSpaceMatrix;
uniform float uNormalOffset;

out vec2 vTexCoord;

void main() {
    // Normal Offset Shadow Mapping：沿法线把几何推向光源
    vec3 world_pos = (uModel * vec4(aPos, 1.0)).xyz;
    // VSM 不支持 normal offset（会破坏深度矩），但保留接口兼容
    gl_Position = uLightSpaceMatrix * vec4(world_pos, 1.0);
    vTexCoord = aTexCoord;
}