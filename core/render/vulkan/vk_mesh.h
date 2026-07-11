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

    void bind() const override;
    void draw() const override;
    void draw_indexed() const override;

    uint32_t vertex_count() const override { return vertex_count_; }
    uint32_t index_count() const override { return index_count_; }

    VkBuffer vertex_buffer() const { return vertex_buffer_.buffer(); }
    VkBuffer index_buffer() const { return index_buffer_.buffer(); }
    const VertexLayout& layout() const { return layout_; }
    bool has_index() const { return has_index_; }

private:
    VulkanDevice* device_ = nullptr;
    VulkanBuffer vertex_buffer_;
    VulkanBuffer index_buffer_;
    uint32_t vertex_count_ = 0;
    uint32_t index_count_ = 0;
    VertexLayout layout_;
    bool has_index_ = false;
};

} // namespace gryce_engine::render
