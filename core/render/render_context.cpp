#include "render_context.h"

#include "render_command_buffer.h"
#include "render_thread.h"
#include "render/render.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

namespace {

void execute_typed_command(IRenderBackend* backend, const RenderCommandTyped& cmd) {
    switch (cmd.type) {
        case RenderCommandType::Clear:
            backend->clear(cmd.clear_color.r, cmd.clear_color.g, cmd.clear_color.b, cmd.clear_color.a);
            break;
        case RenderCommandType::ClearDepth:
            backend->clear_depth();
            break;
        case RenderCommandType::SetViewport:
            backend->set_viewport(cmd.viewport.x, cmd.viewport.y, cmd.viewport.w, cmd.viewport.h);
            break;
        case RenderCommandType::SetDepthTest:
            backend->set_depth_test(cmd.depth_test.enabled);
            break;
        case RenderCommandType::SetDepthWrite:
            backend->set_depth_write(cmd.depth_write.enabled);
            break;
        case RenderCommandType::SetBlend:
            backend->set_blend(cmd.blend.enabled);
            break;
        case RenderCommandType::SetBlendFunc:
            backend->set_blend_func(cmd.blend_func.src, cmd.blend_func.dst);
            break;
        case RenderCommandType::SetBlendEquation:
            backend->set_blend_equation(cmd.blend_equation);
            break;
        case RenderCommandType::SetCullFace:
            backend->set_cull_face(cmd.cull_face.enabled);
            break;
        case RenderCommandType::BindFramebuffer:
            backend->bind_framebuffer(cmd.framebuffer);
            break;
        case RenderCommandType::DrawMesh:
            backend->draw_mesh(cmd.mesh, cmd.shader);
            break;
        case RenderCommandType::DrawIndexed:
            backend->draw_indexed(cmd.mesh, cmd.shader);
            break;
        case RenderCommandType::SetSwapInterval:
            backend->set_swap_interval(cmd.swap_interval);
            break;
        case RenderCommandType::SetGpuBusySpin:
            backend->set_gpu_busy_spin(cmd.gpu_busy_spin.enabled, cmd.gpu_busy_spin.iterations);
            break;
        default:
            break;
    }
}

} // namespace

RenderContext::RenderContext() {}

RenderContext::~RenderContext() {
    shutdown();
    lifetime_->alive.store(false);
}

bool RenderContext::init(void* native_window, RenderAPI api) {
    native_window_ = native_window;

    backend_ = create_render_backend(api);
    if (!backend_) {
        GLOG_ERROR("RenderContext::init: unsupported API");
        return false;
    }

    // 校验层开关必须在 instance 创建之前下发才生效
    backend_->set_validation_enabled(validation_enabled_);

    if (!backend_->init(native_window_)) {
        GLOG_ERROR("RenderContext::init: backend init failed");
        return false;
    }

    GLOG_INFO("RenderContext backend initialized: {} {}", backend_->api_name(), backend_->api_version());

    cmd_buffer_ = std::make_unique<RenderCommandBuffer>();
    initialized_ = true;
    return true;
}

void RenderContext::start() {
    if (!initialized_ || running_) return;

    // 释放主线程的 context，由渲染线程接管
    if (backend_) {
        backend_->release_context();
    }

    render_thread_ = std::make_unique<RenderThread>(backend_.get(), cmd_buffer_.get(), native_window_);
    render_thread_->start();
    running_ = true;

    GLOG_INFO("RenderContext server thread started");
}

