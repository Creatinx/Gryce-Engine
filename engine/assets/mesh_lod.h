// ===========================================================================
// mesh_lod.h — 导入期自动 LOD 链（几何误差驱动的网格简化）
//
// 为什么放在导入期做：
//   LOD 的"档位"必须是**同一份几何的近似**，才能保证切换时轮廓不跳。运行时
//   按需简化会带来卡顿且无法预先知道误差，因此这里在模型导出的 CPU 网格上
//   一次生成整条链，之后只做 GPU 上传与选择。
//
// 算法（顶点聚类 / vertex clustering，误差有界）：
//   1. 用边长为 cell 的立方体栅格把顶点空间划分成簇，每个簇选一个"代表顶点"
//      —— 簇内离簇质心最近的那个**原始顶点**（不是质心！）。
//      保留原始顶点的属性（法线/UV/切线/顶点色）可以避免自造 UV 接缝：
//      一旦合成新顶点，采样坐标落在两个 texel 之间会出现纹理拉伸。
//   2. 三角形三个顶点映射到簇后若有重复则丢弃（退化成线/点，光栅化不出像素）。
//   3. cell 由目标三角数二分搜索得到；每轮同时记录"簇内顶点到代表顶点的最大
//      位移"，它就是这一级引入的几何误差上界（局部单位）。
//   4. 各级误差累加成 cumulative_error（三角不等式给出的上界，保守），供运行时
//      按屏幕空间误差反推切换距离。
//
// 为什么不用 QEM 边收缩：
//   摄影测量 / CAD 网格经常是非流形、非封闭、带大量孤立三角形的三角汤，边
//   收缩需要可靠的半边拓扑与合法性判定，且 1.4M 三角形的优先队列要几十秒；
//   聚类是 O(n) 两次扫描，实测 1.4M 三角形 3 级共 ~0.2s，且永远不会产生破面
//   （它只删三角形，不移动三角形的连通关系）。
// ===========================================================================
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "assets/mesh_data.h"
#include "math/math.h"

namespace gryce_engine::assets {

// 一条 LOD 链最多几级（含 level 0 原始网格）。GPU 侧句柄数组按此开。
constexpr int k_max_lod_levels = 6;

// ---------------------------------------------------------------------------
// LodLevel — 一级细节
// ---------------------------------------------------------------------------
struct LodLevel {
    std::shared_ptr<const MeshData> mesh;   // level 0 就是调用方传入的原始网格
    std::size_t triangles = 0;
    // 本级别相对上一级引入的最大顶点位移（局部单位）
    float error = 0.0f;
    // 相对 level 0 的累计误差上界（各级 error 之和，三角不等式保守估计）
    float cumulative_error = 0.0f;
    // 参考切换距离：screen_height=1080 / fov=60° / pixel_budget=2 像素时，
    // 该级别的屏幕空间误差才降到 2 像素以下。仅用于日志与默认值，
    // 运行时按真实 fov / 分辨率 / pixel_budget / lod_bias 重算。
    float reference_switch_distance = 0.0f;
};

// ---------------------------------------------------------------------------
// LodChain — 一个（模型, 子网格）的完整 LOD 链
// ---------------------------------------------------------------------------
struct LodChain {
    std::string source_path;
    int submesh_index = -1;
    std::vector<LodLevel> levels;   // levels[0] 恒为原始网格（误差 0）

    math::Vector3f local_min = math::Vector3f::zero();
    math::Vector3f local_max = math::Vector3f::zero();
    float local_radius = 0.0f;      // 局部包围球半径（供距离/屏幕覆盖率使用）

