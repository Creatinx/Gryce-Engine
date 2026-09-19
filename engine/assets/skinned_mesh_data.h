#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "animation/animation_clip.h"
#include "animation/skeleton.h"
#include "assets/mesh_data.h"

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// BoneInfluence — 单顶点的骨骼影响（最多 4 个，按权重降序）。
// 布局与 GPU 顶点属性对齐：下轮直接作为 bone ids（UInt4）/ weights（Float4）
// 上传；空槽 weight=0，着色器侧贡献为 0。
// ---------------------------------------------------------------------------
struct BoneInfluence {
    static constexpr int k_max_influences = 4;

    uint16_t bone_ids[k_max_influences] = {0, 0, 0, 0};
    float    weights[k_max_influences]  = {0.0f, 0.0f, 0.0f, 0.0f};

    // 记录一条 (bone, weight)，只保留权重最高的 4 个（降序）。
    // 同一 bone 重复出现时权重累加并保持降序。
    void add(int bone_index, float weight) {
        if (bone_index < 0 || bone_index > 0xFFFF || weight <= 0.0f) return;
        const uint16_t id = static_cast<uint16_t>(bone_index);

        // 同 bone 去重累加
        for (int i = 0; i < k_max_influences && weights[i] > 0.0f; ++i) {
            if (bone_ids[i] == id) {
                weights[i] += weight;
                // 上浮保持降序
                while (i > 0 && weights[i] > weights[i - 1]) {
                    std::swap(weights[i], weights[i - 1]);
                    std::swap(bone_ids[i], bone_ids[i - 1]);
                    --i;
                }
                return;
            }
        }

        // 找降序插入位置；4 槽全满且都比它大则丢弃（截断）
        int insert = -1;
        for (int i = 0; i < k_max_influences; ++i) {
            if (weights[i] < weight) { insert = i; break; }
        }
        if (insert < 0) return;
        for (int i = k_max_influences - 1; i > insert; --i) {
            bone_ids[i] = bone_ids[i - 1];
            weights[i] = weights[i - 1];
        }
        bone_ids[insert] = id;
        weights[insert] = weight;
    }

    // 权重和归一化；全零（未绑定顶点）保持全零。
    void normalize() {
        float sum = 0.0f;
        for (float w : weights) sum += w;
        if (sum <= 1e-8f) return;
        const float inv = 1.0f / sum;
        for (float& w : weights) w *= inv;
    }

    float weight_sum() const {
        float sum = 0.0f;
        for (float w : weights) sum += w;
        return sum;
    }
};

// ---------------------------------------------------------------------------
// SkinnedMeshData — 带蒙皮数据的网格资源。
// 继承 MeshData 复用现有顶点/索引/材质提取与上传路径；
// bone_influences 与 vertices 一一对应，为空表示该网格无蒙皮。
// ---------------------------------------------------------------------------
struct SkinnedMeshData : public MeshData {
    std::vector<BoneInfluence> bone_influences;

    const char* type() const override { return "SkinnedMeshData"; }

    bool has_skin() const { return !bone_influences.empty(); }
};

// ---------------------------------------------------------------------------
// SkinnedModelData — 带骨架与动画的模型导入结果（纯 CPU 值语义，
// 可在 AsyncLoader 工作线程构造后跨线程移动）。
// ---------------------------------------------------------------------------
struct SkinnedModelData {
    std::vector<SkinnedMeshData> meshes;
    animation::Skeleton skeleton;
    std::vector<animation::AnimationClip> animations;
    bool has_skin = false;
};

} // namespace gryce_engine::assets
