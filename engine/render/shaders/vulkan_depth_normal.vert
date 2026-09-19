#version 450 core

// [Shader 阶段] Vertex Shader（Vulkan）
// [功能] Depth+Normal 预通道：为 SSR/SSIL 提供每像素世界空间法线与粗糙度。

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 3) in vec2 aTexCoord;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vTexCoord;

// 与 C++ VulkanShader 非后处理管线的 push constant（4 个 mat4）对齐
layout(push_constant) uniform PushConstants {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpaceMatrix;
} pc;

void main() {
    vec4 world_pos = pc.uModel * vec4(aPos, 1.0);
    // 与 vulkan_pbr.vert 用同样的法线变换
    vNormal = normalize(mat3(pc.uModel) * aNormal);
    vTexCoord = aTexCoord;
    gl_Position = pc.uProjection * pc.uView * world_pos;
}