void RenderContext::pause_render_thread() {
    if (!running_ || !render_thread_) return;

    // 等待渲染线程完成当前帧并优雅退出。
    // 注意：调用方应确保当前帧已经 submit（present 之后），否则未提交的命令会丢失。
    render_thread_->pause();
    render_thread_->join();
    render_thread_.reset();

    // 渲染线程退出时已 release_context()；必须先把 context 切回主线程，
    // 后续 wait_gpu_idle / drain / process_pending_destroys 中的 GL 调用才有 current context。
    if (backend_ && native_window_) {
        backend_->make_current(native_window_);
    }

    // 渲染线程已退出，但 GPU 可能仍在执行已提交的命令。
    // 等待 GPU 真正 idle 后再执行清理命令，避免销毁正在使用的资源。
    if (backend_) {
        backend_->wait_gpu_idle();
    }

    // 把尚未提交到 GPU 的清理命令（例如旧场景销毁产生的 destroy_texture）
    // 在当前线程 GL context 下执行掉，避免资源泄漏。
    if (cmd_buffer_) {
        auto pending = cmd_buffer_->drain();
        for (auto& item : pending) {
            if (item.is_typed()) {
                execute_typed_command(backend_.get(), item.typed());
            } else {
                item.lambda()(backend_.get());
            }
        }
    }

    // pause() 已经关闭旧的 command buffer 并唤醒渲染线程。
    // 这里释放旧对象，resume 时会创建新的 buffer。
    cmd_buffer_.reset();

    // 渲染线程已退出、GPU 已 idle，可以安全处理所有待销毁资源
    process_pending_destroys(true);

    running_ = false;
    GLOG_INFO("RenderContext render thread paused (context back on main thread)");
}

void RenderContext::pause_render_thread_keep_cmdbuffer() {
    if (!running_ || !render_thread_) return;

    render_thread_->pause();
    render_thread_->join();
    render_thread_.reset();

    // 渲染线程退出时已 release_context()；必须先把 context 切回主线程，
    // 后续 wait_gpu_idle / drain / process_pending_destroys 中的 GL 调用才有 current context。
    if (backend_ && native_window_) {
        backend_->make_current(native_window_);
    }

    // 等待 GPU 真正 idle 后再执行清理命令。
    if (backend_) {
        backend_->wait_gpu_idle();
    }

    // 不销毁 cmd_buffer_：暂停后仍可能有同步资源清理命令需要被 drain
    if (cmd_buffer_) {
        auto pending = cmd_buffer_->drain();
        for (auto& item : pending) {
            if (item.is_typed()) {
                execute_typed_command(backend_.get(), item.typed());
            } else {
                item.lambda()(backend_.get());
            }
        }
    }

    // 渲染线程已退出、GPU 已 idle，可以安全处理所有待销毁资源
    process_pending_destroys(true);

    running_ = false;
    GLOG_INFO("RenderContext render thread paused, cmd buffer kept for synchronous cleanup");
}

void RenderContext::resume_render_thread() {
    if (running_ || !initialized_) return;

    // 创建新的 command buffer，替换掉 pause 时关闭的旧 buffer
    cmd_buffer_ = std::make_unique<RenderCommandBuffer>();

    // 释放主线程 context，重新交给渲染线程
    if (backend_) {
        backend_->release_context();
    }

    render_thread_ = std::make_unique<RenderThread>(backend_.get(), cmd_buffer_.get(), native_window_);
    render_thread_->start();
    running_ = true;

    GLOG_INFO("RenderContext render thread resumed");
}

void RenderContext::shutdown() {
    if (!initialized_) return;

    if (render_thread_) {
        render_thread_->stop();
        render_thread_->join();
        render_thread_.reset();
    }

    // 渲染线程已退出，把 context 切回主线程以执行尚未 present 的命令
    if (backend_ && native_window_) {
        backend_->make_current(native_window_);
    }

    if (cmd_buffer_) {
        // drain 剩余命令（常见场景：忘记 final present 的 destroy 命令）
        auto pending = cmd_buffer_->drain();
        for (auto& item : pending) {
            if (item.is_typed()) {
                execute_typed_command(backend_.get(), item.typed());
            } else {
                item.lambda()(backend_.get());
            }
        }
        // 渲染线程已退出，强制处理所有待销毁资源，避免泄漏
        process_pending_destroys(true);
        cmd_buffer_->shutdown();
        cmd_buffer_.reset();
    }

    // 等待 GPU 完成所有已提交命令，再销毁 backend（Vulkan 需要 device idle 后才能安全 destroy device）
    if (backend_) {
        backend_->wait_gpu_idle();
        backend_->release_context();
        backend_.reset();
    }

    initialized_ = false;
    running_ = false;
    lifetime_->alive.store(false);
    GLOG_INFO("RenderContext shutdown");
}

