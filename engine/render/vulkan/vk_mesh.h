#pragma once

#include "render/mesh.h"
#include "vk_buffer.h"

namespace gryce_engine::render {

class VulkanDevice;

// ---------------------------------------------------------------------------
// VulkanMesh — 顶点/索引缓冲 + 绘制
// ---------------------------------------------------------------------------
class VulkanMesh : public IMesh {
public:
    VulkanMesh() = default;
    explicit VulkanMesh(VulkanDevice* device);
    ~VulkanMesh() override;

    void upload_vertices(const void* data, uint32_t size, uint32_t count) override;
    void upload_indices(const void* data, uint32_t size, uint32_t count) override;
    void set_layout(const VertexLayout& layout) override;
    void set_instance_layout(const VertexLayout& layout) override;
    void upload_instances(const void* data, uint32_t size, uint32_t count) override;

    void bind() const override;
    void draw() const override;
    void draw_indexed() const override;

    uint32_t vertex_count() const override { return vertex_count_; }
    uint32_t index_count() const override { return index_count_; }
    uint32_t instance_count() const override { return instance_count_; }

    bool get_ray_tracing_geometry(const float*& out_positions, uint32_t& out_vertex_count,
                                  const uint32_t*& out_indices,
                                  uint32_t& out_index_count) const override;

    VkBuffer vertex_buffer() const { return vertex_buffer_.buffer(); }
    VkBuffer index_buffer() const { return index_buffer_.buffer(); }
    VkBuffer instance_buffer() const { return instance_buffer_.buffer(); }
    const VertexLayout& layout() const { return layout_; }
    bool has_index() const { return has_index_; }

private:
    // 仅在设备支持光追时保留 CPU 几何（紧凑 xyz 位置 + uint32 索引），供 BLAS 构建。
    // 提取是懒执行的：upload_vertices/indices 仅存原始字节 + 位置属性信息，
    // 首次 get_ray_tracing_geometry 时解出紧凑数组。
    void capture_position_info();
    void ensure_ray_tracing_geometry() const;

    VulkanDevice* device_ = nullptr;
    VulkanBuffer vertex_buffer_;
    VulkanBuffer index_buffer_;
    VulkanBuffer instance_buffer_;
    uint32_t vertex_count_ = 0;
    uint32_t index_count_ = 0;
    uint32_t instance_count_ = 0;
    VertexLayout layout_;
    VertexLayout instance_layout_;
    bool has_index_ = false;

    // 光追几何缓存（仅 RT 设备分配；mutable 因懒提取发生在 const 虚方法内）。
    mutable std::vector<uint8_t> rt_vertex_bytes_;
    mutable std::vector<uint8_t> rt_index_bytes_;
    mutable std::vector<float> rt_positions_;
    mutable std::vector<uint32_t> rt_indices_;
    mutable bool rt_valid_ = false;
    uint32_t rt_pos_offset_ = 0;
    uint32_t rt_stride_ = 0;
};

} // namespace gryce_engine::render
