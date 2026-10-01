#version 450 core

// 体积光柱（VolumetricLight）顶点着色器（Vulkan）
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aParams;
layout(location = 2) in vec4 aColor;

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vAxis;
layout(location = 2) out vec3 vCam;
layout(location = 3) out vec4 vParams;
layout(location = 4) out vec4 vColor;

// 与 C++ VulkanShader 非后处理管线的 push constant（4 个 mat4）对齐
layout(push_constant) uniform PushConstants {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpaceMatrix;
} pc;

void main() {
    vec4 world = pc.uModel * vec4(aPosition, 1.0);
    vWorld = world.xyz;
    vAxis = normalize((pc.uModel * vec4(0.0, 0.0, -1.0, 0.0)).xyz);
    vCam = (inverse(pc.uView))[3].xyz;
    vParams = aParams;
    vColor = aColor;
    gl_Position = pc.uProjection * pc.uView * world;
}