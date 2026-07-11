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
    if (gl_dsa_available()) {
        glNamedFramebufferTexture(fbo_id_, GL_COLOR_ATTACHMENT0, gl_tex->texture_id(), 0);
    } else {
        bind();
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gl_tex->texture_id(), 0);
        unbind();
    }
}

void GLFramebuffer::attach_depth_texture(ITexture* texture) {
    if (!texture) return;
    auto* gl_tex = static_cast<GLTexture*>(texture);
    if (gl_dsa_available()) {
        glNamedFramebufferTexture(fbo_id_, GL_DEPTH_ATTACHMENT, gl_tex->texture_id(), 0);
    } else {
        bind();
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, gl_tex->texture_id(), 0);
        unbind();
    }
}

bool GLFramebuffer::is_complete() const {
    if (gl_dsa_available()) {
        return glCheckNamedFramebufferStatus(fbo_id_, GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    bind();
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    unbind();
    return ok;
}

} // namespace gryce_engine::render
