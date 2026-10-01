#version 330 core

// 3D 粒子广告牌顶点着色器（实例化）
// 两条顶点流：
//   binding 0（每顶点）：aCorner，6 顶点的静态四边形，只在创建时上传一次
//   binding 1（每实例）：aCenter/aSize/aRotation/aColor，每颗粒子一份
// 属性 location 与旧的"每顶点重复 6 份"版本完全一致，仅数据来源变了。
layout(location = 0) in vec2 aCorner;    // -1..1 的四边形角
layout(location = 1) in vec3 aCenter;    // 粒子中心（世界空间）
layout(location = 2) in float aSize;     // 世界单位直径
layout(location = 3) in float aRotation; // 弧度
layout(location = 4) in vec4 aColor;

uniform mat4 uView;
uniform mat4 uProjection;

out vec2 vUV;
out vec4 vColor;

void main() {
    float s = sin(aRotation);
    float c = cos(aRotation);
    vec2 corner = vec2(aCorner.x * c - aCorner.y * s,
                       aCorner.x * s + aCorner.y * c) * aSize * 0.5;

    // view 矩阵旋转部分的行向量就是世界空间的相机基向量
    // （列主序 mat4：uView[col][row]，行 0 = right，行 1 = up）
    vec3 right = vec3(uView[0][0], uView[1][0], uView[2][0]);
    vec3 up    = vec3(uView[0][1], uView[1][1], uView[2][1]);

    vec3 world = aCenter + right * corner.x + up * corner.y;

    vUV = aCorner * 0.5 + 0.5;
    vColor = aColor;
    gl_Position = uProjection * uView * vec4(world, 1.0);
}