bool RenderContext::is_running() const {
    return running_;
}

bool RenderContext::is_initialized() const {
    return initialized_;
}

// ---------------------------------------------------------------------------
// 资源创建（主线程直接调用 backend，此时 context 仍在主线程）
// ---------------------------------------------------------------------------
RHIMeshHandle RenderContext::create_mesh() {
    return backend_ ? backend_->create_mesh() : RHIMeshHandle{};
}

RHIShaderHandle RenderContext::create_shader() {
    return backend_ ? backend_->create_shader() : RHIShaderHandle{};
}

RHITextureHandle RenderContext::create_texture() {
    return backend_ ? backend_->create_texture() : RHITextureHandle{};
}

RHIFramebufferHandle RenderContext::create_framebuffer() {
    return backend_ ? backend_->create_framebuffer() : RHIFramebufferHandle{};
}

// ---------------------------------------------------------------------------
// 资源访问（渲染线程使用）
// ---------------------------------------------------------------------------
IMesh* RenderContext::mesh(RHIMeshHandle handle) const {
    return backend_ ? backend_->mesh(handle) : nullptr;
}

IShader* RenderContext::shader(RHIShaderHandle handle) const {
    return backend_ ? backend_->shader(handle) : nullptr;
}

ITexture* RenderContext::texture(RHITextureHandle handle) const {
    return backend_ ? backend_->texture(handle) : nullptr;
}

IFramebuffer* RenderContext::framebuffer(RHIFramebufferHandle handle) const {
    return backend_ ? backend_->framebuffer(handle) : nullptr;
}

// ---------------------------------------------------------------------------
// 渲染命令（push 到 CMDBUFFER，由渲染线程执行）
// ---------------------------------------------------------------------------
uint64_t RenderContext::safe_destroy_seq() const {
    // 保守策略：等到当前写入帧以及再多一帧完成后才删除，
    // 防止实体在中途被销毁时当前帧已经 push 了引用该资源的命令。
    return cmd_buffer_ ? (cmd_buffer_->current_write_seq() + 1) : 0;
}

void RenderContext::enqueue_destroy(std::function<void()>&& deleter) {
    std::lock_guard<std::mutex> lock(pending_destroys_mutex_);
    pending_destroys_.push_back({ std::move(deleter), safe_destroy_seq() });
}

void RenderContext::process_pending_destroys(bool force_all) {
    // 渲染线程运行中（非 force_all）时主线程不持有 GL context，
    // 到期 deleter 必须推给渲染线程执行，否则 glDelete* 在无 current context 下
    // 被调用（驱动静默忽略 → 资源泄漏，严格说是 UB）。
    // force_all 只用于 pause/shutdown 路径，彼时渲染线程已退出且
    // context 已切回主线程，可安全内联执行。
    const bool defer_to_render_thread = running_ && !force_all && cmd_buffer_;

    std::vector<std::function<void()>> deferred;
    {
        std::lock_guard<std::mutex> lock(pending_destroys_mutex_);

        uint64_t completed = 0;
        if (cmd_buffer_) {
            completed = cmd_buffer_->completed_seq();
        }

        size_t keep = 0;
        for (size_t i = 0; i < pending_destroys_.size(); ++i) {
            if (force_all || pending_destroys_[i].safe_seq <= completed) {
                if (defer_to_render_thread) {
                    deferred.push_back(std::move(pending_destroys_[i].deleter));
                } else {
                    pending_destroys_[i].deleter();
                }
            } else {
                if (i != keep) {
                    pending_destroys_[keep] = std::move(pending_destroys_[i]);
                }
                ++keep;
            }
        }
        pending_destroys_.resize(keep);
    }

    // 锁外推送，避免持 pending_destroys_mutex_ 期间再拿 cmd_buffer_ 内部锁。
    // 命令进入当前写入帧，随下一次 submit 在渲染线程（context current）执行。
    for (auto& d : deferred) {
        cmd_buffer_->push([d = std::move(d)](IRenderBackend*) { d(); });
    }
}

