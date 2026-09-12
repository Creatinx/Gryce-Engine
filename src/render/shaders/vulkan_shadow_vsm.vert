#version 450 core

layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aTexCoord;

// 与 C++ VulkanShader::push_constants 的 ShadowPushData 对齐（!color_output 分支）
layout(push_constant) uniform PushConstants {
    mat4 uLightSpaceMatrix;
    mat4 uModel;
    float uNormalOffset;
    vec3 _pad;
} pc;

layout(location = 0) out vec2 vTexCoord;

void main() {
    vec3 world_pos = (pc.uModel * vec4(aPos, 1.0)).xyz;
    // VSM 不支持 normal offset（会破坏深度矩），但保留接口兼容
    gl_Position = pc.uLightSpaceMatrix * vec4(world_pos, 1.0);
    vTexCoord = aTexCoord;
}