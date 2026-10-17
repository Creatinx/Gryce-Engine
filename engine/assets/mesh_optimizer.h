// ===========================================================================
// mesh_optimizer.h — 导入期网格优化（顶点焊接 / 索引重建）
//
// 为什么需要这一步：
//   摄影测量、CAD 导出、部分 FBX/DAE 导出器会把网格**完全去索引化** —— 每个
//   三角形都有 3 个独立顶点，索引缓冲退化成恒等映射（0,1,2,3,...）。
//   4.4M 顶点的场景，顶点缓冲就有 233MB，每帧仅顶点取值就吃掉几十 GB/s 带宽，
//   几何阶段轻松占掉帧时间 1/3。
//
//   按"全部属性逐位相同"焊接之后，同一份资产通常能降到 1/4 左右（实测
//   4.37M → 1.08M），视觉完全不变 —— 因为被合并的顶点本来就是同一个顶点。
//
// 安全性：
//   - 比较用**位级相等**（memcmp），不做浮点容差合并，所以绝不可能把
//     位置/UV/法线/切线/顶点色有差异的顶点焊到一起（那会造成 UV 拉伸、
//     硬边变软）。哈希只用来分桶，命中后仍要逐字节确认。
//   - 顶点顺序保持不变（第一次出现的顺序），三角形顺序保持不变。
//   - 退化三角形（有两个及以上索引相同）顺手剔除，它们本来就光栅化不出像素。
// ===========================================================================
#pragma once

#include <cstddef>

namespace gryce_engine::assets {

struct MeshData;

struct WeldStats {
    std::size_t vertices_before = 0;
    std::size_t vertices_after = 0;
    std::size_t triangles_before = 0;
    std::size_t triangles_after = 0;
    std::size_t degenerate_removed = 0;
    // 导入时索引是否为恒等映射（0,1,2,...），即文件本身完全去索引化
    bool was_trivial_index = false;
    bool welded = false;   // 是否真的重建了索引（顶点数有减少）
};

// 按全部顶点属性逐位相等重建索引。返回优化前后统计。
// 顶点数没有减少时保持原索引不动（避免无意义的重新分配）。
WeldStats weld_mesh(MeshData& mesh);

// 判断 indices 是否为恒等映射（indices[i] == i）。空索引视为真（非索引网格）。
bool has_trivial_indices(const MeshData& mesh);

// 导入收尾统一入口：焊接 + 一条 INFO 日志。
// AssetManager 在拿到每种格式的导入结果后调用它，保证所有格式同样受益。
WeldStats optimize_imported_mesh(MeshData& mesh, const char* source_label);

}  // namespace gryce_engine::assets
