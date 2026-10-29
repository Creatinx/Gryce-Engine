#pragma once

#include <cstdint>
#include <vector>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// 顶点属性描述
// ---------------------------------------------------------------------------
enum class VertexType {
    Float,
    Float2,
    Float3,
    Float4,
    Int,
    UInt,
    Int4,   // 4×int32（整数属性，如 bone ids）
    UInt4   // 4×uint32（整数属性，如 bone ids）
};

// 整数类型属性必须用 glVertexAttribIPointer / VK_FORMAT_*_UINT 路径，
// 不能走 float 属性（否则 shader 中 ivec/uvec 读到未转换的位模式）。
inline bool is_integer_vertex_type(VertexType type) noexcept {
    return type == VertexType::Int || type == VertexType::UInt ||
           type == VertexType::Int4 || type == VertexType::UInt4;
}

struct VertexAttribute {
    uint32_t location = 0;
    VertexType type = VertexType::Float3;
    bool normalized = false;
    uint32_t offset = 0;

    bool operator==(const VertexAttribute& other) const noexcept {
        return location == other.location && type == other.type &&
               normalized == other.normalized && offset == other.offset;
    }
    bool operator!=(const VertexAttribute& other) const noexcept {
        return !(*this == other);
    }
};

struct VertexLayout {
    uint32_t stride = 0;
    std::vector<VertexAttribute> attributes;

    bool operator==(const VertexLayout& other) const noexcept {
        return stride == other.stride && attributes == other.attributes;
    }
    bool operator!=(const VertexLayout& other) const noexcept {
        return !(*this == other);
    }
};

// ---------------------------------------------------------------------------
// IMesh — 跨 API 网格接口
// ---------------------------------------------------------------------------
class IMesh {
public:
    virtual ~IMesh() = default;

    virtual void upload_vertices(const void* data, uint32_t size, uint32_t count) = 0;
    virtual void upload_indices(const void* data, uint32_t size, uint32_t count) = 0;
    virtual void set_layout(const VertexLayout& layout) = 0;

    // 实例流（可选）：与顶点流并列的第二条输入流，inputRate = instance。
    // 几何（顶点流）只描述一份原型，每实例的数据由这条流按实例下标推进。
    // 不使用实例化的 mesh 无需实现——默认空实现，instance_count() 返回 0 时
    // 绘制按普通（非实例化）路径走。
    virtual void set_instance_layout(const VertexLayout& /*layout*/) {}
    virtual void upload_instances(const void* /*data*/, uint32_t /*size*/, uint32_t /*count*/) {}
    virtual uint32_t instance_count() const { return 0; }

    virtual void bind() const = 0;
    virtual void draw() const = 0;
    virtual void draw_indexed() const = 0;

    // 间接绘制（可选能力）：绘制参数（indexCount / instanceCount / firstIndex /
    // baseVertex / baseInstance）由 GPU 缓冲提供，CPU 不参与逐物体提交。
    // offset 为该组参数在缓冲中的字节偏移；调用前后端已把参数缓冲绑到
    // GL_DRAW_INDIRECT_BUFFER（OpenGL）/ indirect buffer（Vulkan）。
    // 默认空实现，不支持的后端不受影响（意味着该 mesh 在 GPU-driven 路径下不绘制）。
    virtual void draw_indexed_indirect(uint32_t /*offset*/) const {}
    // 非索引版（DrawArraysIndirect）：去索引展开的实例化几何走这条。
    virtual void draw_arrays_indirect(uint32_t /*offset*/) const {}

    virtual uint32_t vertex_count() const = 0;
    virtual uint32_t index_count() const = 0;

    // 光追几何导出（可选能力）：后端按需实现，默认 false（不参与光追的后端直接
    // 返回 false）。成功后 out_positions 指向紧凑 xyz 数组（长度 3*vertex_count），
    // out_indices 指向 uint32 三角形索引（长度 3*triangle_count）。引擎默认在
    // 设备支持光追时才保留 CPU 几何，故非 RT 后端/设备不会付出内存成本。
    virtual bool get_ray_tracing_geometry(const float*& out_positions, uint32_t& out_vertex_count,
                                          const uint32_t*& out_indices,
                                          uint32_t& out_index_count) const {
        out_positions = nullptr;
        out_vertex_count = 0;
        out_indices = nullptr;
        out_index_count = 0;
        return false;
    }
};

} // namespace gryce_engine::render
