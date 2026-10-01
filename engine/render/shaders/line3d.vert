#version 330 core

// 3D 线段（折线）顶点着色器 —— 每段一条实例
//   binding 0（每顶点）：aCorner，静态 6 顶点角标，只在创建时上传一次
//                        x: 0 = 起点，1 = 终点；y: -1 / +1 = 线段两侧
//   binding 1（每实例）：aStart/aEnd/aWidth/aColor，每段一份
// 线段面向相机的横向偏移在这里算，因此上传数据与相机无关。
layout(location = 0) in vec2 aCorner;
layout(location = 1) in vec3 aStart;
layout(location = 2) in vec3 aEnd;
layout(location = 3) in float aWidth;
layout(location = 4) in vec4 aColor;

uniform mat4 uView;
uniform mat4 uProjection;

out vec4 vColor;

void main() {
    vec3 dir = aEnd - aStart;
    float len = length(dir);
    // 首尾重合的退化段没有方向，给一个确定的轴向避免 NaN
    vec3 axis = len > 1e-6 ? dir / len : vec3(0.0, 1.0, 0.0);

    // view 矩阵旋转部分的第三行是世界空间的相机视线轴（列主序 mat4：uView[col][row]）
    vec3 view_axis = vec3(uView[0][2], uView[1][2], uView[2][2]);
    vec3 side = cross(axis, view_axis);
    float side_len = length(side);
    // 线段正好朝向相机（与视线共线）时叉乘退化，退回一个固定横向
    side = side_len > 1e-6 ? side / side_len : vec3(1.0, 0.0, 0.0);

    vec3 world = mix(aStart, aEnd, aCorner.x) + side * (aCorner.y * aWidth * 0.5);
    vColor = aColor;
    gl_Position = uProjection * uView * vec4(world, 1.0);
}