#pragma once

#include <cstdint>
#include <cstddef>

#include <GL/glew.h>

#include "render/mesh.h"
#include "render/buffer.h"

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

// ---------------------------------------------------------------------------
// GLStorageBuffer — OpenGL SSBO 实现
// 用于存储光照数据、骨骼矩阵等需要 shader 随机访问的数据。
// 绑定到 GL_SHADER_STORAGE_BUFFER target。
// ---------------------------------------------------------------------------
class GLStorageBuffer : public IBuffer {
public:
    GLStorageBuffer();
    ~GLStorageBuffer() override;

    bool create(size_t size, const void* data, BufferUsage usage) override;
    void update(const void* data, size_t size, size_t offset = 0) override;
    void bind(uint32_t binding_point) const override;
    void destroy() override;

    size_t size() const override { return size_; }
    bool is_valid() const override { return ssbo_ != 0; }

    uint32_t buffer_id() const { return ssbo_; }

private:
    uint32_t ssbo_ = 0;
    size_t size_ = 0;
    GLenum gl_usage_ = GL_DYNAMIC_DRAW;
};

} // namespace gryce_engine::render