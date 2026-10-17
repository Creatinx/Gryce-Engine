#include "assets/mesh_lod.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::assets {
namespace {

constexpr uint32_t k_invalid = 0xFFFFFFFFu;

// 格子坐标量化：每轴 21 位（最多 2M 格）。任何真实网格都用不到这么细，
// 但保留成 64 位键可以一次性打包三个轴，哈希表里不做结构体比较。
constexpr int k_axis_bits = 21;
constexpr uint32_t k_axis_limit = (1u << k_axis_bits) - 1u;

inline uint64_t pack_cell(int x, int y, int z) {
    const uint64_t ux = static_cast<uint32_t>(x) & k_axis_limit;
    const uint64_t uy = static_cast<uint32_t>(y) & k_axis_limit;
    const uint64_t uz = static_cast<uint32_t>(z) & k_axis_limit;
    return (ux << (2 * k_axis_bits)) | (uy << k_axis_bits) | uz;
}

// 整数键的 64 位终混（murmur3 finalizer）：格子键在低位高度规律（z 递增），
// 不混直接取低位会让开放寻址退化成线性探测最坏情形。
inline uint64_t hash_key(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33;
    key *= 0xc4ceb9fe1a85ec53ull;
    key ^= key >> 33;
    return key;
}

// 单簇的累加器
struct ClusterAcc {
    math::Vector3f sum = math::Vector3f::zero();   // 顶点位置和（后面改成质心）
    uint32_t count = 0;
    uint32_t best = k_invalid;    // 离质心最近的**原始**顶点
    float best_d2 = FLT_MAX;
    float max_d2 = 0.0f;          // 簇内顶点到质心的最大距离²（误差代理）
};

// 分块网格平面上"钉住"的顶点集合（只在分块 LOD 时启用）。
// 三个轴各有一组等距平面（分块格线），落在平面 eps 半径内的顶点在所有级别
// 都保持为独立簇 —— 相邻块因此保留同一批顶点/同一个位置，接缝不会开。
struct PinGrid {
    math::Vector3f origin = math::Vector3f::zero();   // 分块栅格起点（模型 AABB 下角）
    math::Vector3f cell = math::Vector3f::zero();     // 每格边长
    int count[3] = {0, 0, 0};                          // 每轴格数
    float eps = 0.0f;

    bool active() const { return eps > 0.0f && (count[0] > 1 || count[1] > 1 || count[2] > 1); }

