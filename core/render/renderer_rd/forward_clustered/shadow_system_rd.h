#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "render/export.h"
#include "render/rhi_handle.h"
#include "math/math.h"
#include "math/camera.h"
#include "render/storage_rd/light_storage.h"

namespace gryce_engine::render {

class RenderContext;
class ITexture;
class IFramebuffer;

// ---------------------------------------------------------------------------
// ShadowSystemRD — Forward Clustered 阴影系统
// 管理 CSM 级联阴影（方向光）和单阴影贴图（聚光灯/点光）。
// 类似于 Godot 的 RendererSceneRenderRD 中的阴影系统。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API ShadowSystemRD {
public:
    static constexpr int k_max_cascades = 4;
    static constexpr int k_default_csm_size = 2048;
    static constexpr int k_default_spot_shadow_size = 1024;

    // 阴影光源渲染数据
    struct ShadowLight {
        LightType type = LightType::Directional;
        math::Vector3f position;
        math::Vector3f direction;
        float range = 10.0f;
        float spot_angle = 45.0f;
        float spot_softness = 0.2f;
        float bias = 0.001f;
        float normal_bias = 0.001f;
        uint32_t shadow_map_id = 0;       // 对应的阴影贴图索引
        bool shadow_enabled = true;
    };

    ShadowSystemRD();
    ~ShadowSystemRD();

    bool init(RenderContext* ctx);
    void shutdown();

    // 每帧更新：根据相机和光源计算级联分割、生成阴影贴图
    void update(const math::Camera& camera,
                const std::vector<ShadowLight>& shadow_lights,
                int viewport_width, int viewport_height);

    // 渲染阴影贴图（遍历所有需要阴影的光源）
    void render_shadows(RenderContext* ctx);

    // 级联阴影贴图访问
    RHITextureHandle cascade_shadow_tex(int cascade) const { return cascade_shadow_tex_[cascade]; }
    RHIFramebufferHandle cascade_shadow_fbo(int cascade) const { return cascade_shadow_fbo_[cascade]; }
    const math::Matrix4f& cascade_light_matrix(int cascade) const { return cascade_light_matrices_[cascade]; }
    float cascade_split(int cascade) const { return cascade_splits_[cascade]; }
    int cascade_count() const { return cascade_count_; }

    // 聚光灯阴影贴图访问
    size_t spot_shadow_count() const { return spot_shadow_tex_.size(); }
    RHITextureHandle spot_shadow_tex(size_t index) const { return index < spot_shadow_tex_.size() ? spot_shadow_tex_[index] : RHITextureHandle{}; }
    RHIFramebufferHandle spot_shadow_fbo(size_t index) const { return index < spot_shadow_fbo_.size() ? spot_shadow_fbo_[index] : RHIFramebufferHandle{}; }
    const math::Matrix4f& spot_light_matrix(size_t index) const { return index < spot_light_matrices_.size() ? spot_light_matrices_[index] : math::Matrix4f::identity(); }

    // 点光源阴影（双抛物面映射）访问
    size_t point_shadow_count() const { return point_shadow_tex_.size(); }
    RHITextureHandle point_shadow_tex(size_t index) const { return index < point_shadow_tex_.size() ? point_shadow_tex_[index] : RHITextureHandle{}; }
    RHIFramebufferHandle point_shadow_fbo(size_t index, int face) const {
        return (index < point_shadow_fbo_.size() && face >= 0 && face < 2) ? point_shadow_fbo_[index][face] : RHIFramebufferHandle{};
    }
    const math::Matrix4f& point_light_view_matrix(size_t index, int face) const {
        static const math::Matrix4f k_identity = math::Matrix4f::identity();
        return (index < point_light_view_matrices_.size() && face >= 0 && face < 2) ? point_light_view_matrices_[index][face] : k_identity;
    }
    const math::Vector3f& point_light_position(size_t index) const {
        static const math::Vector3f k_zero = math::Vector3f::zero();
        return (index < point_light_positions_.size()) ? point_light_positions_[index] : k_zero;
    }

