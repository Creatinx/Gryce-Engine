#include "assets/mesh_optimizer.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "assets/mesh_data.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::assets {
namespace {

constexpr uint32_t k_empty_slot = 0xFFFFFFFFu;

// MeshVertex 是纯 POD 的 14 个 float（56 字节，无 padding，下面有 static_assert）。
// 哈希按 32 位字扫一遍，位级相等 —— 与 memcmp 的判等口径完全一致。
inline uint64_t hash_vertex(const MeshVertex& v) {
    const auto* words = reinterpret_cast<const uint32_t*>(&v);
    constexpr std::size_t k_words = sizeof(MeshVertex) / sizeof(uint32_t);
    uint64_t h = 1469598103934665603ull;   // FNV-1a offset basis
    for (std::size_t i = 0; i < k_words; ++i) {
        h ^= words[i];
        h *= 1099511628211ull;             // FNV prime
    }
    // 折一下高位：开放寻址只用低位选桶，不折容易被"末尾是相同的 uv"聚堆
    return h ^ (h >> 29);
}

static_assert(sizeof(MeshVertex) == 14 * sizeof(float),
              "MeshVertex 必须是 14 个 float 紧密排列；若加了成员请同步修改哈希");

}  // namespace

bool has_trivial_indices(const MeshData& mesh) {
    if (mesh.indices.empty()) return true;
    if (mesh.indices.size() != mesh.vertices.size()) return false;
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) {
        if (mesh.indices[i] != static_cast<uint32_t>(i)) return false;
    }
    return true;
}

WeldStats weld_mesh(MeshData& mesh) {
    WeldStats stats;
    stats.vertices_before = mesh.vertices.size();
    stats.triangles_before = mesh.indices.size() / 3;
    stats.was_trivial_index = has_trivial_indices(mesh);

    const std::size_t n = mesh.vertices.size();
    if (n < 3) {
        stats.vertices_after = n;
        stats.triangles_after = stats.triangles_before;
        return stats;
    }

    // 没有索引的网格（部分是 draw-arrays 风格）：先补成顺序索引，
    // 否则后面既不能焊接、渲染侧也只有 glDrawElements 一条路（会画不出东西）。
    if (mesh.indices.empty()) {
        mesh.indices.resize(n);
        for (std::size_t i = 0; i < n; ++i) mesh.indices[i] = static_cast<uint32_t>(i);
        stats.was_trivial_index = true;
    }

    // 开放寻址：装填率 <= 50%（表大小 >= 2n），线性探测足够快且无指针追逐。
    std::size_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    const std::size_t mask = cap - 1;

    std::vector<uint32_t> slots(cap, k_empty_slot);
    std::vector<uint32_t> remap(n, k_empty_slot);
    std::vector<MeshVertex> welded;
    welded.reserve(n / 2 + 16);

    const std::vector<MeshVertex>& src = mesh.vertices;
    for (std::size_t i = 0; i < n; ++i) {
        const MeshVertex& v = src[i];
        std::size_t pos = static_cast<std::size_t>(hash_vertex(v)) & mask;
        for (;;) {
            const uint32_t slot = slots[pos];
            if (slot == k_empty_slot) {
                const uint32_t new_index = static_cast<uint32_t>(welded.size());
                welded.push_back(v);
                slots[pos] = new_index;
                remap[i] = new_index;
                break;
            }
            // 位级相等才算同一个顶点：任何属性有差异都必须保留成两个顶点，
            // 否则 UV/法线会被错合（表现为贴图拉伸、硬边变软、法线贴图错位）。
            if (std::memcmp(&welded[slot], &v, sizeof(MeshVertex)) == 0) {
                remap[i] = slot;
                break;
            }
            pos = (pos + 1) & mask;
        }
    }

    stats.vertices_after = welded.size();
    if (welded.size() == n) {
        // 没有可合并的顶点：保持原顶点/索引不动，省一次大块拷贝。
        stats.triangles_after = stats.triangles_before;
        return stats;
    }

    // 重建索引并剔除退化三角形（两个及以上索引相同 → 光栅化不出任何像素）。
    const std::size_t tri_count = mesh.indices.size() / 3;
    std::vector<uint32_t> new_indices;
    new_indices.reserve(mesh.indices.size());
    for (std::size_t t = 0; t < tri_count; ++t) {
        const uint32_t a = remap[mesh.indices[t * 3 + 0]];
        const uint32_t b = remap[mesh.indices[t * 3 + 1]];
        const uint32_t c = remap[mesh.indices[t * 3 + 2]];
        if (a == b || b == c || a == c) {
            ++stats.degenerate_removed;
            continue;
        }
        new_indices.push_back(a);
        new_indices.push_back(b);
        new_indices.push_back(c);
    }

    mesh.vertices = std::move(welded);
    mesh.indices = std::move(new_indices);
    stats.triangles_after = mesh.indices.size() / 3;
    stats.welded = true;
    return stats;
}

WeldStats optimize_imported_mesh(MeshData& mesh, const char* source_label) {
    const WeldStats stats = weld_mesh(mesh);

    const double before_mb =
        static_cast<double>(stats.vertices_before) * sizeof(MeshVertex) / (1024.0 * 1024.0);
    const double after_mb =
        static_cast<double>(stats.vertices_after) * sizeof(MeshVertex) / (1024.0 * 1024.0);

    if (stats.welded) {
        GLOG_INFO("MeshOptimizer[{}]: '{}' vertices {} -> {} ({:.1f}x, {:.1f}MB -> {:.1f}MB), "
                  "tris {} -> {} (degenerate {}){}",
                  source_label ? source_label : "?", mesh.name,
                  stats.vertices_before, stats.vertices_after,
                  stats.vertices_after > 0
                      ? static_cast<double>(stats.vertices_before) / static_cast<double>(stats.vertices_after)
                      : 0.0,
                  before_mb, after_mb, stats.triangles_before, stats.triangles_after,
                  stats.degenerate_removed,
                  stats.was_trivial_index ? ", source was fully de-indexed" : "");
    } else if (stats.was_trivial_index && stats.vertices_before > 0) {
        // 没焊出东西但仍值得提一句：说明这个资产的顶点本来就不重复
        GLOG_INFO("MeshOptimizer[{}]: '{}' already fully de-indexed with no duplicate vertices "
                  "({} verts), nothing to weld",
                  source_label ? source_label : "?", mesh.name, stats.vertices_before);
    }
    return stats;
}

}  // namespace gryce_engine::assets
