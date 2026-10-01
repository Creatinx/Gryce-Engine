#version 450 core

// 3D 粒子广告牌顶点着色器（Vulkan，深度补写 pass，实例化）
// 与 vulkan_particle.vert 完全一致：几何必须和颜色 pass 重合。
layout(location = 0) in vec2 aCorner;
layout(location = 1) in vec3 aCenter;
layout(location = 2) in float aSize;
layout(location = 3) in float aRotation;
layout(location = 4) in vec4 aColor;

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
    float s = sin(aRotation);
    float c = cos(aRotation);
    vec2 corner = vec2(aCorner.x * c - aCorner.y * s,
                       aCorner.x * s + aCorner.y * c) * aSize * 0.5;

    vec3 right = vec3(pc.uView[0][0], pc.uView[1][0], pc.uView[2][0]);
    vec3 up    = vec3(pc.uView[0][1], pc.uView[1][1], pc.uView[2][1]);

    vec3 world = aCenter + right * corner.x + up * corner.y;

    vUV = aCorner * 0.5 + 0.5;
    vColor = aColor;
    gl_Position = pc.uProjection * pc.uView * vec4(world, 1.0);
}