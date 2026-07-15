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
    UInt
};

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

    virtual void bind() const = 0;
    virtual void draw() const = 0;
    virtual void draw_indexed() const = 0;

    virtual uint32_t vertex_count() const = 0;
    virtual uint32_t index_count() const = 0;
};

} // namespace gryce_engine::render
