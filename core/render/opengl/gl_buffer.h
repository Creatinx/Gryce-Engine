#pragma once

#include <cstdint>

#include "render/mesh.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// GLMesh — OpenGL Buffer + VAO 实现
// ---------------------------------------------------------------------------
class GLMesh : public IMesh {
public:
    GLMesh();
    ~GLMesh() override;

    void upload_vertices(const void* data, uint32_t size, uint32_t count) override;
    void upload_indices(const void* data, uint32_t size, uint32_t count) override;
    void set_layout(const VertexLayout& layout) override;

    void bind() const override;
    void draw() const override;
    void draw_indexed() const override;

    uint32_t vertex_count() const override { return vertex_count_; }
    uint32_t index_count() const override { return index_count_; }

private:
    uint32_t vao_ = 0;
    uint32_t vbo_ = 0;
    uint32_t ebo_ = 0;
    uint32_t vertex_count_ = 0;
    uint32_t index_count_ = 0;
    uint32_t vertex_buffer_size_ = 0;
    uint32_t index_buffer_size_ = 0;
    VertexLayout layout_;
    bool has_index_ = false;

    uint32_t get_gl_type(VertexType type) const;
    int get_component_count(VertexType type) const;
};

} // namespace gryce_engine::render