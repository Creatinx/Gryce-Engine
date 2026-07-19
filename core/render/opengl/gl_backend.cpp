#include "gl_backend.h"

#include <cstring>
#include <fstream>
#include <vector>

#include "gl_utils.h"
#include "gl_buffer.h"
#include "gl_shader.h"
#include "gl_texture.h"
#include "gl_framebuffer.h"
#include "gl_imgui_backend.h"
#include "render/renderer2d_impl.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

GLBackend::GLBackend() : window_(nullptr) {}

GLBackend::~GLBackend() {
    if (window_) {
        shutdown();
    }
}

bool GLBackend::init(void* native_window) {
    window_ = static_cast<GLFWwindow*>(native_window);
    if (!window_) {
        GLOG_ERROR("GLBackend::init: null window handle");
        return false;
    }

    make_current(native_window);

    GLenum err = glewInit();
    if (err != GLEW_OK) {
        GLOG_ERROR("glewInit failed: {}", reinterpret_cast<const char*>(glewGetErrorString(err)));
        return false;
    }

    // GLEW 在 core profile 下可能留下 GL_INVALID_ENUM，清除避免误导后续错误检查
    while (glGetError() != GL_NO_ERROR) {}

    // 初始化 NVIDIA 帧率控制扩展（必须在 glewInit 之后，context current）
    frame_pacing_.init(native_window);

    GL_CHECK_ERROR();

    gl_register_debug_callback();
    GL_CHECK_ERROR();

    GLOG_INFO("OpenGL backend initialized");
    GLOG_INFO("  Vendor:   {}", reinterpret_cast<const char*>(glGetString(GL_VENDOR)));
    GLOG_INFO("  Renderer: {}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    GLOG_INFO("  Version:  {}", reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    GL_CHECK_ERROR();

    return true;
}

void GLBackend::shutdown() {
    GLOG_INFO("OpenGL backend shutdown");
    window_ = nullptr;
}

void GLBackend::make_current(void* native_window) {
    if (native_window) {
        glfwMakeContextCurrent(static_cast<GLFWwindow*>(native_window));
    }
}

void GLBackend::release_context() {
    glfwMakeContextCurrent(nullptr);
}

void GLBackend::begin_frame() {
    // 帧开始钩子：不再自动 clear。
    // clear 应由应用层通过 RenderContext::clear() 显式提交，
    // 否则保留上一帧内容（标准 OpenGL 行为）。
    // 每帧重置状态缓存，防止外部或驱动状态变化导致缓存失效。
    state_cache_valid_ = false;
}

void GLBackend::flush_gpu() {
    // 及时把命令提交给驱动，保留 CPU/GPU 并行度。
    glFlush();
}

void GLBackend::wait_gpu_idle() {
    // 真正等待 GPU 完成所有已提交命令。
    // 仅在销毁资源或后端前调用，避免影响每帧性能。
    glFinish();
}

void GLBackend::end_frame() {
    if (window_) {
        if (!screenshot_path_.empty() && frame_count_ == screenshot_frame_) {
            save_screenshot(screenshot_path_);
            screenshot_path_.clear();
        }

        // NVIDIA 特定帧率控制 + GPU busy-spin
        frame_pacing_.before_swap();

        GLOG_DEBUG("GLBackend::end_frame: glfwSwapBuffers");
        glfwSwapBuffers(window_);
    }
    ++frame_count_;
}

void GLBackend::request_screenshot(const std::string& path) {
    screenshot_path_ = path;
    screenshot_frame_ = frame_count_ + 1;
}

namespace {

void save_bgr_bmp(const std::string& path, const unsigned char* bgr_data,
                  uint32_t width, uint32_t height) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        GLOG_ERROR("save_bgr_bmp: failed to open '{}'", path);
        return;
    }

    const uint32_t row_size = (width * 3 + 3) & ~3u;
    const uint32_t data_size = row_size * height;
    const uint32_t file_size = 54 + data_size;

    unsigned char header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    *reinterpret_cast<uint32_t*>(&header[2]) = file_size;
    *reinterpret_cast<uint32_t*>(&header[10]) = 54;
    *reinterpret_cast<uint32_t*>(&header[14]) = 40;
    *reinterpret_cast<uint32_t*>(&header[18]) = width;
    *reinterpret_cast<int32_t*>(&header[22]) = static_cast<int32_t>(height);
    *reinterpret_cast<uint16_t*>(&header[26]) = 1;
    *reinterpret_cast<uint16_t*>(&header[28]) = 24;
    *reinterpret_cast<uint32_t*>(&header[34]) = data_size;

    file.write(reinterpret_cast<char*>(header), 54);

    std::vector<unsigned char> row(row_size, 0);
    for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
        const unsigned char* src = bgr_data + static_cast<std::size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        file.write(reinterpret_cast<char*>(row.data()), row_size);
    }
}

} // namespace

void GLBackend::set_swap_interval(int interval) {
    if (window_) {
        glfwSwapInterval(interval);
        GLOG_INFO("GLBackend::set_swap_interval: {}", interval);
    }
}

