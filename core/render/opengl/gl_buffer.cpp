#include "gl_buffer.h"

#include "gl_utils.h"
#include <GL/glew.h>

#include <GLFW/glfw3.h>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

namespace {

// 当前绑定的 VAO，避免重复的 glBindVertexArray 调用。
thread_local GLuint g_current_bound_vao = 0;

} // namespace

GLMesh::GLMesh() {
    if (gl_dsa_available()) {
        glCreateVertexArrays(1, &vao_);
        glCreateBuffers(1, &vbo_);
        glCreateBuffers(1, &ebo_);
    } else {
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &ebo_);
    }
}

GLMesh::~GLMesh() {
    glDeleteVertexArrays(1, &vao_);
    glDeleteBuffers(1, &vbo_);
    glDeleteBuffers(1, &ebo_);
}

void GLMesh::upload_vertices(const void* data, uint32_t size, uint32_t count) {
    vertex_count_ = count;
    if (size == 0) return;

    if (gl_dsa_available()) {
        if (size <= vertex_buffer_size_) {
            glNamedBufferSubData(vbo_, 0, static_cast<GLsizeiptr>(size), data);
        } else {
            glNamedBufferData(vbo_, static_cast<GLsizeiptr>(size), data, GL_DYNAMIC_DRAW);
            vertex_buffer_size_ = size;
        }
    } else {
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        if (size <= vertex_buffer_size_) {
            glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(size), data);
        } else {
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(size), data, GL_DYNAMIC_DRAW);
            vertex_buffer_size_ = size;
        }
        glBindVertexArray(0);
    }
    GL_CHECK_ERROR();
}

void GLMesh::upload_indices(const void* data, uint32_t size, uint32_t count) {
    index_count_ = count;
    has_index_ = true;
    if (size == 0) return;

    if (gl_dsa_available()) {
        if (size <= index_buffer_size_) {
            glNamedBufferSubData(ebo_, 0, static_cast<GLsizeiptr>(size), data);
        } else {
            glNamedBufferData(ebo_, static_cast<GLsizeiptr>(size), data, GL_STATIC_DRAW);
            index_buffer_size_ = size;
        }
    } else {
        glBindVertexArray(vao_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        if (size <= index_buffer_size_) {
            glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(size), data);
        } else {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(size), data, GL_STATIC_DRAW);
            index_buffer_size_ = size;
        }
        glBindVertexArray(0);
    }
    GL_CHECK_ERROR();
}

void GLMesh::set_layout(const VertexLayout& layout) {
    if (layout_ == layout) {
        return;
    }

    if (gl_dsa_available()) {
        // 先禁用所有 vertex attrib array，避免上一轮布局的残留状态影响当前布局
        for (GLuint loc = 0; loc < 16; ++loc) {
            glDisableVertexArrayAttrib(vao_, loc);
        }

        layout_ = layout;
        glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, static_cast<GLsizei>(layout_.stride));
        glVertexArrayElementBuffer(vao_, ebo_);

        for (const auto& attr : layout_.attributes) {
            glEnableVertexArrayAttrib(vao_, attr.location);
            glVertexArrayAttribFormat(
                vao_,
                attr.location,
                get_component_count(attr.type),
                get_gl_type(attr.type),
                attr.normalized ? GL_TRUE : GL_FALSE,
                static_cast<GLuint>(attr.offset)
            );
            glVertexArrayAttribBinding(vao_, attr.location, 0);
        }
    } else {
        glBindVertexArray(vao_);

        for (const auto& attr : layout_.attributes) {
            glDisableVertexAttribArray(attr.location);
        }

        layout_ = layout;
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);

        for (const auto& attr : layout_.attributes) {
            glEnableVertexAttribArray(attr.location);
            glVertexAttribPointer(
                attr.location,
                get_component_count(attr.type),
                get_gl_type(attr.type),
                attr.normalized ? GL_TRUE : GL_FALSE,
                static_cast<GLsizei>(layout_.stride),
                reinterpret_cast<const void*>(static_cast<uintptr_t>(attr.offset))
            );
        }

        glBindVertexArray(0);
    }
    GL_CHECK_ERROR();
}

void GLMesh::bind() const {
    if (vao_ != g_current_bound_vao) {
        glBindVertexArray(vao_);
        g_current_bound_vao = vao_;
    }
}

void GLMesh::draw() const {
    bind();
    if (has_index_ && index_count_ > 0) {
        glDrawElements(GL_TRIANGLES, static_cast<int>(index_count_), GL_UNSIGNED_INT, nullptr);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, static_cast<int>(vertex_count_));
    }
}

void GLMesh::draw_indexed() const {
    bind();
    glDrawElements(GL_TRIANGLES, static_cast<int>(index_count_), GL_UNSIGNED_INT, nullptr);
}

uint32_t GLMesh::get_gl_type(VertexType type) const {
    switch (type) {
        case VertexType::Float:
        case VertexType::Float2:
        case VertexType::Float3:
        case VertexType::Float4:
            return GL_FLOAT;
        case VertexType::Int:
            return GL_INT;
        case VertexType::UInt:
            return GL_UNSIGNED_INT;
    }
    return GL_FLOAT;
}

int GLMesh::get_component_count(VertexType type) const {
    switch (type) {
        case VertexType::Float:  return 1;
        case VertexType::Float2: return 2;
        case VertexType::Float3: return 3;
        case VertexType::Float4: return 4;
        case VertexType::Int:    return 1;
        case VertexType::UInt:   return 1;
    }
    return 3;
}

} // namespace gryce_engine::render