void RenderContext::destroy_mesh(RHIMeshHandle handle) {
    if (!handle.is_valid()) return;
    if (running_) {
        enqueue_destroy([this, handle]() {
            if (backend_) backend_->destroy_mesh(handle);
        });
    } else {
        if (backend_) backend_->destroy_mesh(handle);
    }
}

void RenderContext::destroy_shader(RHIShaderHandle handle) {
    if (!handle.is_valid()) return;
    if (running_) {
        enqueue_destroy([this, handle]() {
            if (backend_) backend_->destroy_shader(handle);
        });
    } else {
        if (backend_) backend_->destroy_shader(handle);
    }
}

void RenderContext::destroy_texture(RHITextureHandle handle) {
    if (!handle.is_valid()) return;
    if (running_) {
        enqueue_destroy([this, handle]() {
            if (backend_) backend_->destroy_texture(handle);
        });
    } else {
        if (backend_) backend_->destroy_texture(handle);
    }
}

void RenderContext::destroy_framebuffer(RHIFramebufferHandle handle) {
    if (!handle.is_valid()) return;
    if (running_) {
        enqueue_destroy([this, handle]() {
            if (backend_) backend_->destroy_framebuffer(handle);
        });
    } else {
        if (backend_) backend_->destroy_framebuffer(handle);
    }
}

void RenderContext::set_shader(RHIShaderHandle shader) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_shader(shader));
}

void RenderContext::set_uniform_int(RHIShaderHandle shader, const std::string& name, int value) {
    set_uniform_int(shader, name.c_str(), value);
}

void RenderContext::set_uniform_int(RHIShaderHandle shader, const char* name, int value) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_uniform_int(shader, name, value));
}

void RenderContext::set_uniform_float(RHIShaderHandle shader, const std::string& name, float value) {
    set_uniform_float(shader, name.c_str(), value);
}

void RenderContext::set_uniform_float(RHIShaderHandle shader, const char* name, float value) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_uniform_float(shader, name, value));
}

void RenderContext::set_uniform_vec3(RHIShaderHandle shader, const std::string& name, const gryce_engine::math::Vector3f& value) {
    set_uniform_vec3(shader, name.c_str(), value);
}

void RenderContext::set_uniform_vec3(RHIShaderHandle shader, const char* name, const gryce_engine::math::Vector3f& value) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_uniform_vec3(shader, name, value));
}

void RenderContext::set_uniform_vec4(RHIShaderHandle shader, const std::string& name, const gryce_engine::math::Vector4f& value) {
    set_uniform_vec4(shader, name.c_str(), value);
}

void RenderContext::set_uniform_vec4(RHIShaderHandle shader, const char* name, const gryce_engine::math::Vector4f& value) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_uniform_vec4(shader, name, value));
}

void RenderContext::set_uniform_mat4(RHIShaderHandle shader, const std::string& name, const gryce_engine::math::Matrix4f& value) {
    set_uniform_mat4(shader, name.c_str(), value);
}

void RenderContext::set_uniform_mat4(RHIShaderHandle shader, const char* name, const gryce_engine::math::Matrix4f& value) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_uniform_mat4(shader, name, value));
}

void RenderContext::set_uniform_mat4_array(RHIShaderHandle shader, const char* name,
                                           std::shared_ptr<const std::vector<gryce_engine::math::Matrix4f>> values) {
    if (!values || values->empty()) return;
    // 句柄 + shared_ptr 全部按值捕获；命令在渲染线程执行时通过 backend 解析 shader。
    // shader 句柄带 generation 校验：shader 已销毁则 shader(handle) 返回 nullptr，安全跳过。
    cmd_buffer_->push([shader, name = std::string(name ? name : ""), values = std::move(values)](IRenderBackend* backend) {
        if (!backend) return;
        IShader* s = backend->shader(shader);
        if (!s) return;
        s->set_mat4_array(name.c_str(), values->data(), static_cast<uint32_t>(values->size()));
    });
}