    bool pinned(const math::Vector3f& p) const {
        const float c[3] = {cell.x, cell.y, cell.z};
        const float v[3] = {p.x, p.y, p.z};
        const float o[3] = {origin.x, origin.y, origin.z};
        for (int a = 0; a < 3; ++a) {
            const int n = count[a];
            if (n <= 1 || !(c[a] > 1e-9f)) continue;
            const float t = (v[a] - o[a]) / c[a];
            // 最外两面不钉：外轮廓只被一块拥有，钉住只会白留三角形
            if (t <= 0.0f || t >= static_cast<float>(n)) continue;
            // 最近格线用 int 取整（+0.5 截断），不要用 std::round —— 它在
            // 这里会被调用上千万次，未内联的 libm 调用能吃掉一秒以上。
            const int nearest = static_cast<int>(t + 0.5f);
            if (nearest <= 0 || nearest >= n) continue;
            if (std::fabs(t - static_cast<float>(nearest)) * c[a] <= eps) return true;
        }
        return false;
    }
};

// 一次聚类过程的临时缓冲（跨级别/跨二分迭代复用，避免反复申请上百 MB）
struct Scratch {
    std::vector<uint32_t> slots;      // 开放寻址表：key 槽 → 簇下标
    std::vector<uint64_t> keys;       // 簇下标 → 格子键（碰撞时确认）
    std::vector<ClusterAcc> acc;
    std::vector<uint32_t> vertex_cluster;   // 原顶点 → 簇下标
    // 分块钉住位图：同一份输入网格在二分搜索里会被聚类 6~7 次，
    // 钉住判定与 cell 无关，缓存一次即可（否则每轮都重算一遍）。
    const MeshData* pin_flags_for = nullptr;
    const PinGrid* pin_flags_grid = nullptr;
    std::vector<uint8_t> pin_flags;
};

struct ClusterResult {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
    float max_error = 0.0f;
};

// 一次固定格子边长的聚类。输出三角数随 cell 增大单调减少（近似）。
// pin 非空时，落在分块格线附近的顶点各自独占一簇（见 PinGrid 的说明）。
void cluster_once(const MeshData& src, const math::Vector3f& bmin, float cell,
                  const PinGrid* pin, Scratch& s, ClusterResult& out) {
    out.vertices.clear();
    out.indices.clear();
    out.max_error = 0.0f;

    const std::size_t nv = src.vertices.size();
    if (nv == 0) return;
    if (!(cell > 1e-9f)) cell = 1e-9f;
    const float inv_cell = 1.0f / cell;

    // 开放寻址表装填率 <= 50%
    std::size_t cap = 16;
    while (cap < nv * 2) cap <<= 1;
    s.slots.assign(cap, k_invalid);
    const std::size_t mask = cap - 1;
    s.keys.clear();
    s.keys.reserve(nv / 4 + 16);
    s.acc.clear();
    s.acc.reserve(nv / 4 + 16);
    s.vertex_cluster.resize(nv);

    const auto cell_coord = [&](float v, float lo) {
        const int g = static_cast<int>(std::floor((v - lo) * inv_cell));
        return static_cast<int>(std::clamp<int64_t>(g, 0, static_cast<int64_t>(k_axis_limit)));
    };

    // ---- 第一遍：分簇 + 累加质心 ----
    // 钉住的顶点用"自己独占一个键"，即簇代表恒为自身、误差贡献恒为 0，
    // 位置/属性原样保留。第 63 位在格子键里恒为 0（三轴各 21 位），不会冲突。
    constexpr uint64_t k_pin_bit = 1ull << 63;
    const bool use_pin = pin && pin->active();
    if (use_pin && (s.pin_flags_for != &src || s.pin_flags_grid != pin)) {
        s.pin_flags.resize(nv);
        for (std::size_t i = 0; i < nv; ++i) {
            s.pin_flags[i] = pin->pinned(src.vertices[i].position) ? 1u : 0u;
        }
        s.pin_flags_for = &src;
        s.pin_flags_grid = pin;
    }
    for (std::size_t i = 0; i < nv; ++i) {
        const math::Vector3f& p = src.vertices[i].position;
        uint64_t key = pack_cell(cell_coord(p.x, bmin.x),
                                 cell_coord(p.y, bmin.y),
                                 cell_coord(p.z, bmin.z));
        if (use_pin && s.pin_flags[i]) key = k_pin_bit | static_cast<uint64_t>(i);
        std::size_t pos = static_cast<std::size_t>(hash_key(key)) & mask;
        uint32_t cluster = k_invalid;
        for (;;) {
            const uint32_t slot = s.slots[pos];
            if (slot == k_invalid) {
                cluster = static_cast<uint32_t>(s.acc.size());
                s.slots[pos] = cluster;
                s.keys.push_back(key);
                s.acc.emplace_back();
                break;
            }
            if (s.keys[slot] == key) {
                cluster = slot;
                break;
            }
            pos = (pos + 1) & mask;
        }
        ClusterAcc& a = s.acc[cluster];
        a.sum += p;
        ++a.count;
        s.vertex_cluster[i] = cluster;
    }

    const uint32_t cluster_count = static_cast<uint32_t>(s.acc.size());
    if (cluster_count == 0) return;
    for (uint32_t c = 0; c < cluster_count; ++c) {
        s.acc[c].sum = s.acc[c].sum / static_cast<float>(s.acc[c].count);
    }

    // ---- 第二遍：选代表顶点（离质心最近的原始顶点）+ 统计误差 ----
    // 用原始顶点当代表而不是质心：位置/法线/UV/切线全部原样保留，
    // 不会在簇之间自造 UV 接缝（摄影测量贴图对 0.5 texel 偏移很敏感）。
    for (std::size_t i = 0; i < nv; ++i) {
        ClusterAcc& a = s.acc[s.vertex_cluster[i]];
        const float d2 = (src.vertices[i].position - a.sum).length_sq();
        if (d2 < a.best_d2) {
            a.best_d2 = d2;
            a.best = static_cast<uint32_t>(i);
        }
        if (d2 > a.max_d2) a.max_d2 = d2;
    }

    if (use_pin && std::getenv("GRYCE_CHUNK_PIN_STATS")) {
        static int logged = 0;
        if (logged < 6) {
            ++logged;
            std::size_t pinned_n = 0;
            for (std::size_t i = 0; i < nv; ++i) pinned_n += s.pin_flags[i] ? 1u : 0u;
            GLOG_INFO("MeshLOD pin stats: nv={} clusters={} pinned={} ({:.1f}%) cell={:.3f}",
                      nv, cluster_count, pinned_n, 100.0 * double(pinned_n) / double(nv), cell);
        }
    }
    out.vertices.resize(cluster_count);
    for (uint32_t c = 0; c < cluster_count; ++c) {
        const uint32_t rep = s.acc[c].best;
        out.vertices[c] = src.vertices[rep < nv ? rep : 0];
        const float e = std::sqrt(s.acc[c].max_d2);
        if (e > out.max_error) out.max_error = e;
    }

    // ---- 重建索引：三顶点同簇的三角形直接丢弃（退化成点/线）----
    const auto push_tri = [&out](uint32_t a, uint32_t b, uint32_t c) {
        if (a == b || b == c || a == c) return;
        out.indices.push_back(a);
        out.indices.push_back(b);
        out.indices.push_back(c);
    };
    if (!src.indices.empty()) {
        const std::size_t tris = src.indices.size() / 3;
        out.indices.reserve(src.indices.size());
        for (std::size_t t = 0; t < tris; ++t) {
            const uint32_t ia = src.indices[t * 3 + 0];
            const uint32_t ib = src.indices[t * 3 + 1];
            const uint32_t ic = src.indices[t * 3 + 2];
            if (ia >= nv || ib >= nv || ic >= nv) continue;
            push_tri(s.vertex_cluster[ia], s.vertex_cluster[ib], s.vertex_cluster[ic]);
        }
    } else {
        for (std::size_t i = 0; i + 2 < nv; i += 3) {
            push_tri(s.vertex_cluster[i], s.vertex_cluster[i + 1], s.vertex_cluster[i + 2]);
        }
    }
}

}  // namespace

