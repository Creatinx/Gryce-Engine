#include "render_thread.h"

#include <array>

#include "render_command_buffer.h"
#include "render/render.h"
#include "render/render_commands.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

namespace {

void dispatch_typed_command(IRenderBackend* backend, const RenderCommandTyped& cmd) {
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
        case RenderCommandType::SetShader: {
            IShader* s = backend->shader(cmd.shader);
            if (s) s->bind();
            break;
        }
        case RenderCommandType::SetTexture: {
            IShader* s = backend->shader(cmd.shader);
            ITexture* t = backend->texture(cmd.texture);
            if (!s || !t) return;
            t->bind(cmd.uniform_int);
            if (!cmd.uniform_name.empty()) {
                s->set_int(cmd.uniform_name, cmd.uniform_int);
            }
            s->set_texture(cmd.uniform_int, t);
            break;
        }
        case RenderCommandType::SetUniformInt: {
            IShader* s = backend->shader(cmd.shader);
            if (s) s->set_int(cmd.uniform_name, cmd.uniform_int);
            break;
        }
        case RenderCommandType::SetUniformFloat: {
            IShader* s = backend->shader(cmd.shader);
            if (s) s->set_float(cmd.uniform_name, cmd.uniform_float);
            break;
        }
        case RenderCommandType::SetUniformVec3: {
            IShader* s = backend->shader(cmd.shader);
            if (s) s->set_vec3(cmd.uniform_name, cmd.uniform_vec3);
            break;
        }
        case RenderCommandType::SetUniformVec4: {
            IShader* s = backend->shader(cmd.shader);
            if (s) s->set_vec4(cmd.uniform_name, cmd.uniform_vec4);
            break;
        }
        case RenderCommandType::SetUniformMat4: {
            IShader* s = backend->shader(cmd.shader);
            if (s) s->set_mat4(cmd.uniform_name, cmd.uniform_mat4);
            break;
        }
        case RenderCommandType::SetScissor:
            backend->set_scissor(cmd.scissor.x, cmd.scissor.y, cmd.scissor.w, cmd.scissor.h);
            break;
        case RenderCommandType::CustomLambda:
        case RenderCommandType::None:
            break;
    }
}

// ---------------------------------------------------------------------------
// CommandStateCache — 渲染命令状态缓存，跳过冗余的状态设置命令。
// 每帧开始时重置，执行 draw/uniform 等不缓存的命令后状态仍然保持。
// 只缓存状态设置命令（SetDepthTest/SetBlend/SetViewport/...），不缓存 draw/uniform。
// ---------------------------------------------------------------------------
struct CommandStateCache {
    bool depth_test = false;
    bool blend = false;
    bool cull_face = false;
    BlendFactor blend_src = BlendFactor::One;
    BlendFactor blend_dst = BlendFactor::One;
    BlendEquation blend_eq = BlendEquation::Add;
    int viewport_x = -1, viewport_y = -1, viewport_w = -1, viewport_h = -1;
    int scissor_x = -1, scissor_y = -1, scissor_w = -1, scissor_h = -1;
    RHIFramebufferHandle framebuffer;
    bool has_framebuffer = false;
    bool initialized = false;

    static constexpr int kMaxTextureSlots = 32;
    std::array<RHITextureHandle, kMaxTextureSlots> bound_textures;
    std::array<RHIShaderHandle, kMaxTextureSlots> bound_texture_shaders;
    bool texture_slots_initialized = false;