void RenderContext::set_texture(RHIShaderHandle shader, RHITextureHandle texture, int slot,
                                const std::string& uniform_name) {
    set_texture(shader, texture, slot, uniform_name.c_str());
}

void RenderContext::set_texture(RHIShaderHandle shader, RHITextureHandle texture, int slot,
                                const char* uniform_name) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_texture(shader, texture, slot, uniform_name ? uniform_name : ""));
}

void RenderContext::clear(float r, float g, float b, float a) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_clear(r, g, b, a));
}

void RenderContext::clear_depth() {
    cmd_buffer_->push_typed(RenderCommandTyped::make_clear_depth());
}

void RenderContext::set_viewport(int x, int y, int w, int h) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_viewport(x, y, w, h));
}

void RenderContext::set_depth_test(bool enabled) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_depth_test(enabled));
}

void RenderContext::set_depth_write(bool enabled) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_depth_write(enabled));
}

void RenderContext::set_blend(bool enabled) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_blend(enabled));
}

void RenderContext::set_cull_face(bool enabled) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_set_cull_face(enabled));
}

void RenderContext::set_framebuffer(RHIFramebufferHandle fb) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_bind_framebuffer(fb));
}

void RenderContext::draw_mesh(RHIMeshHandle mesh, RHIShaderHandle shader) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_draw_mesh(mesh, shader));
}

void RenderContext::draw_indexed(RHIMeshHandle mesh, RHIShaderHandle shader) {
    cmd_buffer_->push_typed(RenderCommandTyped::make_draw_indexed(mesh, shader));
}

void RenderContext::push_command(std::function<void(IRenderBackend*)>&& cmd) {
    cmd_buffer_->push(std::move(cmd));
}

void RenderContext::present() {
    cmd_buffer_->submit();
    process_pending_destroys();
}

void RenderContext::wait_for_idle() {
    if (cmd_buffer_) {
        cmd_buffer_->wait_for_idle();
    }
}

IRenderBackend* RenderContext::backend() const {
    return backend_.get();
}

// ---------------------------------------------------------------------------
// 帧率 / 呈现控制
// ---------------------------------------------------------------------------
void RenderContext::set_swap_interval(int interval) {
    if (!backend_) return;
    if (running_) {
        cmd_buffer_->push_typed(RenderCommandTyped::make_set_swap_interval(interval));
    } else {
        backend_->set_swap_interval(interval);
    }
}

void RenderContext::set_gpu_busy_spin(bool enabled, int iterations) {
    if (!backend_) return;
    if (running_) {
        cmd_buffer_->push_typed(RenderCommandTyped::make_set_gpu_busy_spin(enabled, iterations));
    } else {
        backend_->set_gpu_busy_spin(enabled, iterations);
    }
}

void RenderContext::set_nv_delay_before_swap(float seconds) {
    if (!backend_) return;
    if (running_) {
        cmd_buffer_->push([seconds](IRenderBackend* backend) {
            backend->set_nv_delay_before_swap(seconds);
        });
    } else {
        backend_->set_nv_delay_before_swap(seconds);
    }
}

bool RenderContext::supports_nv_delay_before_swap() const {
    return backend_ ? backend_->supports_nv_delay_before_swap() : false;
}

void RenderContext::request_screenshot(const std::string& path) {
    if (!backend_) return;
    if (running_) {
        cmd_buffer_->push([path](IRenderBackend* backend) {
            backend->request_screenshot(path);
        });
    } else {
        backend_->request_screenshot(path);
    }
}

std::unique_ptr<IRenderer2D> RenderContext::create_renderer2d() {
    return backend_ ? backend_->create_renderer2d() : nullptr;
}

std::unique_ptr<IImGuiBackend> RenderContext::create_imgui_backend() {
    return backend_ ? backend_->create_imgui_backend() : nullptr;
}

void RenderContext::set_validation_enabled(bool enabled) {
    // 记录并在 init 时（instance 创建前）应用；init 之后再调用也直接生效
    validation_enabled_ = enabled;
    if (backend_) {
        backend_->set_validation_enabled(enabled);
    }
}

} // namespace gryce_engine::render
