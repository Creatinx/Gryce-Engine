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

    // 仅在设备支持光追时保留 CPU 顶点字节，供 BLAS 构建（非 RT 设备零成本）。
    if (device_ && device_->supports_ray_tracing()) {
        rt_vertex_bytes_.assign(static_cast<const uint8_t*>(data),
                               static_cast<const uint8_t*>(data) + size);
        capture_position_info();
    }
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

    if (device_ && device_->supports_ray_tracing()) {
        rt_index_bytes_.assign(static_cast<const uint8_t*>(data),
                               static_cast<const uint8_t*>(data) + size);
    }
}

void VulkanMesh::set_layout(const VertexLayout& layout) {
    layout_ = layout;
    if (device_ && device_->supports_ray_tracing()) {
        capture_position_info();
    }
}

void VulkanMesh::capture_position_info() {
    rt_stride_ = 0;
    rt_pos_offset_ = 0;
    if (layout_.attributes.empty()) return;
    // 位置属性约定：优先选 offset==0 的 Float3；否则第一个 Float3/Float4；否则 attributes[0]。
    int chosen = -1;
    for (size_t i = 0; i < layout_.attributes.size(); ++i) {
        if (layout_.attributes[i].type == VertexType::Float3 &&
            layout_.attributes[i].offset == 0) {
            chosen = static_cast<int>(i);
            break;
        }
    }
    if (chosen < 0) {
        for (size_t i = 0; i < layout_.attributes.size(); ++i) {
            if (layout_.attributes[i].type == VertexType::Float3 ||
                layout_.attributes[i].type == VertexType::Float4) {
                chosen = static_cast<int>(i);
                break;
            }
        }
    }
    if (chosen < 0) chosen = 0;
    rt_pos_offset_ = layout_.attributes[static_cast<size_t>(chosen)].offset;
    rt_stride_ = layout_.stride;
}

void VulkanMesh::ensure_ray_tracing_geometry() const {
    if (rt_valid_) return;
    rt_valid_ = true; // 失败也置位，避免每帧重试
    if (rt_vertex_bytes_.empty() || rt_index_bytes_.empty() || rt_stride_ == 0 ||
        vertex_count_ == 0 || index_count_ == 0) {
        return;
    }
    rt_positions_.resize(static_cast<size_t>(vertex_count_) * 3);
    for (uint32_t v = 0; v < vertex_count_; ++v) {
        const float* p = reinterpret_cast<const float*>(
            rt_vertex_bytes_.data() + rt_pos_offset_ + static_cast<size_t>(v) * rt_stride_);
        rt_positions_[static_cast<size_t>(v) * 3 + 0] = p[0];
        rt_positions_[static_cast<size_t>(v) * 3 + 1] = p[1];
        rt_positions_[static_cast<size_t>(v) * 3 + 2] = p[2];
    }
    const uint32_t* src = reinterpret_cast<const uint32_t*>(rt_index_bytes_.data());
    rt_indices_.assign(src, src + index_count_);
}

bool VulkanMesh::get_ray_tracing_geometry(const float*& out_positions, uint32_t& out_vertex_count,
                                          const uint32_t*& out_indices,
                                          uint32_t& out_index_count) const {
    ensure_ray_tracing_geometry();
    if (rt_positions_.empty() || rt_indices_.empty()) {
        out_positions = nullptr;
        out_vertex_count = 0;
        out_indices = nullptr;
        out_index_count = 0;
        return false;
    }
    out_positions = rt_positions_.data();
    out_vertex_count = vertex_count_;
    out_indices = rt_indices_.data();
    out_index_count = index_count_;
    return true;
}

void VulkanMesh::set_instance_layout(const VertexLayout& layout) {
    instance_layout_ = layout;
}

void VulkanMesh::upload_instances(const void* data, uint32_t size, uint32_t count) {
    instance_count_ = count;
    if (size == 0) return;
    if (!instance_buffer_.buffer() || instance_buffer_.size() < size) {
        instance_buffer_.shutdown();
        if (!instance_buffer_.init(device_, size,
                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            GLOG_ERROR("VulkanMesh: failed to create instance buffer");
            return;
        }
    }
    instance_buffer_.upload(data, size);
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
