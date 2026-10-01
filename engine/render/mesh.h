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

    virtual uint32_t vertex_count() const = 0;
    virtual uint32_t index_count() const = 0;
};

} // namespace gryce_engine::render
