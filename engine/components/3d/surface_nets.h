#pragma once

#include <cstdint>

#include "assets/mesh_data.h"
#include "math/math.h"

namespace gryce_engine::components::surface_nets {

// ---------------------------------------------------------------------------
// Surface Nets 等值面提取
//
// 输入是一张规则标量场（角点采样，x 变化最快、z 最慢），输出三角网格：
//   * 每个跨越等值面的单元生成一个顶点，位置取该单元 12 条边交点的平均；
//   * 沿 +X/+Y/+Z 三个方向的网格边，把共享该边的 4 个单元顶点连成四边形
//     （每四边形拆两个三角形）。
//
// 相比 Marching Cubes，每个单元最多一个顶点，顶点数少、拓扑天然水密，代价是
// 缺少尖锐棱角——对流体表面（水、烟）正合适。
//
// 法线取场的负梯度（场在内部更大，所以梯度指向内部），在顶点的**真实位置**
// 做三线性采样，而不是用单元角点的梯度：后者在公共边处跳变，低粗糙度水面上
// 会看到网格状高光接缝。
//
// 顶点色固定写白：着色器里 albedo *= vColor，而 MeshVertex 的默认构造不初始化
// 该字段，留空会把整片表面乘成黑色。
// ---------------------------------------------------------------------------

// field 长度必须是 (nx+1)*(ny+1)*(nz+1)。nx/ny/nz 是**单元数**。
// origin 是场在原点的角点（对应角点下标 0,0,0），cell 是格距。
// iso 是等值面阈值（场值大于 iso 的一侧视为内部）。
// 输出顶点位置是本地空间坐标，直接可用于渲染。
void extract(const float* field, int nx, int ny, int nz, const math::Vector3f& origin, float cell,
             float iso, assets::MeshData& out);

// 三线性采样（输入是**单元坐标**，即 gx 在 [0, nx] 之间）。
float sample_trilinear(const float* field, int nx, int ny, int nz, float gx, float gy, float gz);

// 中心差分梯度（输入同上）。步长按半格，比整格更贴合顶点真实位置。
math::Vector3f gradient(const float* field, int nx, int ny, int nz, float gx, float gy, float gz);

} // namespace gryce_engine::components::surface_nets
