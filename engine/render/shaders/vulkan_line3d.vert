#version 450 core

// 3D 线段（折线）顶点着色器（Vulkan，实例化）
// 两条顶点流（与 vk_shader.cpp 的 line3d 分支一致）：
//   binding 0（每顶点，stride 8）：aCorner，静态角标
//   binding 1（每实例，stride 44）：aStart/aEnd/aWidth/aColor
layout(location = 0) in vec2 aCorner;
layout(location = 1) in vec3 aStart;
layout(location = 2) in vec3 aEnd;
layout(location = 3) in float aWidth;
layout(location = 4) in vec4 aColor;

layout(location = 0) out vec4 vColor;

// 与 C++ VulkanShader 非后处理管线的 push constant（4 个 mat4）对齐
layout(push_constant) uniform PushConstants {
    mat4 uModel;
    mat4 uView;
    mat4 uProjection;
    mat4 uLightSpaceMatrix;
} pc;

void main() {
    vec3 dir = aEnd - aStart;
    float len = length(dir);
    // 首尾重合的退化段没有方向，给一个确定的轴向避免 NaN
    vec3 axis = len > 1e-6 ? dir / len : vec3(0.0, 1.0, 0.0);

    // view 矩阵旋转部分的第三行是世界空间的相机视线轴（列主序 mat4：pc.uView[col][row]）
    vec3 view_axis = vec3(pc.uView[0][2], pc.uView[1][2], pc.uView[2][2]);
    vec3 side = cross(axis, view_axis);
    float side_len = length(side);
    // 线段正好朝向相机（与视线共线）时叉乘退化，退回一个固定横向
    side = side_len > 1e-6 ? side / side_len : vec3(1.0, 0.0, 0.0);

    vec3 world = mix(aStart, aEnd, aCorner.x) + side * (aCorner.y * aWidth * 0.5);
    vColor = aColor;
    gl_Position = pc.uProjection * pc.uView * vec4(world, 1.0);
}