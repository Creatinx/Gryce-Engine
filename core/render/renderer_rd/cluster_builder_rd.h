#pragma once

#include <cstdint>
#include <vector>

#include "render/export.h"
#include "render/rhi_handle.h"
#include "math/math.h"
#include "math/camera.h"

namespace gryce_engine::render {

struct LightData;
class RenderContext;

// ---------------------------------------------------------------------------
// Cluster — 单个集群（tile × slice）
// 存储影响该集群的光源索引范围。
// ---------------------------------------------------------------------------
struct GRYCE_RENDERER_API Cluster {
    uint32_t offset = 0;      // 在全局光源索引缓冲区中的偏移
    uint32_t count = 0;       // 影响本集群的光源数量
};

// ---------------------------------------------------------------------------
// ClusterBuilderRD — 集群光剔除
// 类似于 Godot 的 ClusterBuilderRD。
// 将视锥体分割为 3D 网格（tile × slice），每个集群计算影响它的光源列表。
// 支持 CPU 回退（当 GPU 不支持 compute shader 时）。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API ClusterBuilderRD {
public:
    // 默认集群参数（与 Godot 一致）
    static constexpr int k_tile_size_x = 32;
    static constexpr int k_tile_size_y = 32;
    static constexpr int k_max_cluster_z_layers = 24;
    static constexpr int k_max_elements = 2048;  // 每集群最大光源数

    ClusterBuilderRD();
    ~ClusterBuilderRD();

    // 初始化集群资源
    bool init(RenderContext* ctx);
    void shutdown();

    // 每帧重建集群
    // 接收光源列表、相机参数、视口尺寸，计算每个集群的光源影响
    void build(const math::Camera& camera,
               const std::vector<LightData>& lights,
               int screen_width, int screen_height,
               float z_near, float z_far);

    // 获取集群数据
    int tile_count_x() const { return tile_count_x_; }
    int tile_count_y() const { return tile_count_y_; }
    int cluster_z_layers() const { return cluster_z_layers_; }

    // 获取集群索引缓冲区（供 shader 使用）
    const uint32_t* light_index_buffer() const { return light_index_buffer_.data(); }
    size_t light_index_buffer_size() const { return light_index_buffer_.size() * sizeof(uint32_t); }

    // 获取集群描述缓冲区（供 shader 使用）
    const Cluster* cluster_buffer() const { return cluster_buffer_.data(); }
    size_t cluster_buffer_size() const { return cluster_buffer_.size() * sizeof(Cluster); }

    // 获取已剔除的光源列表（用于 shader uniform 设置）
    const std::vector<LightData>& culled_lights() const { return culled_lights_; }

    // 调试：获取指定像素位置的集群索引
    int cluster_index(int screen_x, int screen_y, float depth) const;
    int cluster_index(int tile_x, int tile_y, int slice_z) const;

private:
    // 计算 tile 数量
    int _compute_tile_count(int pixels, int tile_size) const;
    // 计算 depth slice
    int _compute_depth_slice(float depth, float z_near, float z_far) const;
    // 光源与集群的相交测试
    bool _light_intersects_cluster(const LightData& light, int tile_x, int tile_y, int slice_z,
                                   const math::Camera& camera) const;

    RenderContext* ctx_ = nullptr;
    bool initialized_ = false;

    // 集群维度
    int tile_count_x_ = 0;
    int tile_count_y_ = 0;
    int cluster_z_layers_ = k_max_cluster_z_layers;
    int total_clusters_ = 0;

    // 光源索引缓冲区（全局，所有集群的光源索引连续排列）
    std::vector<uint32_t> light_index_buffer_;
    std::vector<uint32_t> light_index_scratch_;

    // 集群描述缓冲区（每个集群一个 entry，记录 offset + count）
    std::vector<Cluster> cluster_buffer_;

    // 已剔除的光源
    std::vector<LightData> culled_lights_;
};

} // namespace gryce_engine::render