#include "render/renderer_rd/cluster_builder_rd.h"
#include "math/camera.h"
#include "math/math.h"
#include "render/render_context.h"
#include "render/storage_rd/light_storage.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace gryce_engine::render {

ClusterBuilderRD::ClusterBuilderRD() = default;

ClusterBuilderRD::~ClusterBuilderRD() {
    shutdown();
}

bool ClusterBuilderRD::init(RenderContext* ctx) {
    if (initialized_) return true;
    ctx_ = ctx;
    initialized_ = true;
    return true;
}

void ClusterBuilderRD::shutdown() {
    if (initialized_) {
        light_index_buffer_.clear();
        light_index_scratch_.clear();
        cluster_buffer_.clear();
        culled_lights_.clear();
        initialized_ = false;
    }
}

int ClusterBuilderRD::_compute_tile_count(int pixels, int tile_size) const {
    return (pixels + tile_size - 1) / tile_size;
}

int ClusterBuilderRD::_compute_depth_slice(float depth, float z_near, float z_far) const {
    // Godot 使用 log 空间划分 depth slice
    // slice = log2(depth / z_near) / log2(z_far / z_near) * layers
    if (depth <= z_near) return 0;
    if (depth >= z_far) return cluster_z_layers_ - 1;
    float log_depth = std::log2(depth / z_near);
    float log_range = std::log2(z_far / z_near);
    return static_cast<int>(log_depth / log_range * cluster_z_layers_);
}

int ClusterBuilderRD::cluster_index(int tile_x, int tile_y, int slice_z) const {
    return slice_z * (tile_count_x_ * tile_count_y_) + tile_y * tile_count_x_ + tile_x;
}

int ClusterBuilderRD::cluster_index(int screen_x, int screen_y, float depth) const {
    int tx = std::min(screen_x / k_tile_size_x, tile_count_x_ - 1);
    int ty = std::min(screen_y / k_tile_size_y, tile_count_y_ - 1);
    int tz = _compute_depth_slice(depth, 0.1f, 1000.0f);
    return cluster_index(tx, ty, tz);
}