    bool should_dispatch(const RenderCommandTyped& cmd) {
        switch (cmd.type) {
            case RenderCommandType::SetDepthTest: {
                if (initialized && depth_test == cmd.depth_test.enabled) return false;
                depth_test = cmd.depth_test.enabled;
                break;
            }
            case RenderCommandType::SetBlend: {
                if (initialized && blend == cmd.blend.enabled) return false;
                blend = cmd.blend.enabled;
                break;
            }
            case RenderCommandType::SetBlendFunc: {
                if (initialized && blend_src == cmd.blend_func.src && blend_dst == cmd.blend_func.dst) return false;
                blend_src = cmd.blend_func.src;
                blend_dst = cmd.blend_func.dst;
                break;
            }
            case RenderCommandType::SetBlendEquation: {
                if (initialized && blend_eq == cmd.blend_equation) return false;
                blend_eq = cmd.blend_equation;
                break;
            }
            case RenderCommandType::SetCullFace: {
                if (initialized && cull_face == cmd.cull_face.enabled) return false;
                cull_face = cmd.cull_face.enabled;
                break;
            }
            case RenderCommandType::SetViewport: {
                if (initialized && viewport_x == cmd.viewport.x && viewport_y == cmd.viewport.y &&
                    viewport_w == cmd.viewport.w && viewport_h == cmd.viewport.h) return false;
                viewport_x = cmd.viewport.x; viewport_y = cmd.viewport.y;
                viewport_w = cmd.viewport.w; viewport_h = cmd.viewport.h;
                break;
            }
            case RenderCommandType::SetScissor: {
                if (initialized && scissor_x == cmd.scissor.x && scissor_y == cmd.scissor.y &&
                    scissor_w == cmd.scissor.w && scissor_h == cmd.scissor.h) return false;
                scissor_x = cmd.scissor.x; scissor_y = cmd.scissor.y;
                scissor_w = cmd.scissor.w; scissor_h = cmd.scissor.h;
                break;
            }
            case RenderCommandType::BindFramebuffer: {
                if (initialized && has_framebuffer && framebuffer == cmd.framebuffer) return false;
                framebuffer = cmd.framebuffer;
                has_framebuffer = true;
                break;
            }
            case RenderCommandType::SetTexture: {
                const int slot = cmd.uniform_int;
                if (slot >= 0 && slot < kMaxTextureSlots) {
                    if (texture_slots_initialized &&
                        bound_textures[slot] == cmd.texture &&
                        bound_texture_shaders[slot] == cmd.shader) {
                        return false;
                    }
                    bound_textures[slot] = cmd.texture;
                    bound_texture_shaders[slot] = cmd.shader;
                    texture_slots_initialized = true;
                }
                break;
            }
            // 其他命令（draw, clear, uniform, shader, swap）不做缓存，总是执行
            default:
                return true;
        }
        initialized = true;
        return true;
    }

    void reset() {
        initialized = false;
        has_framebuffer = false;
        texture_slots_initialized = false;
    }
};

} // namespace

RenderThread::RenderThread(IRenderBackend* backend, RenderCommandBuffer* cmd_buffer, void* native_window)
    : backend_(backend)
    , cmd_buffer_(cmd_buffer)
    , native_window_(native_window) {}

RenderThread::~RenderThread() {
    stop();
    join();
}

void RenderThread::start() {
    if (running_.load()) return;
    running_.store(true);
    stop_requested_.store(false);
    thread_ = std::thread(&RenderThread::thread_loop, this);
    if (thread_.joinable()) {
        GLOG_INFO("RenderThread thread is joinable");
    } else {
        GLOG_WARN("RenderThread thread is NOT joinable");
    }
    GLOG_INFO("RenderThread started");
}

void RenderThread::stop() {
    if (!running_.load()) return;
    stop_requested_.store(true);
    // 不在渲染线程里调用 backend_->shutdown()：
    // Vulkan 若在此处销毁 device，主线程 drain 剩余命令时（delete framebuffer/texture）
    // 会拿到无效 device。backend 的销毁交给 RenderContext::shutdown() 在主线程执行。
    shutdown_backend_on_exit_.store(false);
    if (cmd_buffer_) {
        cmd_buffer_->shutdown();
    }
    GLOG_INFO("RenderThread stop requested");
}

void RenderThread::pause() {
    if (!running_.load()) return;
    stop_requested_.store(true);
    shutdown_backend_on_exit_.store(false);
    // 关闭 command buffer 以唤醒正在等待命令的渲染线程，使其安全退出。
    // resume 时会创建新的 command buffer。
    if (cmd_buffer_) {
        cmd_buffer_->shutdown();
    }
    GLOG_INFO("RenderThread pause requested");
}

void RenderThread::join() {
    if (thread_.joinable()) {
        thread_.join();
        running_.store(false);
        GLOG_INFO("RenderThread joined");
    }
}

bool RenderThread::is_running() const {
    return running_.load();
}

void RenderThread::thread_loop() {
    // 将 context 绑定到渲染线程
    if (backend_) {
        backend_->make_current(native_window_);
    }
    GLOG_INFO("RenderThread thread_loop entered");

    while (!stop_requested_.load()) {
        auto* commands = cmd_buffer_->acquire();
        if (!commands) {
            // shutdown 信号
            break;
        }

        backend_->begin_frame();
        CommandStateCache state_cache;
        for (auto& item : *commands) {
            if (item.is_typed()) {
                if (state_cache.should_dispatch(item.typed())) {
                    dispatch_typed_command(backend_, item.typed());
                }
            } else {
                item.lambda()(backend_);
            }
        }

        backend_->end_frame();

        cmd_buffer_->release();
    }

    if (shutdown_backend_on_exit_.load()) {
        backend_->shutdown();
    }
    if (backend_) {
        backend_->release_context();
    }
}

} // namespace gryce_engine::render