float lod_switch_distance(float error, float tan_half_fov, int viewport_height,
                          float pixel_budget) {
    if (!(error > 0.0f)) return 0.0f;
    if (!(tan_half_fov > 1e-6f)) tan_half_fov = 1e-6f;
    if (pixel_budget < 0.05f) pixel_budget = 0.05f;
    const float half_h = 0.5f * static_cast<float>(viewport_height > 0 ? viewport_height : 1080);
    return error * half_h / (tan_half_fov * pixel_budget);
}

namespace {

// pin 非空时构建"分块用"的链：格线附近的顶点在每一级都钉住。
std::shared_ptr<const LodChain> build_chain_internal(const std::shared_ptr<const MeshData>& base,
                                                     const LodBuildSettings& settings,
                                                     int submesh_index,
                                                     const PinGrid* pin) {
    if (!base || base->vertices.empty()) return nullptr;
    const int max_levels = std::clamp(settings.max_levels, 1, k_max_lod_levels);
    if (max_levels <= 1) return nullptr;

    auto chain = std::make_shared<LodChain>();
    chain->source_path = base->path();
    chain->submesh_index = submesh_index;

    // 局部包围盒（聚类栅格与包围球共用）
    math::Vector3f lo = base->vertices[0].position;
    math::Vector3f hi = lo;
    for (const auto& v : base->vertices) {
        lo = lo.min(v.position);
        hi = hi.max(v.position);
    }
    chain->local_min = lo;
    chain->local_max = hi;
    chain->local_radius = ((hi - lo) * 0.5f).length();

    const std::size_t base_tris = base->indices.empty()
                                      ? base->vertices.size() / 3
                                      : base->indices.size() / 3;

    LodLevel lvl0;
    lvl0.mesh = base;
    lvl0.triangles = base_tris;
    lvl0.error = 0.0f;
    lvl0.cumulative_error = 0.0f;
    lvl0.reference_switch_distance = 0.0f;
    chain->levels.push_back(std::move(lvl0));

    const math::Vector3f extent = hi - lo;
    const float diag = extent.length();
    if (!(diag > 1e-6f) || base_tris < settings.min_triangles) {
        GLOG_INFO("MeshLOD: '{}' skipped (diag {:.4f}, {} tris too small)",
                  base->path(), diag, base_tris);
        return nullptr;
    }

    // 参考口径：1080p / 垂直 60° / 2 像素误差（仅用于日志与默认阈值）
    const float k_ref_tan_half = std::tan(30.0f * 3.14159265358979f / 180.0f);
    constexpr int k_ref_height = 1080;
    constexpr float k_ref_pixel = 2.0f;

    Scratch scratch;
    ClusterResult result;
    std::shared_ptr<const MeshData> prev = base;
    float prev_cum_error = 0.0f;
    std::size_t prev_tris = base_tris;

    for (int k = 1; k < max_levels; ++k) {
        const float ratio = std::pow(settings.ratio_per_level, static_cast<float>(k));
        if (ratio < settings.min_ratio) break;

        const std::size_t target = static_cast<std::size_t>(
            std::max(static_cast<double>(settings.min_triangles),
                     static_cast<double>(base_tris) * static_cast<double>(ratio)));
        // 二分 cell 边长：cell 越大 → 簇越少 → 三角数越少（近似单调）。
        // 用几何中点（sqrt(lo*hi)）比算术中点收敛快：三角数 ~ 1/cell²。
        float cell_lo = std::max(diag * 1e-5f, 1e-6f);   // 细
        float cell_hi = std::max(diag * 0.5f, cell_lo * 2.0f);  // 粗
        float best_cell = cell_lo;
        double best_score = 1e300;
        for (int it = 0; it < 6; ++it) {
            const float cell = std::sqrt(cell_lo * cell_hi);
            cluster_once(*prev, lo, cell, pin, scratch, result);
            const std::size_t tris = result.indices.size() / 3;
            if (tris == 0) {
                cell_hi = cell;   // 太粗，全部退化
                continue;
            }
            // 计分：偏离目标的比例；宁可略多面（画质优先），多的一侧不加罚
            const double diff = static_cast<double>(tris) - static_cast<double>(target);
            const double score = diff >= 0.0 ? diff : -diff * 1.6;
            if (score < best_score) {
                best_score = score;
                best_cell = cell;
            }
            if (tris > target) {
                cell_lo = cell;   // 面还太多 → 放大格子
            } else {
                cell_hi = cell;
            }
        }

        cluster_once(*prev, lo, best_cell, pin, scratch, result);
        const std::size_t tris = result.indices.size() / 3;
        if (tris == 0 || tris >= prev_tris * 9 / 10) {
            GLOG_INFO("MeshLOD: '{}' stop at L{} (cannot reduce below {} tris)",
                      base->path(), k, prev_tris);
            break;
        }

        auto lod_mesh = std::make_shared<MeshData>();
        lod_mesh->name = base->name + "_L" + std::to_string(k);
        lod_mesh->vertices = std::move(result.vertices);
        lod_mesh->indices = std::move(result.indices);
        lod_mesh->material = base->material;   // 各级共用同一份材质/贴图路径
        lod_mesh->set_path(base->path());

        LodLevel level;
        level.triangles = tris;
        level.error = result.max_error;
        level.cumulative_error = prev_cum_error + result.max_error;
        level.reference_switch_distance =
            lod_switch_distance(level.cumulative_error, k_ref_tan_half, k_ref_height, k_ref_pixel);
        level.mesh = std::move(lod_mesh);

        GLOG_INFO("MeshLOD: '{}' L{} {} tris ({} verts), level error {:.4f}, "
                  "cumulative {:.4f}, ref switch >= {:.1f}u",
                  base->path(), k, level.triangles, level.mesh->vertices.size(),
                  level.error, level.cumulative_error, level.reference_switch_distance);

        prev_cum_error = level.cumulative_error;
        prev_tris = level.triangles;
        prev = level.mesh;
        chain->levels.push_back(std::move(level));
    }

    if (chain->levels.size() <= 1) return nullptr;

    std::size_t total_verts = 0;
    std::size_t total_tris = 0;
    for (const auto& l : chain->levels) {
        total_verts += l.mesh->vertices.size();
        total_tris += l.triangles;
    }
    GLOG_INFO("MeshLOD: '{}' chain ready: {} levels, {} -> {} tris (all levels {} tris / {} verts), "
              "bounds [{:.2f},{:.2f},{:.2f}]..[{:.2f},{:.2f},{:.2f}] radius {:.2f}",
              base->path(), chain->levels.size(), base_tris, chain->levels.back().triangles,
              total_tris, total_verts,
              lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, chain->local_radius);
    return chain;
}

}  // namespace

