#include "gl_framebuffer.h"

#include "gl_utils.h"
#include "gl_texture.h"
#include <GL/glew.h>

#include <GLFW/glfw3.h>

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

GLFramebuffer::GLFramebuffer() : fbo_id_(0), width_(0), height_(0) {}

GLFramebuffer::~GLFramebuffer() {
    if (fbo_id_) {
        glDeleteFramebuffers(1, &fbo_id_);
    }
}

bool GLFramebuffer::create(int width, int height) {
    if (fbo_id_) {
        glDeleteFramebuffers(1, &fbo_id_);
    }
    width_ = width;
    height_ = height;
    if (gl_dsa_available()) {
        glCreateFramebuffers(1, &fbo_id_);
    } else {
        glGenFramebuffers(1, &fbo_id_);
    }
    return fbo_id_ != 0;
}

void GLFramebuffer::destroy() {
    if (fbo_id_) {
        glDeleteFramebuffers(1, &fbo_id_);
        fbo_id_ = 0;
    }
}

void GLFramebuffer::bind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_id_);
}

void GLFramebuffer::unbind() const {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLFramebuffer::resize(int width, int height) {
    width_ = width;
    height_ = height;
}

void GLFramebuffer::attach_color_texture(ITexture* texture) {
    if (!texture) return;
    auto* gl_tex = static_cast<GLTexture*>(texture);
    // 多样本附件必须用 GL_TEXTURE_2D_MULTISAMPLE 目标挂载（GL_TEXTURE_2D
    // 会被判不完整 → 整个 FBO 不可用）。
    const GLenum target = gl_tex->is_multisampled() ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
    color_samples_ = gl_tex->sample_count();
    if (gl_dsa_available()) {
        glNamedFramebufferTexture(fbo_id_, GL_COLOR_ATTACHMENT0, gl_tex->texture_id(), 0);
    } else {
        bind();
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, gl_tex->texture_id(), 0);
        unbind();
    }
}

void GLFramebuffer::attach_depth_texture(ITexture* texture) {
    if (!texture) return;
    auto* gl_tex = static_cast<GLTexture*>(texture);
    const GLenum target = gl_tex->is_multisampled() ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
    depth_samples_ = gl_tex->sample_count();
    if (gl_dsa_available()) {
        glNamedFramebufferTexture(fbo_id_, GL_DEPTH_ATTACHMENT, gl_tex->texture_id(), 0);
    } else {
        bind();
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, target, gl_tex->texture_id(), 0);
        unbind();
    }
}

int GLFramebuffer::color_sample_count() const {
    return color_samples_;
}

bool GLFramebuffer::resolve_color_to(IFramebuffer* dst) {
    if (!dst) return false;
    auto* gl_dst = static_cast<GLFramebuffer*>(dst);
    if (fbo_id_ == 0 || gl_dst->fbo_id_ == 0) return false;
    // glBlitFramebuffer 的 READ/DRAW 是分别绑定的两个 binding point，
    // 与 bind() 用的 GL_FRAMEBUFFER（同时覆盖两者）不同，这里显式操作。
    GLint prev_read = 0, prev_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_id_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl_dst->fbo_id_);
    // NEAREST：MSAA resolve 就是逐样本平均（深度取单样本值），不做插值过滤。
    // 深度附件：仅当本 FBO 挂了多样本深度时才需要解析（GL 的 MSAA 目标
    // 颜色/深度同采样数；dst 的 hdr_depth_ 是单样本，正好接收）。
    // 无深度附件时带 GL_DEPTH_BUFFER_BIT 会报 GL_INVALID_OPERATION。
    GLbitfield mask = GL_COLOR_BUFFER_BIT;
    if (has_depth() && depth_samples_ > 1) mask |= GL_DEPTH_BUFFER_BIT;
    glBlitFramebuffer(0, 0, width_, height_, 0, 0, width_, height_, mask, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prev_read));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prev_draw));
    return true;
}

bool GLFramebuffer::is_complete() const {
    if (gl_dsa_available()) {
        const GLenum status = glCheckNamedFramebufferStatus(fbo_id_, GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            GLOG_ERROR("GLFramebuffer: incomplete (status=0x{:04x}, color_samples={})",
                       static_cast<unsigned>(status), color_samples_);
        }
        return status == GL_FRAMEBUFFER_COMPLETE;
    }
    bind();
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    unbind();
    return ok;
}

} // namespace gryce_engine::render