    int level_count() const { return static_cast<int>(levels.size()); }
    std::size_t base_triangles() const {
        return levels.empty() ? 0 : levels[0].triangles;
    }
};

// ---------------------------------------------------------------------------
// LodBuildSettings
// ---------------------------------------------------------------------------
struct LodBuildSettings {
    // 含 level 0 在内的总级数（>=2 才有意义；1 表示不生成）
    int max_levels = 4;
    // 每级三角数相对上一级的比例（0.5 = 每级减半）
    float ratio_per_level = 0.5f;
    // 相对原始三角数的下限：低于它不再继续降级（再减就只剩轮廓）
    float min_ratio = 0.05f;
    // 绝对三角数下限（太小的一级不如没有）
    std::size_t min_triangles = 512;
};

// 屏幕空间误差 → 切换距离（世界单位）。
//   error: 几何误差（与世界单位一致，调用方需把局部误差乘以缩放）
//   tan_half_fov: tan(垂直 fov / 2)
//   viewport_height: 渲染目标高度（像素）
//   pixel_budget: 允许的误差像素数（越大越早切低模、越省）
// 推导：误差在屏幕上占的像素 = error / (距离 * tan_half_fov) * (height/2)，
// 令其等于 pixel_budget 即得距离。
float lod_switch_distance(float error, float tan_half_fov, int viewport_height,
                          float pixel_budget);

// ---------------------------------------------------------------------------
// ChunkedLod — 空间分块 LOD（单个巨模型用）
//
// 为什么需要它：逐物体 LOD 的粒度是"整个物体"。一个占满屏幕的巨模型
// （城市街区 / 地形 / 大型建筑）在任何逐物体判据下都只能选 L0 —— 相机站在
// 它里面，最近点距离≈0，而"近处立面的细节"和"远处本来可以降级"是同时成立的，
// 一个级别满足不了。切成空间块之后，每块按自己的距离选级，才能真正吃到 LOD。
//
// 裂缝：三角形按质心整块归属，相邻块在网格平面附近共享顶点。如果这些共享
// 顶点在某一级被合并掉，两侧就接不上（T 形接缝）。所以构建时把"落在分块
// 网格平面附近的顶点"在所有级别里钉住（簇代表 = 自己），两侧保留同一批
// 顶点、同一批位置，接缝天然闭合。
// ---------------------------------------------------------------------------
struct LodChunk {
    math::Vector3f local_min = math::Vector3f::zero();
    math::Vector3f local_max = math::Vector3f::zero();
    float local_radius = 0.0f;
    // 栅格坐标（日志输出用）
    int grid[3] = {0, 0, 0};
    std::shared_ptr<const LodChain> chain;   // chain->levels[0] = 该块的原始几何
};

struct ChunkedLod {
    std::string source_path;
    int submesh_index = -1;
    std::vector<LodChunk> chunks;
    std::size_t base_triangles = 0;
    int max_levels = 0;   // 各块级数的最大值（运行时按块各自的级数夹取）
    int cells_per_axis = 1;

    bool valid() const { return chunks.size() > 1 && max_levels > 1; }
};

// 按空间分块生成 LOD：把 base 的 AABB 切成 cells_per_axis³ 的栅格，
// 三角形按质心归属，每块各自生成一条 LOD 链（边界顶点钉住防裂缝）。
// 块数太少（<=1）或模型太小返回 nullptr（退回逐物体 LOD）。
std::shared_ptr<const ChunkedLod> build_chunked_lod(const std::shared_ptr<const MeshData>& base,
                                                    const LodBuildSettings& settings,
                                                    int submesh_index,
                                                    int cells_per_axis);

// 该选多少块：让每块三角数落在 target_triangles 附近（cbrt 反推每轴格数），
// 夹在 [2, 6]。太小不值得分块，太多则绘制批次暴涨 —— 而且格数越多格线越密，
// 被"钉住"的接缝顶点越多、可简化量越少，实测 4³ 起反而回退（见 .cpp 里的数据）。
int choose_chunk_cells(std::size_t base_triangles, std::size_t target_triangles = 20000);

// 生成 LOD 链。base 为原始网格（level 0），settings 控制级数与比例。
// 失败（base 为空 / 级数不足）时返回 nullptr。
std::shared_ptr<const LodChain> build_lod_chain(const std::shared_ptr<const MeshData>& base,
                                                const LodBuildSettings& settings,
                                                int submesh_index = -1);

}  // namespace gryce_engine::assets
