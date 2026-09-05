#include "vk_mesh.h"

#include "vk_device.h"
#include "utils/glog/glog_lib.h"

#include <cstring>

namespace gryce_engine::render {

VulkanMesh::VulkanMesh(VulkanDevice* device) : device_(device) {}

VulkanMesh::~VulkanMesh() = default;

void VulkanMesh::upload_vertices(const void* data, uint32_t size, uint32_t count) {
    vertex_count_ = count;
    if (!vertex_buffer_.buffer() || vertex_buffer_.size() < size) {
        vertex_buffer_.shutdown();
        if (!vertex_buffer_.init(device_, size,
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            GLOG_ERROR("VulkanMesh: failed to create vertex buffer");
            return;
        }
    }
    vertex_buffer_.upload(data, size);
}

void VulkanMesh::upload_indices(const void* data, uint32_t size, uint32_t count) {
    index_count_ = count;
    has_index_ = true;
    if (!index_buffer_.buffer() || index_buffer_.size() < size) {
        index_buffer_.shutdown();
        if (!index_buffer_.init(device_, size,
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            GLOG_ERROR("VulkanMesh: failed to create index buffer");
            return;
        }
    }
    index_buffer_.upload(data, size);
}

void VulkanMesh::set_layout(const VertexLayout& layout) {
    layout_ = layout;
}

void VulkanMesh::bind() const {
    // Vulkan 绑定在 command buffer 记录时进行
}

void VulkanMesh::draw() const {
    // 由 backend 根据 has_index_ 决定 draw 方式
    (void)device_;
}

void VulkanMesh::draw_indexed() const {
    (void)device_;
}

} // namespace gryce_engine::render