void GLBackend::set_gpu_busy_spin(bool enabled, int iterations) {
    frame_pacing_.set_gpu_busy_enabled(enabled);
    frame_pacing_.set_gpu_busy_iterations(iterations);
    GLOG_INFO("GLBackend::set_gpu_busy_spin: enabled={}, iterations={}", enabled, iterations);
}

void GLBackend::set_nv_delay_before_swap(float seconds) {
    frame_pacing_.set_nv_delay_before_swap(seconds);
    GLOG_INFO("GLBackend::set_nv_delay_before_swap: {}s", seconds);
}

bool GLBackend::supports_nv_delay_before_swap() const {
    return frame_pacing_.supports_nv_delay_before_swap();
}

void GLBackend::clear(float r, float g, float b, float a) {
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void GLBackend::clear_depth() {
    glClear(GL_DEPTH_BUFFER_BIT);
}

void GLBackend::set_viewport(int x, int y, int w, int h) {
    if (state_cache_valid_ && viewport_x_ == x && viewport_y_ == y &&
        viewport_w_ == w && viewport_h_ == h) {
        return;
    }
    glViewport(x, y, w, h);
    viewport_x_ = x;
    viewport_y_ = y;
    viewport_w_ = w;
    viewport_h_ = h;
    state_cache_valid_ = true;
}

void GLBackend::set_scissor(int x, int y, int w, int h) {
    if (state_cache_valid_ && scissor_x_ == x && scissor_y_ == y &&
        scissor_w_ == w && scissor_h_ == h) {
        return;
    }
    glScissor(x, y, w, h);
    scissor_x_ = x;
    scissor_y_ = y;
    scissor_w_ = w;
    scissor_h_ = h;
    state_cache_valid_ = true;
}

void GLBackend::set_viewport(int x, int y, int w, int h, uint32_t viewport_index) {
    (void)viewport_index;
    set_viewport(x, y, w, h);
}

void GLBackend::set_scissor(int x, int y, int w, int h, uint32_t viewport_index) {
    (void)viewport_index;
    set_scissor(x, y, w, h);
}

void GLBackend::bind_framebuffer(RHIFramebufferHandle fb) {
    IFramebuffer* fbo = framebuffer(fb);
    if (fbo) {
        fbo->bind();
    } else {
        unbind_framebuffer();
    }
}

void GLBackend::unbind_framebuffer() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GLBackend::set_depth_test(bool enabled) {
    if (state_cache_valid_ && depth_test_enabled_ == enabled) {
        return;
    }
    if (enabled) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    depth_test_enabled_ = enabled;
    state_cache_valid_ = true;
}

void GLBackend::set_depth_write(bool enabled) {
    if (state_cache_valid_ && depth_write_enabled_ == enabled) {
        return;
    }
    glDepthMask(enabled ? GL_TRUE : GL_FALSE);
    depth_write_enabled_ = enabled;
    state_cache_valid_ = true;
}

namespace {

GLenum blend_factor_to_gl(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero: return GL_ZERO;
        case BlendFactor::One: return GL_ONE;
        case BlendFactor::SrcAlpha: return GL_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha: return GL_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
        case BlendFactor::SrcColor: return GL_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return GL_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColor: return GL_DST_COLOR;
        case BlendFactor::OneMinusDstColor: return GL_ONE_MINUS_DST_COLOR;
    }
    return GL_ONE;
}

GLenum blend_equation_to_gl(BlendEquation e) {
    switch (e) {
        case BlendEquation::Add: return GL_FUNC_ADD;
        case BlendEquation::Subtract: return GL_FUNC_SUBTRACT;
        case BlendEquation::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case BlendEquation::Min: return GL_MIN;
        case BlendEquation::Max: return GL_MAX;
    }
    return GL_FUNC_ADD;
}

} // namespace

void GLBackend::set_blend(bool enabled) {
    if (state_cache_valid_ && blend_enabled_ == enabled &&
        blend_src_ == BlendFactor::SrcAlpha && blend_dst_ == BlendFactor::OneMinusSrcAlpha &&
        blend_equation_ == BlendEquation::Add) {
        return;
    }
    if (enabled) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    blend_enabled_ = enabled;
    blend_src_ = BlendFactor::SrcAlpha;
    blend_dst_ = BlendFactor::OneMinusSrcAlpha;
    blend_equation_ = BlendEquation::Add;
    state_cache_valid_ = true;
}

void GLBackend::set_blend_func(BlendFactor src_factor, BlendFactor dst_factor) {
    if (state_cache_valid_ && blend_src_ == src_factor && blend_dst_ == dst_factor) {
        return;
    }
    glBlendFunc(blend_factor_to_gl(src_factor), blend_factor_to_gl(dst_factor));
    blend_src_ = src_factor;
    blend_dst_ = dst_factor;
    state_cache_valid_ = true;
}

void GLBackend::set_blend_equation(BlendEquation mode) {
    if (state_cache_valid_ && blend_equation_ == mode) {
        return;
    }
    glBlendEquation(blend_equation_to_gl(mode));
    blend_equation_ = mode;
    state_cache_valid_ = true;
}