// ---------------------------------------------------------------------------
// 集群 AABB 计算（在视锥体空间中）
// 使用逆投影矩阵将 NDC 角点变换到观察空间，计算 AABB。
// 注意：out_min/out_max 使用引用传回。
// ---------------------------------------------------------------------------
static void compute_cluster_aabb(
    int tile_x, int tile_y, int slice_z,
    int tile_count_x, int tile_count_y, int cluster_z_layers,
    float z_near, float z_far,
    const math::Matrix4f& inv_proj,
    math::Vector3f& out_min, math::Vector3f& out_max)
{
    // 计算 tile 在 NDC 空间的边界
    float ndc_left = (float(tile_x) / tile_count_x) * 2.0f - 1.0f;
    float ndc_right = (float(tile_x + 1) / tile_count_x) * 2.0f - 1.0f;
    float ndc_bottom = (float(tile_y) / tile_count_y) * 2.0f - 1.0f;
    float ndc_top = (float(tile_y + 1) / tile_count_y) * 2.0f - 1.0f;

    // 计算深度边界（log 空间）
    float log_range = std::log2(z_far / z_near);
    float slice_depth_min = z_near * std::exp2(float(slice_z) / cluster_z_layers * log_range);
    float slice_depth_max = z_near * std::exp2(float(slice_z + 1) / cluster_z_layers * log_range);

    // 将 NDC 点反投影到观察空间
    // 使用逆投影矩阵: view_pos = inv_proj * ndc_pos
    // 对于透视投影，需要除以 w 分量
    out_min = math::Vector3f(FLT_MAX, FLT_MAX, FLT_MAX);
    out_max = math::Vector3f(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    // 近平面和远平面在观察空间的 z 值（OpenGL 约定：z 负方向为前）
    // 但集群 AABB 使用正深度值，所以我们取绝对值
    // 近平面在观察空间 z = -slice_depth_min, 远平面 z = -slice_depth_max
    const float ndc_z_near = (z_near + z_far) / (z_near - z_far) + (2.0f * z_near * z_far) / (z_near - z_far) / (-slice_depth_min);
    const float ndc_z_far  = (z_near + z_far) / (z_near - z_far) + (2.0f * z_near * z_far) / (z_near - z_far) / (-slice_depth_max);

    // 简化：使用近/远平面深度在 NDC 的近似值
    // 对于透视投影，ndc_z = (A*z + B) / (-z) = -A - B/z
    // 其中 A = (far+near)/(near-far), B = 2*far*near/(near-far)
    // 所以 ndc_z = -(far+near)/(near-far) - 2*far*near/(near-far)/z
    // 对于 z = -slice_depth_min: ndc_z = -(far+near)/(near-far) + 2*far*near/(near-far)/slice_depth_min

    // 使用 4 个角点 + 近/远 2 个深度，共 8 个点
    float ndc_znear = (z_near + z_far) / (z_near - z_far) + (2.0f * z_near * z_far) / (z_near - z_far) / (-slice_depth_min);
    float ndc_zfar  = (z_near + z_far) / (z_near - z_far) + (2.0f * z_near * z_far) / (z_near - z_far) / (-slice_depth_max);

    // 裁剪到 [-1, 1] 范围
    ndc_znear = std::max(-1.0f, std::min(1.0f, ndc_znear));
    ndc_zfar  = std::max(-1.0f, std::min(1.0f, ndc_zfar));

    // 8 个 NDC 角点
    float ndc_corners[8][3] = {
        {ndc_left,  ndc_bottom, ndc_znear},
        {ndc_right, ndc_bottom, ndc_znear},
        {ndc_left,  ndc_top,    ndc_znear},
        {ndc_right, ndc_top,    ndc_znear},
        {ndc_left,  ndc_bottom, ndc_zfar},
        {ndc_right, ndc_bottom, ndc_zfar},
        {ndc_left,  ndc_top,    ndc_zfar},
        {ndc_right, ndc_top,    ndc_zfar},
    };

    for (int i = 0; i < 8; ++i) {
        math::Vector4f ndc_pt(ndc_corners[i][0], ndc_corners[i][1], ndc_corners[i][2], 1.0f);
        math::Vector4f view_pt = inv_proj * ndc_pt;
        // 透视除法
        if (std::abs(view_pt.w) > 1e-8f) {
            view_pt.x /= view_pt.w;
            view_pt.y /= view_pt.w;
            view_pt.z /= view_pt.w;
        }

        // 使用绝对值作为深度（正数方便比较）
        float vx = view_pt.x;
        float vy = view_pt.y;
        float vz = std::abs(view_pt.z);

        if (vx < out_min.x) out_min.x = vx;
        if (vy < out_min.y) out_min.y = vy;
        if (vz < out_min.z) out_min.z = vz;
        if (vx > out_max.x) out_max.x = vx;
        if (vy > out_max.y) out_max.y = vy;
        if (vz > out_max.z) out_max.z = vz;
    }

    // 确保 AABB 至少有一个最小尺寸（避免零体积集群导致相交测试失败）
    const float k_min_extent = 0.001f;
    auto fix_axis = [&](float& min_v, float& max_v) {
        if (max_v - min_v < k_min_extent) {
            float center = (min_v + max_v) * 0.5f;
            min_v = center - k_min_extent * 0.5f;
            max_v = center + k_min_extent * 0.5f;
        }
    };
    fix_axis(out_min.x, out_max.x);
    fix_axis(out_min.y, out_max.y);
    fix_axis(out_min.z, out_max.z);
}

// ---------------------------------------------------------------------------
// 球体与 AABB 相交测试
// ---------------------------------------------------------------------------
static bool sphere_aabb_intersect(
    const math::Vector3f& sphere_center, float sphere_radius,
    const math::Vector3f& aabb_min, const math::Vector3f& aabb_max)
{
    float dist_sq = 0.0f;
    auto test_axis = [&](float v, float min_v, float max_v) {
        if (v < min_v) dist_sq += (min_v - v) * (min_v - v);
        if (v > max_v) dist_sq += (v - max_v) * (v - max_v);
    };
    test_axis(sphere_center.x, aabb_min.x, aabb_max.x);
    test_axis(sphere_center.y, aabb_min.y, aabb_max.y);
    test_axis(sphere_center.z, aabb_min.z, aabb_max.z);
    return dist_sq <= sphere_radius * sphere_radius;
}

// ---------------------------------------------------------------------------
// 圆锥与 AABB 相交测试（简化版）
// 检测聚光灯锥体是否与 AABB 相交
// ---------------------------------------------------------------------------
static bool cone_aabb_intersect(
    const math::Vector3f& cone_tip,           // 锥体顶点（光源位置）
    const math::Vector3f& cone_dir,           // 锥体方向（归一化）
    float cone_angle,                         // 半锥角（弧度）
    float cone_range,                         // 锥体长度
    const math::Vector3f& aabb_min,
    const math::Vector3f& aabb_max)
{
    // 第一步：先检测球体（光源范围）是否与 AABB 相交
    if (!sphere_aabb_intersect(cone_tip, cone_range, aabb_min, aabb_max)) {
        return false;
    }

    // 第二步：检测 AABB 是否在锥体内
    // 计算 AABB 中心到锥体轴线的距离
    math::Vector3f aabb_center = (aabb_min + aabb_max) * 0.5f;
    math::Vector3f to_center = aabb_center - cone_tip;
    float dist_along_axis = to_center.dot(cone_dir);

    // 如果 AABB 在锥体反向，不相交
    if (dist_along_axis < 0.0f) return false;
    // 如果 AABB 超出锥体范围，不相交
    if (dist_along_axis > cone_range) return false;

    // 计算 AABB 中心到锥体轴线的垂直距离
    math::Vector3f proj = cone_tip + cone_dir * dist_along_axis;
    float perp_dist = (aabb_center - proj).length();

    // 在该距离处，锥体的半径
    float cone_radius_at_dist = dist_along_axis * std::tan(cone_angle);

    // AABB 包围球半径
    math::Vector3f aabb_extent = (aabb_max - aabb_min) * 0.5f;
    float aabb_sphere_radius = aabb_extent.length();

    return perp_dist <= cone_radius_at_dist + aabb_sphere_radius;
}

bool ClusterBuilderRD::_light_intersects_cluster(
    const LightData& light, int tile_x, int tile_y, int slice_z,
    const math::Camera& camera) const
{
    // 计算集群在观察空间的 AABB
    math::Vector3f cluster_min, cluster_max;

    // 获取逆投影矩阵（从观察空间到 NDC）
    math::Matrix4f inv_proj = camera.get_projection_matrix().inverse();

    float z_near = 0.1f;
    float z_far = 1000.0f;

    compute_cluster_aabb(tile_x, tile_y, slice_z,
                         tile_count_x_, tile_count_y_, cluster_z_layers_,
                         z_near, z_far, inv_proj,
                         cluster_min, cluster_max);

    // 将光源位置变换到观察空间
    math::Matrix4f view = camera.get_view_matrix();
    math::Vector4f light_pos_view = view * math::Vector4f(light.position.x, light.position.y, light.position.z, 1.0f);
    math::Vector3f light_pos_vs(light_pos_view.x / light_pos_view.w,
                                 light_pos_view.y / light_pos_view.w,
                                 light_pos_view.z / light_pos_view.w);

    // 点光源：球体与 AABB 相交测试
    if (light.type == LightType::Point) {
        return sphere_aabb_intersect(light_pos_vs, light.range, cluster_min, cluster_max);
    }

    // 聚光灯：圆锥与 AABB 相交测试
    if (light.type == LightType::Spot) {
        // 变换方向到观察空间
        math::Vector4f light_dir_view = view * math::Vector4f(light.direction.x, light.direction.y, light.direction.z, 0.0f);
        math::Vector3f dir_vs(light_dir_view.x, light_dir_view.y, light_dir_view.z);
        dir_vs = dir_vs.normalized();

        float half_angle = math::to_radians(light.spot_angle * 0.5f);
        return cone_aabb_intersect(light_pos_vs, dir_vs, half_angle, light.range, cluster_min, cluster_max);
    }

    // 方向光：影响所有集群
    if (light.type == LightType::Directional) {
        return true;
    }

    return false;
}

void ClusterBuilderRD::build(const math::Camera& camera,
                              const std::vector<LightData>& lights,
                              int screen_width, int screen_height,
                              float z_near, float z_far)
{
    // 计算集群维度
    tile_count_x_ = _compute_tile_count(screen_width, k_tile_size_x);
    tile_count_y_ = _compute_tile_count(screen_height, k_tile_size_y);
    cluster_z_layers_ = k_max_cluster_z_layers;
    total_clusters_ = tile_count_x_ * tile_count_y_ * cluster_z_layers_;

    // 重置缓冲区
    cluster_buffer_.assign(total_clusters_, {0, 0});
    culled_lights_.clear();

    // 分离方向光和非方向光
    // culled_lights_ 顺序：[所有方向光] + [所有非方向光]
    std::vector<size_t> directional_indices;
    std::vector<size_t> non_directional_indices;
    for (size_t i = 0; i < lights.size(); ++i) {
        if (lights[i].type == LightType::Directional) {
            directional_indices.push_back(i);
        } else {
            non_directional_indices.push_back(i);
        }
    }

    // 将方向光添加到 culled_lights_（先）
    for (size_t idx : directional_indices) {
        culled_lights_.push_back(lights[idx]);
    }

    // 将非方向光添加到 culled_lights_（后）
    for (size_t idx : non_directional_indices) {
        culled_lights_.push_back(lights[idx]);
    }

    const uint32_t num_directional = static_cast<uint32_t>(directional_indices.size());
    const uint32_t num_non_directional = static_cast<uint32_t>(non_directional_indices.size());

    // 如果没有光源，直接返回
    if (lights.empty()) return;

    // 计算每个集群的光源数量（第一遍）
    std::vector<uint32_t> cluster_light_counts(total_clusters_, 0);

    // 方向光影响所有集群
    if (num_directional > 0) {
        for (int c = 0; c < total_clusters_; ++c) {
            cluster_light_counts[c] = num_directional;
        }
    }

    // 非方向光：逐个测试与集群的相交
    for (uint32_t ni = 0; ni < num_non_directional; ++ni) {
        const auto& light = lights[non_directional_indices[ni]];

        for (int tz = 0; tz < cluster_z_layers_; ++tz) {
            for (int ty = 0; ty < tile_count_y_; ++ty) {
                for (int tx = 0; tx < tile_count_x_; ++tx) {
                    if (_light_intersects_cluster(light, tx, ty, tz, camera)) {
                        int ci = cluster_index(tx, ty, tz);
                        cluster_light_counts[ci]++;
                    }
                }
            }
        }
    }

    // 计算偏移并分配空间
    uint32_t total_indices = 0;
    for (int c = 0; c < total_clusters_; ++c) {
        cluster_buffer_[c].offset = total_indices;
        cluster_buffer_[c].count = 0;
        total_indices += cluster_light_counts[c];
    }
    light_index_scratch_.resize(total_indices, 0);

    // 第二遍：填充光源索引
    // 方向光索引范围: [0, num_directional)
    // 非方向光索引范围: [num_directional, num_directional + num_non_directional)
    std::vector<uint32_t> cluster_write_positions(total_clusters_, 0);

    // 方向光：写入所有集群
    for (uint32_t di = 0; di < num_directional; ++di) {
        for (int c = 0; c < total_clusters_; ++c) {
            uint32_t write_pos = cluster_buffer_[c].offset + cluster_write_positions[c];
            light_index_scratch_[write_pos] = di;  // 方向光在 culled_lights_ 中的索引
            cluster_write_positions[c]++;
        }
    }

    // 非方向光：写入相交的集群
    for (uint32_t ni = 0; ni < num_non_directional; ++ni) {
        const auto& light = lights[non_directional_indices[ni]];
        uint32_t light_idx = num_directional + ni;  // 在 culled_lights_ 中的索引

        for (int tz = 0; tz < cluster_z_layers_; ++tz) {
            for (int ty = 0; ty < tile_count_y_; ++ty) {
                for (int tx = 0; tx < tile_count_x_; ++tx) {
                    if (_light_intersects_cluster(light, tx, ty, tz, camera)) {
                        int ci = cluster_index(tx, ty, tz);
                        uint32_t write_pos = cluster_buffer_[ci].offset + cluster_write_positions[ci];
                        light_index_scratch_[write_pos] = light_idx;
                        cluster_write_positions[ci]++;
                    }
                }
            }
        }
    }

    // 更新 count
    for (int c = 0; c < total_clusters_; ++c) {
        cluster_buffer_[c].count = cluster_write_positions[c];
    }

    // 移动 scratch 到主缓冲区
    light_index_buffer_ = std::move(light_index_scratch_);
}

} // namespace gryce_engine::render