#pragma once

#include <cstdint>
#include <vector>

#include "render/rhi_handle.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderContext;

// ---------------------------------------------------------------------------
// VoxelGI_RD — 体素全局光照
// 参考 Godot 的 VoxelGI 实现：
// 预先体素化静态场景几何体，从光源注入光照，提供低频间接光照。
// ---------------------------------------------------------------------------
class VoxelGI_RD {
public:
    static constexpr int k_default_voxel_size = 64;

    VoxelGI_RD() = default;
    ~VoxelGI_RD() { destroy(); }

    bool init(RenderContext* ctx);
    void destroy();

    bool create(const math::Vector3f& bounds_min, const math::Vector3f& bounds_max,
                int voxel_size = k_default_voxel_size);
    void destroy_voxels();

    // 光照注入（灯光变化时触发）
    void inject_light(const math::Vector3f& position, const math::Vector3f& color,
                      float intensity, const math::Vector3f& direction);

    RHITextureHandle voxel_tex() const { return voxel_tex_; }
    bool valid() const { return voxel_tex_.is_valid(); }

private:
    RenderContext* ctx_ = nullptr;

    math::Vector3f bounds_min_;
    math::Vector3f bounds_max_;
    int voxel_size_ = 64;

    // 体素 3D 纹理
    RHITextureHandle voxel_tex_;
    RHITextureHandle irradiance_tex_;
    RHIFramebufferHandle fbo_;

    bool initialized_ = false;
};

} // namespace gryce_engine::render