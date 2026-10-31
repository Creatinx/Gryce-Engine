#include "render_thread.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "render_command_buffer.h"
#include "render/render.h"
#include "render/render_commands.h"
#include "render/mesh.h"
#include "render/shader.h"
#include "render/buffer.h"
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
            backend->set_cull_face(cmd.cull_face.mode);
            break;
        case RenderCommandType::BindFramebuffer:
            backend->bind_framebuffer(cmd.framebuffer);
            break;
        case RenderCommandType::ResolveFramebuffer:
            backend->resolve_framebuffer(cmd.framebuffer, cmd.framebuffer_dst);
            break;
        case RenderCommandType::DrawMesh:
            backend->draw_mesh(cmd.mesh, cmd.shader);
            break;
        case RenderCommandType::DrawIndexed:
            backend->draw_indexed(cmd.mesh, cmd.shader);
            break;
        case RenderCommandType::SetPostProcessParams: {
            // 后处理参数必须在渲染线程按命令序应用：D3D12 的 uniform 影子内存
            // 按帧复位（on_begin_frame），主线程录制期直接写会被擦掉。
            IShader* s = backend->shader(cmd.shader);
            if (s && cmd.post_process_params) {
                s->set_post_process_params(*cmd.post_process_params);
            }
            break;
        }
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
            ITexture* t = backend->texture(cmd.texture);
            if (!t) return;
            // 槽位可能超过后端纹理单元上限（GL 片段单元常见 32），统一换算
            const int unit = backend->texture_unit_for_slot(cmd.uniform_int);
            if (cmd.texture_raw_depth) {
                t->bind_raw_depth(unit);
            } else {
                t->bind(unit);
            }
            // shader 可能为空（级联阴影/SSAO 等仅绑定纹理单元，采样器由 set_uniform_int 单独指定），
            // 此时仍须完成纹理绑定，不能因空句柄跳过。
            IShader* s = backend->shader(cmd.shader);
            if (s) {
                if (cmd.uniform_name && cmd.uniform_name[0]) {
                    s->set_int(cmd.uniform_name, unit);
                }
                s->set_texture(unit, t);
            }
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
        case RenderCommandType::SetUniformVec2: {
            IShader* s = backend->shader(cmd.shader);
            if (s) s->set_vec2(cmd.uniform_name, cmd.uniform_vec2);
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
        case RenderCommandType::DispatchCompute:
            backend->dispatch_compute(cmd.shader, cmd.compute_grid.gx, cmd.compute_grid.gy,
                                      cmd.compute_grid.gz);
            break;
        case RenderCommandType::BindStorageBuffer:
            if (cmd.storage_name) {
                backend->bind_storage_buffer(cmd.shader, cmd.storage_name->c_str(), cmd.buffer);
            }
            break;
        case RenderCommandType::BindInstanceStream:
            backend->bind_instance_stream(cmd.buffer);
            break;
        case RenderCommandType::DrawIndexedIndirect:
            backend->draw_indexed_indirect(cmd.mesh, cmd.shader, cmd.buffer,
                                           static_cast<uint32_t>(cmd.uniform_int));
            break;
        case RenderCommandType::DrawArraysIndirect:
            backend->draw_arrays_indirect(cmd.mesh, cmd.shader, cmd.buffer,
                                          static_cast<uint32_t>(cmd.uniform_int));
            break;
        case RenderCommandType::UpdateBuffer: {
            IBuffer* b = backend->buffer(cmd.buffer);
            if (b && cmd.buffer_data) {
                b->update(cmd.buffer_data->data(), cmd.buffer_data->size(), cmd.buffer_offset);
            }
            break;
        }
        case RenderCommandType::CopyTextureToBuffer:
            backend->copy_texture_to_buffer(cmd.texture, cmd.buffer,
                                            static_cast<uint32_t>(cmd.uniform_int));
            break;
        case RenderCommandType::EndRenderPassForTransfer:
            backend->end_render_pass_for_transfer();
            break;
        case RenderCommandType::CreateBufferStorage: {
            IBuffer* b = backend->buffer(cmd.buffer);
            if (!b) break;
            // ★ 先声明"这是一块 SSBO"（D3D12 必须据此选 DEFAULT 堆 + UAV 标志）。
            b->set_storage(true);
            b->create(cmd.buffer_size,
                      (cmd.buffer_data && !cmd.buffer_data->empty()) ? cmd.buffer_data->data()
                                                                     : nullptr,
                      cmd.buffer_usage);
            break;
        }
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
    CullMode cull_face = CullMode::Back;
    BlendFactor blend_src = BlendFactor::One;
    BlendFactor blend_dst = BlendFactor::One;
    BlendEquation blend_eq = BlendEquation::Add;
    int viewport_x = -1, viewport_y = -1, viewport_w = -1, viewport_h = -1;
    int scissor_x = -1, scissor_y = -1, scissor_w = -1, scissor_h = -1;
    RHIFramebufferHandle framebuffer;
    bool has_framebuffer = false;
    // 当前绑定的 shader：主 pass 里同一 shader 会连续用于多个物体，跳过重复
    // 的 SetShader（避免逐物体的 glUseProgram 与后端句柄查找）。
    RHIShaderHandle bound_shader;
    bool has_shader = false;
    bool initialized = false;

    static constexpr int kMaxTextureSlots = 32;
    static constexpr size_t kMaxIntUniformCache = 64;
    std::array<RHITextureHandle, kMaxTextureSlots> bound_textures;
    std::array<RHIShaderHandle, kMaxTextureSlots> bound_texture_shaders;
    std::array<bool, kMaxTextureSlots> bound_texture_raws;
    bool texture_slots_initialized = false;

    // int uniform 值缓存。键 = (shader, uniform 名指针)。名字是字面量，指针
    // 即身份，无需字符串比较。用 vector 线性查找（同一 shader 的 int uniform
    // 数量通常 < 32），比哈希表更适合这个规模，也避免每帧分配。
    struct IntUniformKey {
        RHIShaderHandle shader;
        const char* name;
        bool operator==(const IntUniformKey& o) const {
            return shader == o.shader && name == o.name;
        }
    };
    std::vector<std::pair<IntUniformKey, int>> int_uniform_cache_;

    // float / vec2 / vec3 / vec4 / mat4 的值缓存。键=(shader, 名字指针, 类型)：
    // 名是字面量，指针即身份。场景 pass 里每个物体都会重发一遍相机矩阵、环境光、
    // PCSS 参数、级联阴影矩阵等 —— 这些值在整帧内恒定，只有 uModelMatrix /
    // uMVP / uNormalMatrix 真正逐物体变化。缓存命中即跳过整条后端调用（含
    // VulkanShader 内部的名字比较）。
    //
    // 正确性：VulkanShader 在每个 draw 时把整份 ubo_data_ 拷进该 draw 的槽位，
    // 所以"上一 draw 已写入同值"意味着本 draw 的槽位同样能拿到该值，跳过安全。
    //
    // 结构用**开放寻址哈希表**而不是线性 vector：跨帧保留后条目数会涨到几百，
    // 线性查找的代价会超过它省下的字符串比较，缓存反而变成负债。
    static constexpr size_t kValueCacheCap = 1024;  // 2 的幂
    struct ValueUniformEntry {
        RHIShaderHandle shader;
        const char* name = nullptr;
        uint8_t kind = 0xFF;
        bool used = false;
        float value[16] = {};
    };
    std::array<ValueUniformEntry, kValueCacheCap> value_cache_{};

    static size_t value_slot_hash(const RHIShaderHandle& s, const char* name, uint8_t kind) {
        uint64_t x = (static_cast<uint64_t>(s.index) << 32) | s.generation;
        x ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(name));
        x ^= static_cast<uint64_t>(kind) << 56;
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33;
        return static_cast<size_t>(x);
    }

    // 返回 true 表示"值变了、需要下发"。
    bool value_changed(const RenderCommandTyped& cmd, uint8_t kind, const float* value,
                       size_t count) {
        const size_t mask = kValueCacheCap - 1;
        size_t i = value_slot_hash(cmd.shader, cmd.uniform_name, kind) & mask;
        for (size_t probe = 0; probe < 32; ++probe, i = (i + 1) & mask) {
            ValueUniformEntry& e = value_cache_[i];
            if (!e.used) {
                e.used = true;
                e.shader = cmd.shader;
                e.name = cmd.uniform_name;
                e.kind = kind;
                std::memcpy(e.value, value, count * sizeof(float));
                return true;
            }
            if (e.shader == cmd.shader && e.name == cmd.uniform_name && e.kind == kind) {
                if (std::memcmp(e.value, value, count * sizeof(float)) == 0) return false;
                std::memcpy(e.value, value, count * sizeof(float));
                return true;
            }
        }
        // 探测窗口内没有空槽：宁可多下发一次，也不破坏正确性。
        return true;
    }

    void clear_value_cache() {
        for (auto& e : value_cache_) e.used = false;
    }

    bool should_dispatch(const RenderCommandTyped& cmd) {
        switch (cmd.type) {
            case RenderCommandType::SetDepthTest: {
                if (initialized && depth_test == cmd.depth_test.enabled) return false;
                depth_test = cmd.depth_test.enabled;
                break;
            }
            case RenderCommandType::SetBlend: {
                // 后端 set_blend(enabled) 带副作用：启用/关闭混合的同时会把混合
                // 因子重置为 SrcAlpha/OneMinusSrcAlpha、方程重置为 Add（GL 语义）。
                // 缓存必须镜像这一副作用，否则后续 SetBlendFunc 会误判"已匹配"
                // 而跳过，使加法混合退化成 alpha 混合（SSR 合成会整帧变黑）。
                const bool same = initialized && blend == cmd.blend.enabled &&
                                  blend_src == BlendFactor::SrcAlpha &&
                                  blend_dst == BlendFactor::OneMinusSrcAlpha &&
                                  blend_eq == BlendEquation::Add;
                if (same) return false;
                blend = cmd.blend.enabled;
                blend_src = BlendFactor::SrcAlpha;
                blend_dst = BlendFactor::OneMinusSrcAlpha;
                blend_eq = BlendEquation::Add;
                break;
            }
            case RenderCommandType::SetBlendFunc: {
                if (initialized && blend_src == cmd.blend_func.src &&
                    blend_dst == cmd.blend_func.dst) {
                    return false;
                }
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
                if (initialized && cull_face == cmd.cull_face.mode) return false;
                cull_face = cmd.cull_face.mode;
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
                        bound_texture_shaders[slot] == cmd.shader &&
                        bound_texture_raws[slot] == cmd.texture_raw_depth) {
                        return false;
                    }
                    bound_textures[slot] = cmd.texture;
                    bound_texture_shaders[slot] = cmd.shader;
                    bound_texture_raws[slot] = cmd.texture_raw_depth;
                    texture_slots_initialized = true;
                }
                break;
            }
            case RenderCommandType::SetShader: {
                if (initialized && has_shader && bound_shader == cmd.shader) return false;
                bound_shader = cmd.shader;
                has_shader = true;
                // shader 变了，int 值缓存作废（不同 shader 的同名 uniform 是
                // 独立状态）。值缓存不用清：它的键里带 shader，天然不会串到
                // 别的 shader 上，清了反而丢掉来回切换时的命中。
                int_uniform_cache_.clear();
                break;
            }
            // int uniform 缓存：本引擎的 uniform 名全部是编译期字符串字面量，
            // 生命周期覆盖整条命令，故可安全地按 (shader, name 指针) 做键。
            // 材质绑定里 6 组 uUseXxxMap/uXxxMap 每实体各写一次，值几乎恒定，
            // 这一层能直接省掉数千条冗余后端调用。
            case RenderCommandType::SetUniformInt: {
                if (!cmd.uniform_name) return true;
                const IntUniformKey key{cmd.shader, cmd.uniform_name};
                for (auto& e : int_uniform_cache_) {
                    if (e.first == key) {
                        if (e.second == cmd.uniform_int) return false;
                        e.second = cmd.uniform_int;
                        break;
                    }
                }
                // 未命中：登记（容量给足，超出后仍正确，只是不再增长缓存）。
                if (int_uniform_cache_.size() < kMaxIntUniformCache) {
                    int_uniform_cache_.emplace_back(key, cmd.uniform_int);
                }
                break;
            }
            case RenderCommandType::SetUniformFloat: {
                if (!cmd.uniform_name) return true;
                if (!value_changed(cmd, 0, &cmd.uniform_float, 1)) return false;
                break;
            }
            case RenderCommandType::SetUniformVec2: {
                if (!cmd.uniform_name) return true;
                if (!value_changed(cmd, 1, &cmd.uniform_vec2.x, 2)) return false;
                break;
            }
            case RenderCommandType::SetUniformVec3: {
                if (!cmd.uniform_name) return true;
                if (!value_changed(cmd, 2, &cmd.uniform_vec3.x, 3)) return false;
                break;
            }
            case RenderCommandType::SetUniformVec4: {
                if (!cmd.uniform_name) return true;
                if (!value_changed(cmd, 3, &cmd.uniform_vec4.x, 4)) return false;
                break;
            }
            case RenderCommandType::SetUniformMat4: {
                if (!cmd.uniform_name) return true;
                if (!value_changed(cmd, 4, cmd.uniform_mat4.m, 16)) return false;
                break;
            }
            // 其他命令（draw, clear, swap）不做缓存，总是执行
            default:
                return true;        }
        initialized = true;
        return true;
    }

    void reset(bool keep_uniform_values) {
        initialized = false;
        has_framebuffer = false;
        texture_slots_initialized = false;
        has_shader = false;
        int_uniform_cache_.clear();
        // 只有值缓存能跨帧：状态类命令（framebuffer/viewport/blend/纹理槽）
        // 每帧都会重新下发一遍，留着缓存只会让首条命令被误判为"已设置"。
        if (!keep_uniform_values) clear_value_cache();
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
    if (!cmd_buffer_ || !backend_) {
        GLOG_ERROR("RenderThread::start: missing cmd_buffer or backend");
        return;
    }
    running_.store(true);
    ready_.store(false);
    stop_requested_.store(false);
    shutdown_backend_on_exit_.store(true);
    try {
        thread_ = std::thread(&RenderThread::thread_loop, this);
    } catch (const std::exception& e) {
        running_.store(false);
        GLOG_ERROR("RenderThread::start: failed to create thread: {}", e.what());
        return;
    }
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
        // 清空线程 id：线程已退出，此后任何线程都不算"渲染线程"。
        thread_id_ = std::thread::id{};
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
    thread_id_ = std::this_thread::get_id();
    GLOG_INFO("RenderThread thread_loop entered");
    ready_.store(true);

    // 渲染线程分段计时（GRYCE_RT_PROF=1）。开关只在这里读一次 —— Windows CRT
    // 的 getenv 单次约 5us，放进每帧路径会把被测量的东西自己改掉。
    const bool rt_prof = std::getenv("GRYCE_RT_PROF") != nullptr;
    double rt_acc[4] = {0, 0, 0, 0};  // wait / begin / exec / end
    int rt_frames = 0;
    // 状态缓存提到循环外：uniform 值缓存要跨帧存活（见 reset 的说明）。
    CommandStateCache state_cache;
    const bool keep_uniform_values = backend_->uniform_state_persistent();

    while (!stop_requested_.load()) {
        if (!cmd_buffer_ || !backend_) {
            GLOG_ERROR("RenderThread: cmd_buffer or backend became null, exiting");
            break;
        }
        GLOG_DEBUG("RenderThread: waiting for commands");
        const auto w0 = std::chrono::steady_clock::now();
        auto* commands = cmd_buffer_->acquire();
        const auto w1 = std::chrono::steady_clock::now();
        if (!commands) {
            // shutdown 信号
            GLOG_INFO("RenderThread: acquire returned null, exiting");
            break;
        }
        GLOG_DEBUG("RenderThread: acquired {} commands", commands->size());

        backend_->begin_frame();
        const auto w2 = std::chrono::steady_clock::now();
        state_cache.reset(keep_uniform_values);
        for (auto& item : *commands) {
            if (item.is_typed()) {
                if (state_cache.should_dispatch(item.typed())) {
                    dispatch_typed_command(backend_, item.typed());
                }
            } else {
                item.lambda()(backend_);
                // 非类型化 lambda 可任意调用后端状态接口（如 Renderer2D 直接调
                // set_blend_func），缓存无法跟踪其副作用；执行后整体失效，
                // 保证后续类型化状态命令至少重新下发一次，避免误跳过导致
                // 实际后端状态与缓存不一致。lambda 可能改写了任意 shader 的
                // uniform，值缓存也一并丢弃（它很少出现，不承担性能作用）。
                state_cache.reset(false);
            }
        }

        const auto w3 = std::chrono::steady_clock::now();

        backend_->end_frame();
        const auto w4 = std::chrono::steady_clock::now();

        if (rt_prof) {
            rt_acc[0] += std::chrono::duration<double, std::milli>(w1 - w0).count();
            rt_acc[1] += std::chrono::duration<double, std::milli>(w2 - w1).count();
            rt_acc[2] += std::chrono::duration<double, std::milli>(w3 - w2).count();
            rt_acc[3] += std::chrono::duration<double, std::milli>(w4 - w3).count();
            if (++rt_frames >= 120) {
                std::fprintf(stderr,
                             "[rtprof] wait=%.3f begin=%.3f exec=%.3f end=%.3f cmds=%zu\n",
                             rt_acc[0] / rt_frames, rt_acc[1] / rt_frames,
                             rt_acc[2] / rt_frames, rt_acc[3] / rt_frames, commands->size());
                for (double& v : rt_acc) v = 0.0;
                rt_frames = 0;
            }
        }

        cmd_buffer_->release();
        GLOG_DEBUG("RenderThread: frame released");
    }

    if (shutdown_backend_on_exit_.load()) {
        if (backend_) backend_->shutdown();
    }
    if (backend_) {
        backend_->release_context();
    }
}

} // namespace gryce_engine::render
