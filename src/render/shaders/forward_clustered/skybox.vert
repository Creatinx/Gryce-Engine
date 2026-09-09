#version 330 core

// 天空盒：渲染全屏四边形，用逆 VP 矩阵重建世界方向
// 顶点位置在 NDC（[-1, 1] 的四边形）
layout(location = 0) in vec3 aPos;

out vec3 vWorldDir;

uniform mat4 uInvViewProj;
uniform mat4 uViewMatrix; // 仅用于去除平移分量

void main() {
    vec4 pos = vec4(aPos, 1.0);

    // 重建世界方向（从 NDC 到世界空间，去除相机平移）
    vec4 world_pos = uInvViewProj * pos;
    world_pos.xyz /= world_pos.w;
    // 移除平移分量，仅保留旋转
    vec3 dir = (uViewMatrix * vec4(world_pos.xyz, 0.0)).xyz;
    // 转回世界空间方向
    vWorldDir = (inverse(uViewMatrix) * vec4(dir, 0.0)).xyz;

    gl_Position = pos;
}