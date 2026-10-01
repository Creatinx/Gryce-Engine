#version 330 core

// 体积光柱（VolumetricLight）顶点着色器
// 单条顶点流（与 vk_shader.cpp 的 voluelight_ 分支一致）：
//   location 0：aPosition，局部空间锥体位置（沿 -Z 展开）
//   location 1：aParams = (t, cos_inner, cos_outer, range)
//   location 2：aColor  = (r, g, b, a)，rgb 已乘 intensity
// 几何完全在局部空间生成，前向轴由 uModel 旋转到世界空间。
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aParams;
layout(location = 2) in vec4 aColor;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vWorld;
out vec3 vAxis;
out vec3 vCam;
out vec4 vParams;
out vec4 vColor;

void main() {
    vec4 world = uModel * vec4(aPosition, 1.0);
    vWorld = world.xyz;
    // 本地前向轴为 -Z（Transform::forward()），只取旋转部分变换到世界空间
    vAxis = normalize((uModel * vec4(0.0, 0.0, -1.0, 0.0)).xyz);
    // 相机世界位置：uView 逆矩阵的平移列；在顶点阶段算一次即可
    vCam = (inverse(uView))[3].xyz;
    vParams = aParams;
    vColor = aColor;
    gl_Position = uProjection * uView * world;
}