    // 获取所有光照投影矩阵（CSM + Spot + Point）
    const std::vector<math::Matrix4f>& all_light_matrices() const { return light_matrices_; }

    // 设置参数
    void set_cascade_sizes(const std::array<int, k_max_cascades>& sizes) { cascade_sizes_ = sizes; }
    void set_cascade_biases(const std::array<float, k_max_cascades>& biases) { cascade_biases_ = biases; }
    void set_cascade_split_lambda(float lambda) { cascade_split_lambda_ = lambda; }
    void set_spot_shadow_size(int size) { spot_shadow_size_ = size; }
    void set_point_shadow_size(int size) { point_shadow_size_ = size; }

    // CSM 阴影贴图参数访问（供 shader 使用）
    const float* cascade_splits_data() const { return cascade_splits_.data(); }
    const math::Matrix4f* cascade_light_matrices_data() const { return cascade_light_matrices_.data(); }

private:
    // 计算级联分割距离
    void _compute_cascade_splits(const math::Camera& camera);

    // 计算单个级联的光照矩阵（稳定 CSM 算法）
    math::Matrix4f _compute_cascade_light_matrix(
        const math::Camera& camera, int cascade,
        const math::Vector3f& light_dir);

    // 渲染单个级联阴影
    void _render_cascade(RenderContext* ctx, int cascade,
                         const math::Vector3f& light_dir);

    // 渲染单个聚光灯阴影
    void _render_spot_shadow(RenderContext* ctx, int index,
                             const ShadowLight& light);

    // 渲染单个点光源阴影（双抛物面映射）
    void _render_point_shadow_face(RenderContext* ctx, int index, int face,
                                   const ShadowLight& light);

    RenderContext* ctx_ = nullptr;
    bool initialized_ = false;

    // --- CSM ---
    std::array<int, k_max_cascades> cascade_sizes_ = {k_default_csm_size, 1024, 512, 512};
    std::array<float, k_max_cascades> cascade_biases_ = {0.0005f, 0.001f, 0.002f, 0.004f};
    std::array<RHITextureHandle, k_max_cascades> cascade_shadow_tex_;
    std::array<RHIFramebufferHandle, k_max_cascades> cascade_shadow_fbo_;
    std::array<math::Matrix4f, k_max_cascades> cascade_light_matrices_;
    std::array<float, k_max_cascades + 1> cascade_splits_;  // 分割距离（观察空间）
    int cascade_count_ = 3;
    float cascade_split_lambda_ = 0.5f;

    // --- 聚光灯阴影 ---
    int spot_shadow_size_ = k_default_spot_shadow_size;
    std::vector<RHITextureHandle> spot_shadow_tex_;
    std::vector<RHIFramebufferHandle> spot_shadow_fbo_;
    std::vector<math::Matrix4f> spot_light_matrices_;

    // --- 点光源阴影（双抛物面映射）---
    static constexpr int k_default_point_shadow_size = 512;
    int point_shadow_size_ = k_default_point_shadow_size;
    std::vector<RHITextureHandle> point_shadow_tex_;           // 每点光源 1 张纹理（双抛物面 packed）
    std::vector<std::array<RHIFramebufferHandle, 2>> point_shadow_fbo_; // 2 个 face 各一个 FBO
    std::vector<std::array<math::Matrix4f, 2>> point_light_view_matrices_; // 2 个 face 各一个 view
    std::vector<math::Vector3f> point_light_positions_;       // 光源位置

    // 所有光照投影矩阵（CSM + Spot + Point 合并）
    std::vector<math::Matrix4f> light_matrices_;

    // 当前帧的阴影光源列表
    std::vector<ShadowLight> current_shadow_lights_;
    math::Vector3f current_light_dir_;
    bool has_directional_shadow_ = false;
};

} // namespace gryce_engine::render