std::shared_ptr<const LodChain> build_lod_chain(const std::shared_ptr<const MeshData>& base,
                                                const LodBuildSettings& settings,
                                                int submesh_index) {
    return build_chain_internal(base, settings, submesh_index, nullptr);
}

int choose_chunk_cells(std::size_t base_triangles, std::size_t target_triangles) {
    // 每块 ~20000 三角形是实测的甜点（主 demo 1.46M 三角形 → 每轴 3 块）。
    // 格数不是越多越好：格线越密，"钉住"的顶点越多（每个内部格面都要钉，
    // 都是不可合并的顶点），可简化量被吃掉 —— 实测同一模型
    //   2³(8 块)  20 万三角形/块 → 逐块合计 1025k 三角形，6.33ms
    //   3³(27 块)  2.3 万/块      → 802k，6.15ms   ← 最优
    //   4³(64 块)  1.1 万/块      → 920k，6.48ms   ← 反而回退
    if (target_triangles == 0) target_triangles = 20000;
    const double want = static_cast<double>(base_triangles) / static_cast<double>(target_triangles);
    if (!(want > 1.0)) return 1;
    const int cells = static_cast<int>(std::lround(std::cbrt(want)));
    return std::clamp(cells, 2, 6);
}

std::shared_ptr<const ChunkedLod> build_chunked_lod(const std::shared_ptr<const MeshData>& base,
                                                    const LodBuildSettings& settings,
                                                    int submesh_index, int cells_per_axis) {
    if (!base || base->vertices.empty()) return nullptr;
    const int n = std::clamp(cells_per_axis, 1, 8);
    if (n <= 1) return nullptr;

    const std::size_t nv = base->vertices.size();
    const bool indexed = !base->indices.empty();
    const std::size_t tri_count = indexed ? base->indices.size() / 3 : nv / 3;
    if (tri_count < 4) return nullptr;

    // ---- 1) 模型 AABB 与分块栅格 ----
    math::Vector3f lo = base->vertices[0].position;
    math::Vector3f hi = lo;
    for (const auto& v : base->vertices) {
        lo = lo.min(v.position);
        hi = hi.max(v.position);
    }
    const math::Vector3f extent = hi - lo;
    const float diag = extent.length();
    if (!(diag > 1e-6f)) return nullptr;

    math::Vector3f cell(extent.x / static_cast<float>(n), extent.y / static_cast<float>(n),
                        extent.z / static_cast<float>(n));
    // 退化轴（薄片/平面）给个下限，否则整块都落在同一格
    const float min_cell = std::max(diag * 1e-3f, 1e-6f);
    cell.x = std::max(cell.x, min_cell);
    cell.y = std::max(cell.y, min_cell);
    cell.z = std::max(cell.z, min_cell);

    PinGrid pin;
    pin.origin = lo;
    pin.cell = cell;
    pin.count[0] = pin.count[1] = pin.count[2] = n;
    // 格线附近多宽的带里的顶点要钉住：三角形尺度远小于格边长，取格边的 2%
    // 足以覆盖"跨格线三角形"的顶点，又不会把整块都钉死。
    const float max_cell = std::max(cell.x, std::max(cell.y, cell.z));
    pin.eps = std::max(max_cell * 0.02f, diag * 1e-5f);

    // ---- 2) 三角形按质心归属（整三角形归属，不切开）----
    const int chunks_per_layer = n * n;
    std::vector<std::vector<uint32_t>> chunk_tris(static_cast<std::size_t>(n) * n * n);
    const auto grid_coord = [&](float v, float o, float c) {
        const int g = static_cast<int>(std::floor((v - o) / c));
        return std::clamp(g, 0, n - 1);
    };
    for (std::size_t t = 0; t < tri_count; ++t) {
        uint32_t ia, ib, ic;
        if (indexed) {
            ia = base->indices[t * 3 + 0];
            ib = base->indices[t * 3 + 1];
            ic = base->indices[t * 3 + 2];
            if (ia >= nv || ib >= nv || ic >= nv) continue;
        } else {
            ia = static_cast<uint32_t>(t * 3 + 0);
            ib = static_cast<uint32_t>(t * 3 + 1);
            ic = static_cast<uint32_t>(t * 3 + 2);
            if (ic >= nv) break;
        }
        const math::Vector3f centroid =
            (base->vertices[ia].position + base->vertices[ib].position +
             base->vertices[ic].position) / 3.0f;
        const int gx = grid_coord(centroid.x, lo.x, cell.x);
        const int gy = grid_coord(centroid.y, lo.y, cell.y);
        const int gz = grid_coord(centroid.z, lo.z, cell.z);
        chunk_tris[static_cast<std::size_t>(gz * chunks_per_layer + gy * n + gx)].push_back(
            static_cast<uint32_t>(t));
    }

    // ---- 3) 每块重建紧凑网格并生成 LOD 链 ----
    const auto t_begin = std::chrono::steady_clock::now();
    double ms_assemble = 0.0;
    double ms_chain = 0.0;
    auto result = std::make_shared<ChunkedLod>();
    result->source_path = base->path();
    result->submesh_index = submesh_index;
    result->base_triangles = tri_count;
    result->cells_per_axis = n;

    std::vector<std::pair<uint32_t, uint32_t>> remap;   // 全局顶点 → 块内顶点
    std::unordered_map<uint32_t, uint32_t> lookup;
    std::size_t total_lod_verts = 0;
    int l0_only_chunks = 0;

    for (int gz = 0; gz < n; ++gz) {
        for (int gy = 0; gy < n; ++gy) {
            for (int gx = 0; gx < n; ++gx) {
                const std::size_t ci = static_cast<std::size_t>(gz * chunks_per_layer + gy * n + gx);
                if (chunk_tris[ci].empty()) continue;

                auto sub = std::make_shared<MeshData>();
                sub->name = base->name + "_c" + std::to_string(gx) + std::to_string(gy) +
                            std::to_string(gz);
                sub->material = base->material;
                sub->set_path(base->path());
                lookup.clear();
                sub->indices.reserve(chunk_tris[ci].size() * 3);
                math::Vector3f clo = math::Vector3f::zero();
                math::Vector3f chi = math::Vector3f::zero();
                bool first_vertex = true;
                for (uint32_t t : chunk_tris[ci]) {
                    uint32_t src_idx[3];
                    if (indexed) {
                        src_idx[0] = base->indices[t * 3 + 0];
                        src_idx[1] = base->indices[t * 3 + 1];
                        src_idx[2] = base->indices[t * 3 + 2];
                    } else {
                        src_idx[0] = t * 3 + 0;
                        src_idx[1] = t * 3 + 1;
                        src_idx[2] = t * 3 + 2;
                    }
                    for (uint32_t gi : src_idx) {
                        auto it = lookup.find(gi);
                        uint32_t li;
                        if (it == lookup.end()) {
                            li = static_cast<uint32_t>(sub->vertices.size());
                            lookup.emplace(gi, li);
                            sub->vertices.push_back(base->vertices[gi]);
                            const math::Vector3f& p = base->vertices[gi].position;
                            if (first_vertex) {
                                clo = chi = p;
                                first_vertex = false;
                            } else {
                                clo = clo.min(p);
                                chi = chi.max(p);
                            }
                        } else {
                            li = it->second;
                        }
                        sub->indices.push_back(li);
                    }
                }

                LodChunk chunk;
                chunk.local_min = clo;
                chunk.local_max = chi;
                chunk.local_radius = (chi - clo).length() * 0.5f;
                chunk.grid[0] = gx;
                chunk.grid[1] = gy;
                chunk.grid[2] = gz;
                const auto t_chain0 = std::chrono::steady_clock::now();
                // GRYCE_CHUNK_NO_PIN=1：不钉接缝顶点。用于验证"裂缝是钉顶点解决的"
                // 与测量钉顶点本身的代价（两侧必须同开同关才可比）。
                static const bool no_pin = std::getenv("GRYCE_CHUNK_NO_PIN") != nullptr;
                chunk.chain = build_chain_internal(sub, settings, submesh_index,
                                                   no_pin ? nullptr : &pin);
                ms_chain += std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t_chain0).count();
                if (!chunk.chain) {
                    // 这块三角形太少（< settings.min_triangles），生成不了多级链。
                    // **不能就此丢掉**：分块绘制是逐块画的，少一块就是少一块几何
                    // （模型上直接出现空洞）。退化成长度为 1 的链，永远用 L0。
                    auto only = std::make_shared<LodChain>();
                    only->source_path = base->path();
                    only->submesh_index = submesh_index;
                    LodLevel l0;
                    l0.mesh = sub;
                    l0.triangles = sub->indices.size() / 3;
                    l0.error = 0.0f;
                    l0.cumulative_error = 0.0f;
                    l0.reference_switch_distance = 0.0f;
                    only->levels.push_back(std::move(l0));
                    only->local_min = clo;
                    only->local_max = chi;
                    only->local_radius = chunk.local_radius;
                    chunk.chain = std::move(only);
                    ++l0_only_chunks;
                }
                total_lod_verts += chunk.chain->levels.back().mesh->vertices.size();
                result->chunks.push_back(std::move(chunk));
            }
        }
    }

    if (result->chunks.size() <= 1) return nullptr;

    int max_levels = 1;
    for (const auto& c : result->chunks) {
        if (c.chain) max_levels = std::max(max_levels, c.chain->level_count());
    }
    result->max_levels = max_levels;
    if (max_levels <= 1) return nullptr;

    const double ms_total = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t_begin).count();
    ms_assemble = ms_total - ms_chain;
    GLOG_INFO("MeshLOD: '{}' chunked into {} blocks ({}x{}x{}, cell {:.2f}/{:.2f}/{:.2f}, "
              "pin eps {:.3f}), max {} levels, {} tris ({} blocks L0-only); build {:.2f}ms "
              "(assemble {:.2f} + chain {:.2f})",
              base->path(), result->chunks.size(), n, n, n, cell.x, cell.y, cell.z, pin.eps,
              max_levels, tri_count, l0_only_chunks, ms_total, ms_assemble, ms_chain);
    (void)total_lod_verts;
    return result;
}

}  // namespace gryce_engine::assets
