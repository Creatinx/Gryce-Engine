#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

// 与 C++ VulkanShader::push_constants 的 ShadowPushData 对齐
layout(push_constant) uniform PushConstants {
    mat4 uLightSpaceMatrix;
    mat4 uModel;
    float uNormalOffset;
    vec3 _pad;
} pc;

void main() {
    // Normal Offset Shadow Mapping：沿法线把几何推向光源
    vec3 world_pos = (pc.uModel * vec4(aPos, 1.0)).xyz;
    vec3 N = normalize(mat3(pc.uModel) * aNormal);
    world_pos += N * pc.uNormalOffset;
    gl_Position = pc.uLightSpaceMatrix * vec4(world_pos, 1.0);
}
