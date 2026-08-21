#pragma once

#include <cstdint>
#include <vector>

#include "render/rhi_handle.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// DecalData — 单个贴花数据
// ---------------------------------------------------------------------------
struct DecalData {
    math::Vector3f position;        // 世界空间位置
    math::Vector3f rotation;        // 欧拉角
    math::Vector3f scale;           // 贴花 AABB 尺寸
    math::Vector3f albedo;          // 基础颜色（当无纹理时使用）
    float albedo_tex_handle = 0;    // 纹理句柄（简化：使用 float 传递）
    float normal_tex_handle = 0;
    float roughness = 0.5f;
    float metallic = 0.0f;
    float normal_strength = 1.0f;
    float opacity = 1.0f;
    bool enabled = true;
};

// ---------------------------------------------------------------------------
// DecalStorage — 贴花存储
// 管理场景中的贴花数据，提供 GPU 上传和查询接口。
// ---------------------------------------------------------------------------
class DecalStorage {
public:
    static constexpr int k_max_decals = 128;

    DecalStorage() = default;
    ~DecalStorage() = default;

    void init(RenderContext* ctx);
    void destroy();

    int add_decal(const DecalData& decal);
    void remove_decal(int index);
    void update_decal(int index, const DecalData& decal);
    void clear();

    const DecalData& get_decal(int index) const { return decals_[index]; }
    int decal_count() const { return static_cast<int>(decals_.size()); }
    const std::vector<DecalData>& decals() const { return decals_; }

private:
    RenderContext* ctx_ = nullptr;
    std::vector<DecalData> decals_;
};

} // namespace gryce_engine::render