void GLBackend::set_cull_face(bool enabled) {
    if (state_cache_valid_ && cull_face_enabled_ == enabled) {
        return;
    }
    if (enabled) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
    } else {
        glDisable(GL_CULL_FACE);
    }
    cull_face_enabled_ = enabled;
    state_cache_valid_ = true;
}

void GLBackend::draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader) {
    IMesh* m = this->mesh(mesh);
    IShader* s = this->shader(shader);
    if (!m || !s) return;
    s->bind();
    GL_CHECK_ERROR();
    m->draw();
    GL_CHECK_ERROR();
    // 不要 unbind：后续 uniform 设置需要当前 program 保持绑定。
}

void GLBackend::draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader) {
    IMesh* m = this->mesh(mesh);
    IShader* s = this->shader(shader);
    if (!m || !s) return;
    s->bind();
    GL_CHECK_ERROR();
    m->draw_indexed();
    GL_CHECK_ERROR();
    // 不要 unbind：后续 uniform 设置需要当前 program 保持绑定。
}

RHIMeshHandle GLBackend::create_mesh() {
    uint32_t index = mesh_pool_.allocate();
    return {index, mesh_pool_.generation(index)};
}

RHIShaderHandle GLBackend::create_shader() {
    uint32_t index = shader_pool_.allocate();
    return {index, shader_pool_.generation(index)};
}

RHITextureHandle GLBackend::create_texture() {
    uint32_t index = texture_pool_.allocate();
    return {index, texture_pool_.generation(index)};
}

RHIFramebufferHandle GLBackend::create_framebuffer() {
    uint32_t index = framebuffer_pool_.allocate();
    return {index, framebuffer_pool_.generation(index)};
}

void GLBackend::destroy_mesh(RHIMeshHandle handle) {
    // 带 generation 校验，防止过期句柄二次销毁误杀复用槽位的新资源
    mesh_pool_.deallocate(handle.index, handle.generation);
}

void GLBackend::destroy_shader(RHIShaderHandle handle) {
    shader_pool_.deallocate(handle.index, handle.generation);
}

void GLBackend::destroy_texture(RHITextureHandle handle) {
    texture_pool_.deallocate(handle.index, handle.generation);
}

void GLBackend::destroy_framebuffer(RHIFramebufferHandle handle) {
    framebuffer_pool_.deallocate(handle.index, handle.generation);
}

IMesh* GLBackend::mesh(RHIMeshHandle handle) {
    return mesh_pool_.get_if_alive(handle.index, handle.generation);
}

IShader* GLBackend::shader(RHIShaderHandle handle) {
    return shader_pool_.get_if_alive(handle.index, handle.generation);
}

ITexture* GLBackend::texture(RHITextureHandle handle) {
    return texture_pool_.get_if_alive(handle.index, handle.generation);
}

IFramebuffer* GLBackend::framebuffer(RHIFramebufferHandle handle) {
    return framebuffer_pool_.get_if_alive(handle.index, handle.generation);
}

const char* GLBackend::api_name() const {
    return "OpenGL";
}

const char* GLBackend::api_version() const {
    return reinterpret_cast<const char*>(glGetString(GL_VERSION));
}

RenderBackendCapabilities GLBackend::get_capabilities() const {
    RenderBackendCapabilities caps;
    caps.supports_vsync_control = true;
    caps.supports_gpu_busy_spin = true;
    caps.supports_nv_delay_before_swap = frame_pacing_.supports_nv_delay_before_swap();
    caps.supports_dynamic_state = false;
    caps.max_texture_slots = 32;
    caps.max_push_constant_size = 0; // OpenGL 无 push constant
    caps.supports_srgb = true;
    caps.supports_depth32f = true;
    caps.supports_r8 = true;
    caps.supports_rgba16f = true;
    return caps;
}

void GLBackend::save_screenshot(const std::string& path) {
    int width = 0, height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    if (width <= 0 || height <= 0) return;

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // OpenGL 返回的是 bottom-up（row 0 为底部），save_bgr_bmp 期望 RGBA top-down，
    // 因此先翻转成 top-down。
    std::vector<unsigned char> flipped(static_cast<std::size_t>(width * height * 4));
    for (int y = 0; y < height; ++y) {
        int src_y = height - 1 - y;
        std::memcpy(flipped.data() + static_cast<std::size_t>(y * width * 4),
                    pixels.data() + static_cast<std::size_t>(src_y * width * 4),
                    static_cast<std::size_t>(width * 4));
    }

    save_bgr_bmp(path, flipped.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    GLOG_INFO("GLBackend: screenshot saved to '{}'", path);
}

std::unique_ptr<IRenderer2D> GLBackend::create_renderer2d() {
    return std::make_unique<Renderer2D>();
}

std::unique_ptr<IImGuiBackend> GLBackend::create_imgui_backend() {
    return std::make_unique<GLImGuiBackend>();
}

void GLBackend::set_validation_enabled(bool /*enabled*/) {
    // OpenGL has no backend validation layers to enable.
}

} // namespace gryce_engine::render