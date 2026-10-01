#version 450 core

// 3D 粒子广告牌顶点着色器（Vulkan，实例化）
// 两条顶点流（与 vk_shader.cpp 的 particle_ 分支一致）：
//   binding 0（每顶点，stride 8）：aCorner，静态四边形
//   binding 1（每实例，stride 36）：aCenter/aSize/aRotation/aColor
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

    // view 矩阵旋转部分的行向量即世界空间相机基向量
    // （列主序 mat4：pc.uView[col][row]，行 0 = right，行 1 = up）
    vec3 right = vec3(pc.uView[0][0], pc.uView[1][0], pc.uView[2][0]);
    vec3 up    = vec3(pc.uView[0][1], pc.uView[1][1], pc.uView[2][1]);

    vec3 world = aCenter + right * corner.x + up * corner.y;

    vUV = aCorner * 0.5 + 0.5;
    vColor = aColor;
    gl_Position = pc.uProjection * pc.uView * vec4(world, 1.0);
}