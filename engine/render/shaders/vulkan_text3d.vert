#version 450 core

// 3D 文本（TextMesh3D）顶点着色器（Vulkan）
// 顶点已经是组件按字形图集展开的局部空间字形四边形，单个顶点流
// （stride 36：position(3)+uv(2)+color(4)，与 vk_shader.cpp 的 text3d_ 分支一致）。
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

// 与 C++ VulkanShader 非后处理管线的 push constant（4 个 mat4）对齐
layout(push_constant) uniform PushConstants {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpaceMatrix;
} pc;

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = pc.uProjection * pc.uView * pc.uModel * vec4(aPosition, 1.0);
}