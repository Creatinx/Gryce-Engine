#include "d3d12_backend.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include "utils/glog/glog_lib.h"
#include "stb/stb_image_write.h"
#include "d3d12_renderer2d.h"
#ifdef GRYCE_D3D12_HAS_IMGUI
#include "d3d12_imgui_backend.h"
#endif

namespace gryce_engine::render::d3d12 {

// native_window 是 GLFWwindow*（render_context.cpp:157 传的就是它）。
// D3D12 需要真实 HWND —— GLFW 的 NoApi 窗口用 glfwGetWin32Window 取出。
// 注意：这里不能把 void* 直接当 HWND 用，否则 CreateSwapChainForHwnd
// 会返回 E_ACCESSDENIED (0x80070005) —— 排查时容易被误导成权限/子窗口问题。
HWND D3D12Backend::hwnd_from_native(void* native_window) {
    if (!native_window) return nullptr;
    auto* win = static_cast<GLFWwindow*>(native_window);
    HWND hwnd = glfwGetWin32Window(win);
    if (!hwnd) {
        GLOG_ERROR("D3D12: glfwGetWin32Window returned null (window not a Win32 window?)");
    }
    return hwnd;
}

D3D12Backend::~D3D12Backend() {
    shutdown();
}

bool D3D12Backend::init(void* native_window) {
    if (initialized_) {
        GLOG_WARN("D3D12::init called twice; ignoring");
        return true;
    }
    hwnd_ = hwnd_from_native(native_window);
    if (!hwnd_) {
        GLOG_ERROR("D3D12::init: native_window is null or not a Win32 GLFW window");
        return false;
    }

    // 1) 加载 d3d12.dll / dxgi.dll 并解析入口点
    if (!init_loader(loader_)) {
        return false;
    }
    // 登记给 ImGui DX12 后端的转发函数用（CreateDXGIFactory1 / D3DCompile）。
    register_loader(&loader_);

    // 2) DXGI Factory。
    // 环境变量 GRYCE_D3D12_DEBUG=1 时打开 D3D12 调试层：它会逐条校验 API 调用，
    // 把违规记进 ID3D12InfoQueue。**这是定位"设备被移除（0x887A0005）"的唯一
    // 有效手段** —— 否则只能看到后果，看不到是哪一条调用非法。
    // 默认关闭（调试层会显著变慢），所以对正常跑分/CI 零影响。
    bool want_debug = false;
    {
        char buf[8] = {};
        const DWORD n = GetEnvironmentVariableA("GRYCE_D3D12_DEBUG", buf, sizeof(buf));
        want_debug = (n > 0 && buf[0] != '0');
    }
    if (want_debug && loader_.get_debug_interface) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(loader_.get_debug_interface(__mingw_uuidof<ID3D12Debug>(),
                                                  reinterpret_cast<void**>(&dbg))) &&
            dbg) {
            dbg->EnableDebugLayer();
            debug_layer_enabled_ = true;
            GLOG_INFO("D3D12: debug layer enabled (GRYCE_D3D12_DEBUG=1)");
            dbg->Release();
        } else {
            GLOG_WARN("D3D12: debug layer unavailable (install Windows 'Graphics Tools')");
        }
    }

    UINT factory_flags = debug_layer_enabled_ ? DXGI_CREATE_FACTORY_DEBUG : 0;
    HRESULT hr = loader_.create_factory(factory_flags, __mingw_uuidof<IDXGIFactory4>(),
                                        reinterpret_cast<void**>(&loader_.factory));
    if (FAILED(hr) || !loader_.factory) {
        GLOG_ERROR("D3D12::init: CreateDXGIFactory2 failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        release_device_objects();
        return false;
    }

    // 3) 首个适配器（EnumAdapters1(0) 通常是首选 GPU）
    hr = loader_.factory->EnumAdapters1(0, &adapter_);
    if (FAILED(hr) || !adapter_) {
        GLOG_ERROR("D3D12::init: EnumAdapters1(0) failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        release_device_objects();
        return false;
    }

    {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter_->GetDesc1(&desc))) {
            char name[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1,
                                nullptr, nullptr);
            std::snprintf(adapter_desc_, sizeof(adapter_desc_), "%s (%llu MB)", name,
                          static_cast<unsigned long long>(desc.DedicatedVideoMemory / (1024 * 1024)));
        } else {
            std::snprintf(adapter_desc_, sizeof(adapter_desc_), "D3D12 adapter");
        }
    }

    // 4) 设备。先试 FL 11_0（兼容面最广），失败再试 12_0。
    feature_level_ = D3D_FEATURE_LEVEL_11_0;
    hr = loader_.create_device(adapter_, feature_level_, __mingw_uuidof<ID3D12Device>(),
                               reinterpret_cast<void**>(&device_));
    if (FAILED(hr)) {
        feature_level_ = D3D_FEATURE_LEVEL_12_0;
        hr = loader_.create_device(adapter_, feature_level_, __mingw_uuidof<ID3D12Device>(),
                                   reinterpret_cast<void**>(&device_));
    }
    if (FAILED(hr) || !device_) {
        GLOG_ERROR("D3D12::init: D3D12CreateDevice failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        release_device_objects();
        return false;
    }

    // 5) DIRECT 命令队列
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qdesc.NodeMask = 0;
    qdesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    hr = device_->CreateCommandQueue(&qdesc, __mingw_uuidof<ID3D12CommandQueue>(),
                                     reinterpret_cast<void**>(&queue_));
    if (FAILED(hr) || !queue_) {
        GLOG_ERROR("D3D12::init: CreateCommandQueue failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        release_device_objects();
        return false;
    }

    // 6) 交换链（FLIP_DISCARD，3 缓冲）。尺寸取 HWND 客户区。
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const uint32_t w = static_cast<uint32_t>(rc.right - rc.left);
    const uint32_t h = static_cast<uint32_t>(rc.bottom - rc.top);
    if (!swapchain_.init(loader_, device_, queue_, hwnd_, w, h, 3)) {
        GLOG_ERROR("D3D12::init: swapchain init failed");
        release_device_objects();
        return false;
    }
    viewport_w_ = static_cast<int>(swapchain_.width());
    viewport_h_ = static_cast<int>(swapchain_.height());

    // 7) 描述符堆（M3）：SRV/CBV/UAV 与 Sampler 各一块 SHADER_VISIBLE 堆。
    //    容量按 4096 / 256 起步（DescriptorAllocator 满时会自动扩容）。
    if (!descriptor_heaps_.init(device_, 8192, 2048)) {
        GLOG_ERROR("D3D12::init: descriptor heap set init failed");
        release_device_objects();
        return false;
    }

    // 8) 上传环（M3）：UBO / 每帧实例数据用。4MB 足够 M3 阶段，后续按需调。
    if (!upload_ring_.init(device_, 4 * 1024 * 1024)) {
        GLOG_ERROR("D3D12::init: upload ring init failed");
        release_device_objects();
        return false;
    }

    initialized_ = true;
    GLOG_INFO("D3D12::init OK (FL=0x{:04x}, adapter={}, {}x{})",
              static_cast<unsigned>(feature_level_), adapter_desc_,
              swapchain_.width(), swapchain_.height());
    return true;
}

void D3D12Backend::shutdown() {
    if (!initialized_ && !device_ && !loader_.d3d12_dll) return;
    // 开了调试层（GRYCE_D3D12_DEBUG=1）时把校验消息导出来 —— 有些非法状态
    // （格式与绑定的 RTV 不匹配、描述符未初始化等）不会移除设备，只在
    // ID3D12InfoQueue 里留 ERROR/WARNING，退出前不打出来就等于白开调试层。
    if (debug_layer_enabled_) dump_info_queue();
    release_device_objects();
    GLOG_INFO("D3D12::shutdown OK");
}

void D3D12Backend::release_device_objects() {
    // 顺序：资源池（依赖 device/queue）→ 描述符堆 / 上传环 → swapchain
    //       （内含 fence/allocator/backbuffer）→ queue → device → adapter
    //       → factory → loader
    //
    // 资源池必须先于 device 释放：池内对象的析构会 Release 各自的 ID3D12Resource，
    // 设备提前释放会留下悬空引用。
    bound_framebuffer_ = RHIFramebufferHandle{};
    mesh_pool_.clear();
    texture_pool_.clear();
    framebuffer_pool_.clear();
    buffer_pool_.clear();

    upload_ring_.shutdown();
    descriptor_heaps_.shutdown();

    swapchain_.shutdown();

    if (queue_)   { queue_->Release();   queue_   = nullptr; }
    if (device_)  { device_->Release();  device_  = nullptr; }
    if (adapter_) { adapter_->Release(); adapter_ = nullptr; }
    if (loader_.factory) { loader_.factory->Release(); loader_.factory = nullptr; }
    shutdown_loader(loader_);

    hwnd_ = nullptr;
    initialized_ = false;
    frame_aborted_ = false;
    back_buffer_as_rt_ = false;
    clear_dirty_ = true;
    adapter_desc_[0] = '\0';
    feature_level_ = D3D_FEATURE_LEVEL_12_0;
}

void D3D12Backend::make_current(void* native_window) {
    // D3D12 无"当前上下文"。窗口句柄变了（外置/嵌入切换）时本应重建交换链，
    // 本阶段先记录，重建统一走 begin_frame 的尺寸检测路径。
    HWND hwnd = hwnd_from_native(native_window);
    if (hwnd && hwnd != hwnd_) {
        hwnd_ = hwnd;
    }
}

void D3D12Backend::release_context() {
    // 无操作：D3D12 不持有线程本地上下文。
}

// ---------------------------------------------------------------------------
// 帧循环
// ---------------------------------------------------------------------------

void D3D12Backend::begin_frame() {
    if (!initialized_) return;
    frame_aborted_ = false;
    back_buffer_as_rt_ = false;

    // 设备健康探针：GetDeviceRemovedReason() 一旦非 S_OK 就是**不可恢复**的
    // （D3D12 的 device removed 是终态，之后所有调用固定返回 0x887A0005）。
    // 每帧只在这一刻检查，能把"设备何时离开"钉到具体帧号上 —— 否则日志里
    // 满屏 0x887A0005 全是后果，看不到源头。
    if (device_ && !device_lost_) {
        const HRESULT reason = device_->GetDeviceRemovedReason();
        if (reason != S_OK) {
            device_lost_ = true;
            GLOG_ERROR("D3D12Backend: device removed at frame {} (reason=0x{:08X})",
                       frame_counter_, static_cast<uint32_t>(reason));
            dump_info_queue();
        }
    }

    // 尺寸变化检测：客户区尺寸与交换链不一致则重建。
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const uint32_t w = static_cast<uint32_t>(rc.right - rc.left);
    const uint32_t h = static_cast<uint32_t>(rc.bottom - rc.top);
    if (w > 0 && h > 0 && (w != swapchain_.width() || h != swapchain_.height())) {
        if (!swapchain_.recreate(w, h)) {
            GLOG_ERROR("D3D12Backend::begin_frame: swapchain recreate failed");
            frame_aborted_ = true;
            return;
        }
        viewport_w_ = static_cast<int>(swapchain_.width());
        viewport_h_ = static_cast<int>(swapchain_.height());
    }

    if (!swapchain_.begin_frame()) {
        GLOG_ERROR("D3D12Backend::begin_frame: swapchain begin_frame failed");
        frame_aborted_ = true;
        return;
    }
    // 帧号自增（从 1 开始，0 保留给"从未重置"）。
    ++frame_counter_;

    // ★ 延迟释放：本帧 GPU 已跑完 frames_in_flight 之前的提交，把那些
    //   已被 GPU 完成引用的待销毁资源真正释放掉（见 retire_* 的说明）。
    //   必须在录制本帧命令**之前**做 —— 这样即使槽位被本帧的 create_* 复用，
    //   也不会和仍在 GPU 上的旧资源撞车。
    drain_retired();

    // 命令列表已 Reset → 所有状态（含描述符堆绑定）都丢了，必须重新绑定。
    // D3D12 要求一次性设置两个堆（SRV/CBV/UAV + Sampler）。
    descriptor_heaps_.bind(swapchain_.command_list());

    // 上传环游标归零：本帧的 UBO / 实例数据从头开始分配。
    // （begin_frame 前 GPU 已跑完 frames_in_flight 之前的帧，复用安全。）
    upload_ring_.reset();

    // 本帧需要重新清屏 + 重新绑 RT（命令列表已 Reset，状态全丢）
    clear_dirty_ = true;
    back_buffer_as_rt_ = false;
    // 帧初的"隐含 RT"就是交换链 back buffer。夹取范围先按它设，
    // 免得引擎在任何绑定动作之前就 set_viewport/set_scissor 时拿到上一帧的尺寸。
    rt_w_ = swapchain_.width();
    rt_h_ = swapchain_.height();
}

void D3D12Backend::record_clear_if_dirty() {
    if (!clear_dirty_) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;
    clear_dirty_ = false;

    const float c[4] = { clear_r_, clear_g_, clear_b_, clear_a_ };

    // 清屏作用于**当前绑定的**渲染目标（与 GL 的 glClear 语义一致）。
    // begin_frame 只把 clear_dirty_ 置位；引擎可能在一帧里先清交换链、再绑
    // 离屏 FB 清一次，所以这里按"当下绑的是谁"决定清哪个 RTV。
    if (bound_framebuffer_.is_valid()) {
        D3D12Framebuffer* f = framebuffer_pool_.get_if_alive(bound_framebuffer_.index,
                                                            bound_framebuffer_.generation);
        if (f) {
            for (uint32_t i = 0; i < f->color_attachment_count(); ++i) {
                cmd->ClearRenderTargetView(f->color_rtv(i), c, 0, nullptr);
            }
            if (f->has_depth()) {
                cmd->ClearDepthStencilView(f->depth_dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0,
                                           nullptr);
            }
            return;
        }
        // 句柄已失效 → 退回交换链（与 bind_framebuffer 的回退规则一致）。
        bound_framebuffer_ = RHIFramebufferHandle{};
    }

    // 交换链：PRESENT → RENDER_TARGET + 绑 RT + 视口/剪裁（幂等）。
    bind_swapchain_rt();
    if (!back_buffer_as_rt_) return;
    cmd->ClearRenderTargetView(swapchain_.back_buffer_rtv(), c, 0, nullptr);
}
void D3D12Backend::end_frame() {
    if (!initialized_) return;
    if (frame_aborted_) return;

    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (cmd && back_buffer_as_rt_) {
        // RENDER_TARGET → PRESENT，Present 前必须完成
        D3D12_RESOURCE_BARRIER to_present{};
        to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_present.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        to_present.Transition.pResource = swapchain_.back_buffer();
        to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmd->ResourceBarrier(1, &to_present);
    }

    if (!swapchain_.end_frame()) {
        GLOG_ERROR("D3D12Backend::end_frame: swapchain end_frame failed");
    }

    // ---- 截图（M5.3）----
    // 放在 Present 之后：此时 back buffer 已回到 PRESENT 状态，capture_frame_rgba
    // 的 PRESENT→COPY_SOURCE→PRESENT 屏障序列是合法且自然的（与 Vulkan 的
    // PRESENT_SRC→TRANSFER_SRC 一致）。
    if (!screenshot_path_.empty() && frame_counter_ >= screenshot_frame_) {
        save_screenshot(screenshot_path_);
        screenshot_path_.clear();
    }
}

void D3D12Backend::wait_gpu_idle() {
    swapchain_.wait_all();
}

// ---------------------------------------------------------------------------
// 状态 / 绘制
// ---------------------------------------------------------------------------

void D3D12Backend::clear(float r, float g, float b, float a) {
    clear_r_ = r; clear_g_ = g; clear_b_ = b; clear_a_ = a;
    // 引擎调用顺序：begin_frame → clear(...) → 绘制。此处命令列表已 Reset 完，
    // 处于 recording 态，可以直接录制（与 GL 的"立即清屏"语义对齐）。
    record_clear_if_dirty();
}

void D3D12Backend::set_viewport(int x, int y, int w, int h) {
    viewport_x_ = x; viewport_y_ = y;
    viewport_w_ = w; viewport_h_ = h;

    // D3D12 的 viewport 是**命令列表状态**（RSSetViewports），不是全局状态 ——
    // 命令列表在 begin_frame 被 Reset 后状态全丢，所以每次 set_viewport 都要
    // 重新录制。而 GL 是立即生效的全局状态。
    if (!initialized_) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    // 夹到当前 RT 内。引擎的 viewport 尺寸来自它自己的 render target 记账，
    // 与我们实际绑定的 RT 可能不一致（尤其 bind_framebuffer 回退过的帧）；
    // 越界的 viewport/scissor 在 D3D12 上是**设备级致命错误**，不是警告。
    LONG vx = x, vy = y;
    if (rt_w_ > 0) {
        if (vx < 0) vx = 0;
        if (static_cast<uint32_t>(vx) > rt_w_) vx = static_cast<LONG>(rt_w_);
        if (w < 0) w = 0;
        const LONG max_w = static_cast<LONG>(rt_w_) - vx;
        if (w > max_w) w = max_w;
    }
    if (rt_h_ > 0) {
        if (vy < 0) vy = 0;
        if (static_cast<uint32_t>(vy) > rt_h_) vy = static_cast<LONG>(rt_h_);
        if (h < 0) h = 0;
        const LONG max_h = static_cast<LONG>(rt_h_) - vy;
        if (h > max_h) h = max_h;
    }

    D3D12_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(vx);
    vp.TopLeftY = static_cast<float>(vy);
    vp.Width = static_cast<float>(w);
    vp.Height = static_cast<float>(h);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &vp);
}

void D3D12Backend::set_scissor(int x, int y, int w, int h) {
    if (!initialized_) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    // ★ scissor 必须完全落在 RT 内，否则驱动直接把设备移除
    // （GetDeviceRemovedReason = DXGI_ERROR_INVALID_CALL → 之后满屏 0x887A0005）。
    // 这里一律夹取，绝不相信传入值。
    LONG left = x < 0 ? 0 : x;
    LONG top = y < 0 ? 0 : y;
    LONG right = w < 0 ? left : left + w;
    LONG bottom = h < 0 ? top : top + h;
    if (rt_w_ > 0) {
        if (static_cast<uint32_t>(left) > rt_w_) left = static_cast<LONG>(rt_w_);
        if (static_cast<uint32_t>(right) > rt_w_) right = static_cast<LONG>(rt_w_);
    }
    if (rt_h_ > 0) {
        if (static_cast<uint32_t>(top) > rt_h_) top = static_cast<LONG>(rt_h_);
        if (static_cast<uint32_t>(bottom) > rt_h_) bottom = static_cast<LONG>(rt_h_);
    }
    if (right < left) right = left;
    if (bottom < top) bottom = top;

    D3D12_RECT sr{};
    sr.left = left;
    sr.top = top;
    sr.right = right;
    sr.bottom = bottom;
    cmd->RSSetScissorRects(1, &sr);
}

void D3D12Backend::clear_depth() {
    if (!initialized_) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    // 深度清除只能在已绑定的 DSV 上做。绑了离屏帧缓冲时清它自己的深度附件；
    // 否则交换链本身没有深度缓冲（M2 的脚手架上交换链只有颜色），此时无操作。
    if (bound_framebuffer_.is_valid()) {
        D3D12Framebuffer* f = framebuffer_pool_.get_if_alive(bound_framebuffer_.index,
                                                            bound_framebuffer_.generation);
        if (f && f->has_depth()) {
            cmd->ClearDepthStencilView(f->depth_dsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0,
                                       nullptr);
        }
    }
}

void D3D12Backend::set_depth_test(bool enabled) { depth_test_ = enabled; }
void D3D12Backend::set_depth_write(bool enabled) { depth_write_ = enabled; }
void D3D12Backend::set_blend(bool enabled) { blend_ = enabled; }
void D3D12Backend::set_blend_func(BlendFactor src_factor, BlendFactor dst_factor) {
    blend_src_ = src_factor;
    blend_dst_ = dst_factor;
}
void D3D12Backend::set_blend_equation(BlendEquation mode) { blend_equation_ = mode; }
void D3D12Backend::set_cull_face(CullMode mode) { cull_ = mode; }

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
// D3D12 的一次 draw 需要把这些全部就位（缺一不可，且顺序有讲究）：
//   1. 描述符堆（begin_frame 已绑，但 draw 前再确认无害）
//   2. root signature
//   3. PSO（按当前动态状态 + 顶点布局指纹选取/创建）
//   4. 顶点/索引缓冲（来自 mesh）
//   5. 根参数：CBV（uniform 块）+ SRV 表 + Sampler 表
//   6. 真正的 DrawIndexedInstanced / DrawInstanced
//
// 引擎的 draw_mesh/draw_indexed 签名只有 (mesh, shader)，不带"实例数"——
// 实例数在 mesh 内部（instance_count()）。所以这里按 mesh 的实例数选择
// Instanced 还是非 Instanced 调用。
void D3D12Backend::draw_indexed(RHIMeshHandle mesh_handle, RHIShaderHandle shader_handle) {
    if (!initialized_ || frame_aborted_) return;

    D3D12Mesh* m = mesh_pool_.get_if_alive(mesh_handle.index, mesh_handle.generation);
    D3D12Shader* s = shader_pool_.get_if_alive(shader_handle.index, shader_handle.generation);
    if (!m || !s || !s->is_valid()) {
        if (!s) {
            GLOG_WARN("D3D12::draw_indexed: invalid shader handle");
        }
        return;
    }

    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    // ★ 这里**不能**做每帧重置。引擎一帧的顺序是"先 dispatch 全部状态设置，
    //   最后才 dispatch draw"，在 draw 时复位会把本帧刚设好的 uniform 与纹理
    //   绑定整块擦掉（症状 = 整帧纯黑）。复位已改到 shader 状态入口的
    //   ensure_frame()；draw 路径只读状态。见 d3d12_shader.cpp 的 ensure_frame。
    s->ensure_frame();

    // mesh 必须在 bind() 之前把顶点/索引缓冲准备好；D3D12Mesh::bind() 记录的是
    // 内部状态（真正 IASetVertexBuffers 在下面按 view 做）。
    const D3D12_VERTEX_BUFFER_VIEW vb = m->vertex_view();
    const D3D12_INDEX_BUFFER_VIEW ib = m->index_view();
    if (ib.BufferLocation == 0) {
        GLOG_WARN("D3D12::draw_indexed: mesh has no index buffer");
        return;
    }

    // ---- PSO 选择 ----
    // 顶点布局的**唯一真相来源是 mesh 本身**：IMesh::set_layout 由 pipeline 直接
    // 调在 mesh 上（见 render_pipeline.cpp:1523 等），后端看不到那个调用。所以
    // 每次 draw 从 mesh 取当前布局构造 PSO 键 —— 不做任何后端侧缓存。
    if (!m->has_layout()) {
        GLOG_WARN("D3D12::draw_indexed: mesh has no vertex layout; skipping");
        return;
    }

    PsoKey key;
    key.layout = make_layout_key(m->layout(), m->instance_layout());
    key.rt_format = static_cast<uint32_t>(current_rt_format_);
    key.ds_format = static_cast<uint32_t>(current_ds_format_);
    key.depth_test = depth_test_ ? 1 : 0;
    // skybox 管线不写深度（与 GL/Vulkan 的 skybox 约定一致）。
    key.depth_write = (s->skybox() ? false : depth_write_) ? 1 : 0;
    key.blend = blend_ ? (s->blending() ? 1 : 0) : 0;
    key.cull = (cull_ == CullMode::None) ? 1 : (cull_ == CullMode::Front ? 2 : 0);
    key.primitive_topology = 0;   // triangle
    key.instance_stream = (m->instance_count() > 0) ? 1 : 0;

    ID3D12PipelineState* pso = s->pipeline_for(key);
    if (!pso) {
        GLOG_ERROR("D3D12::draw_indexed: no pipeline for this state/layout");
        return;
    }

    cmd->SetGraphicsRootSignature(s->root_signature());
    cmd->SetPipelineState(pso);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &vb);
    cmd->IASetIndexBuffer(&ib);

    // ---- 根参数 ----
    // 参数 0 = CBV(register b0, 可见性 VERTEX)，参数 1 = CBV(register b0, PIXEL)。
    // 两个阶段的 Globals 块布局不同（见 d3d12_shader.h 顶部说明），所以各有
    // 一份上传区间；SRV/Sampler 表索引随"是否存在"浮动，用 shader 的访问器取。
    const D3D12Shader::UniformCbv cb = s->commit_uniforms();
    if (cb.vs != 0) cmd->SetGraphicsRootConstantBufferView(0, cb.vs);
    if (cb.ps != 0) cmd->SetGraphicsRootConstantBufferView(1, cb.ps);
    if (s->has_textures()) {
        s->commit_textures();
        if (s->has_srv_table()) {
            cmd->SetGraphicsRootDescriptorTable(s->root_param_srv(), s->srv_table());
        }
        if (s->has_sampler_table()) {
            cmd->SetGraphicsRootDescriptorTable(s->root_param_sampler(), s->sampler_table());
        }
    }

    const uint32_t instances = m->instance_count() > 0 ? m->instance_count() : 1;
    cmd->DrawIndexedInstanced(m->index_count(), instances, 0, 0, 0);
}

void D3D12Backend::draw_mesh(RHIMeshHandle mesh_handle, RHIShaderHandle shader_handle) {
    if (!initialized_ || frame_aborted_) return;

    D3D12Mesh* m = mesh_pool_.get_if_alive(mesh_handle.index, mesh_handle.generation);
    D3D12Shader* s = shader_pool_.get_if_alive(shader_handle.index, shader_handle.generation);
    if (!m || !s || !s->is_valid()) return;

    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    // 同 draw_indexed：draw 路径只读状态，不要在这里重置（见上方说明）。
    s->ensure_frame();

    const D3D12_VERTEX_BUFFER_VIEW vb = m->vertex_view();
    const D3D12_INDEX_BUFFER_VIEW ib = m->index_view();
    if (vb.BufferLocation == 0) {
        GLOG_WARN("D3D12::draw_mesh: mesh has no vertex buffer");
        return;
    }
    if (!m->has_layout()) {
        GLOG_WARN("D3D12::draw_mesh: mesh has no vertex layout; skipping");
        return;
    }

    PsoKey key;
    key.layout = make_layout_key(m->layout(), m->instance_layout());
    key.rt_format = static_cast<uint32_t>(current_rt_format_);
    key.ds_format = static_cast<uint32_t>(current_ds_format_);
    key.depth_test = depth_test_ ? 1 : 0;
    key.depth_write = (s->skybox() ? false : depth_write_) ? 1 : 0;
    key.blend = blend_ ? (s->blending() ? 1 : 0) : 0;
    key.cull = (cull_ == CullMode::None) ? 1 : (cull_ == CullMode::Front ? 2 : 0);
    key.primitive_topology = 0;
    key.instance_stream = (m->instance_count() > 0) ? 1 : 0;

    ID3D12PipelineState* pso = s->pipeline_for(key);
    if (!pso) {
        GLOG_ERROR("D3D12::draw_mesh: no pipeline for this state/layout");
        return;
    }

    cmd->SetGraphicsRootSignature(s->root_signature());
    cmd->SetPipelineState(pso);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &vb);

    // ★ draw_mesh 的语义 = "把网格整个画出来"，**有索引就走索引**。
    //   对齐 GLMesh::draw()（gl_buffer.cpp:272）：
    //     有索引 → glDrawElements[_Instanced](index_count)
    //     无索引 → glDrawArrays[_Instanced](vertex_count)
    //
    //   早先这里只调 DrawIndexedInstanced(index_count) 却**从未 IASetIndexBuffer**，
    //   于是 GPU 去读一段从未绑定（或上一帧残留）的索引缓冲 → 索引值全是垃圾 →
    //   顶点取址飞出去 → GPU 挂死、设备被移除
    //   （GetDeviceRemovedReason = DXGI_ERROR_DEVICE_HUNG 0x887A0006）。
    //   引擎所有绘制都走 draw_mesh（render_pipeline.cpp 里几十处），所以这是
    //   "第一帧就整个崩掉"的直接原因。
    const bool indexed = ib.BufferLocation != 0 && m->index_count() > 0;
    if (indexed) {
        cmd->IASetIndexBuffer(&ib);
    }

    const D3D12Shader::UniformCbv cb = s->commit_uniforms();
    if (cb.vs != 0) cmd->SetGraphicsRootConstantBufferView(0, cb.vs);
    if (cb.ps != 0) cmd->SetGraphicsRootConstantBufferView(1, cb.ps);
    if (s->has_textures()) {
        s->commit_textures();
        if (s->has_srv_table()) {
            cmd->SetGraphicsRootDescriptorTable(s->root_param_srv(), s->srv_table());
        }
        if (s->has_sampler_table()) {
            cmd->SetGraphicsRootDescriptorTable(s->root_param_sampler(), s->sampler_table());
        }
    }


    const uint32_t instances = m->instance_count() > 0 ? m->instance_count() : 1;
    if (indexed) {
        cmd->DrawIndexedInstanced(m->index_count(), instances, 0, 0, 0);
    } else {
        cmd->DrawInstanced(m->vertex_count(), instances, 0, 0);
    }
}

void D3D12Backend::set_swap_interval(int interval) {
    // sync interval 是 Present(interval, flags) 的参数，改它不需要重建交换链 ——
    // 记到 swapchain 上，下一帧 Present 生效（比 Vulkan 少一次 recreate）。
    swapchain_.set_vsync_enabled(interval != 0);
}

// ---------------------------------------------------------------------------
// 截图 / 帧回读（M5.3）
// ---------------------------------------------------------------------------
namespace {

bool d3d12_path_ends_with(const std::string& path, const char* ext) {
    const size_t n = std::strlen(ext);
    return path.size() >= n && path.compare(path.size() - n, n, ext) == 0;
}

// 极简 BMP 写盘（24bpp bottom-up）。入参是 RGBA 行主序，写盘时按 BMP 约定
// 转成 BGR —— 与 GL/Vulkan 后端的 save_bgr_bmp 同构，避免为截图引入新依赖。
void save_bmp_from_rgba(const std::string& path, const uint8_t* rgba, uint32_t width,
                        uint32_t height) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        GLOG_ERROR("D3D12 save_bmp: failed to open '{}'", path);
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
        const unsigned char* src = rgba + static_cast<std::size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        file.write(reinterpret_cast<char*>(row.data()), row_size);
    }
}

} // namespace

void D3D12Backend::request_screenshot(const std::string& path) {
    // 与 GL/Vulkan 同构：请求发生在帧中间，立刻回读拿到的是上一帧内容，
    // 所以记一个"下一帧"的目标帧号，由 end_frame 触发。
    screenshot_path_ = path;
    screenshot_frame_ = frame_counter_ + 1;
}

bool D3D12Backend::capture_frame_rgba(std::vector<uint8_t>& out, int& width, int& height) {
    if (!initialized_ || frame_aborted_ || !device_ || !queue_) return false;

    ID3D12Resource* back = swapchain_.back_buffer();
    if (!back) {
        GLOG_ERROR("D3D12Backend::capture_frame_rgba: swapchain back buffer is null");
        return false;
    }
    const uint32_t w = swapchain_.width();
    const uint32_t h = swapchain_.height();
    if (w == 0 || h == 0) return false;

    // ---- 回读缓冲的尺寸**不能自己猜** ----
    // D3D12 的纹理拷贝行距必须 256 对齐，所以缓冲要按 GetCopyableFootprints
    // 给出的 footprint（行距 + 总字节）分配。用 w*4*h 在宽度非 256 倍数时
    // 会被校验拒绝（或读到错位数据）。
    const D3D12_RESOURCE_DESC tex_desc = back->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total = 0;
    device_->GetCopyableFootprints(&tex_desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);
    if (total == 0 || layout.Footprint.RowPitch == 0) {
        GLOG_ERROR("D3D12Backend::capture_frame_rgba: GetCopyableFootprints returned 0");
        return false;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    hp.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* readback = nullptr;
    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  __uuidof(ID3D12Resource),
                                                  reinterpret_cast<void**>(&readback));
    if (FAILED(hr) || !readback) {
        GLOG_ERROR("D3D12Backend::capture_frame_rgba: CreateCommittedResource(readback) "
                   "failed 0x{:08X} ({} bytes)", static_cast<uint32_t>(hr), total);
        return false;
    }

    // ---- 临时命令分配器/列表（截图是低频操作，用即建即弃最简单）----
    ID3D12CommandAllocator* alloc = nullptr;
    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         __uuidof(ID3D12CommandAllocator),
                                         reinterpret_cast<void**>(&alloc));
    ID3D12GraphicsCommandList* cl = nullptr;
    if (SUCCEEDED(hr) && alloc) {
        hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                        __uuidof(ID3D12GraphicsCommandList),
                                        reinterpret_cast<void**>(&cl));
    }
    if (FAILED(hr) || !cl) {
        GLOG_ERROR("D3D12Backend::capture_frame_rgba: command objects failed 0x{:08X}",
                   static_cast<uint32_t>(hr));
        if (alloc) alloc->Release();
        readback->Release();
        return false;
    }
    // ★★ 这里**不能** Close()。★★
    //
    // CreateCommandList 返回的命令列表已经处于 **recording** 状态，可以直接录制。
    //（"必须先 Close 才能 Reset" 那条规则只在你要**复用**同一个列表时才适用；
    //  本函数是即建即弃，新建的 allocator 也是初始态，根本不需要 Reset。）
    //
    // 早先这里照搬了上面的经验写了 `cl->Close()`，于是紧接着的 transition /
    // CopyTextureRegion 全部录制进了一个**已关闭**的列表 —— 调用本身不报错
    // （返回 void），但最后那次 Close() 返回 E_FAIL，整段命令作废、GPU 什么都不做，
    // 回读缓冲保持全 0。表现就是"截图永远纯黑"，前面几十分钟的排查都在追一个
    // 根本不存在的渲染问题。
    //
    // 结论：保留 recording 态，直接开始录制。

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cl->ResourceBarrier(1, &b);
    };

    // 交换链的 back buffer 在本函数被调用的时点（帧末 Present 之后）处于
    // PRESENT 态 —— 不能指望隐式提升，必须显式切到 COPY_SOURCE。
    transition(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    // ★ 纹理 → 缓冲必须用 CopyTextureRegion + 显式 footprint，
    //   **不能用 CopyResource**：CopyResource 要求源与目标的描述完全一致
    //   （同类型、同尺寸、同格式），而这里是 2D 纹理 → 缓冲区，属于非法组合。
    //   实测该调用不报错也不移除设备，只是**静默什么都不拷**，回读缓冲保持全 0
    //   —— 表现就是"截图永远全黑"，让人误判成渲染管线没生效（本会话为此绕了很久）。
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layout;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = back;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    cl->CopyTextureRegion(&dst, 0, 0, 0, &src_loc, nullptr);
    transition(back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);

    // ★ D3D12 的录制校验是**延迟**的：非法录制不在调用处报错（那些调用返回 void），
    //   而是在 Close() 时以 E_INVALIDARG / E_FAIL 报出来。不检查 Close() 的返回值，
    //   整个命令列表会被静默丢弃，表现就是"拷贝什么也没做、回读缓冲保持全 0"
    //   —— 即"截图永远全黑"。所以这里必须查。
    const HRESULT close_hr = cl->Close();
    if (FAILED(close_hr)) {
        GLOG_ERROR("D3D12Backend::capture_frame_rgba: command list Close failed 0x{:08X} "
                   "(录制非法，回读内容无效)", static_cast<uint32_t>(close_hr));
    }
    ID3D12CommandList* lists[] = { cl };
    queue_->ExecuteCommandLists(1, lists);
    // 等 GPU 跑完才能 Map。swapchain_.wait_all() 就是"signal 一个新 fence 值 + 等它"，
    // 而队列是 FIFO，所以它等价于"等此前所有提交完成"。
    swapchain_.wait_all();

    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total)};
    hr = readback->Map(0, &read_range, &mapped);
    if (FAILED(hr) || !mapped) {
        GLOG_ERROR("D3D12Backend::capture_frame_rgba: Map failed 0x{:08X}",
                   static_cast<uint32_t>(hr));
        cl->Release();
        alloc->Release();
        readback->Release();
        return false;
    }

    // 按 footprint 的行距逐行拆包（行距 ≥ w*4，多出来的是对齐填充）。
    out.resize(static_cast<size_t>(w) * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mapped);
    const uint32_t row_pitch = layout.Footprint.RowPitch;
    const uint32_t copy_bytes = w * 4;
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * copy_bytes,
                    src + static_cast<size_t>(y) * row_pitch, copy_bytes);
    }
    // 回读堆不需要写回 CPU 侧的修改。
    const D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);

    // back buffer 是交换链的资源，**不能**释放；只释放我们自己的三件套。
    cl->Release();
    alloc->Release();
    readback->Release();

    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

void D3D12Backend::save_screenshot(const std::string& path) {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    if (!capture_frame_rgba(rgba, width, height)) {
        GLOG_ERROR("D3D12Backend: failed to capture framebuffer for screenshot '{}'", path);
        return;
    }
    if (d3d12_path_ends_with(path, ".png")) {
        if (stbi_write_png(path.c_str(), width, height, 4, rgba.data(), width * 4)) {
            GLOG_INFO("D3D12Backend: screenshot saved to '{}' ({}x{} PNG)", path, width, height);
        } else {
            GLOG_ERROR("D3D12Backend: stbi_write_png failed for '{}'", path);
        }
    } else {
        save_bmp_from_rgba(path, rgba.data(), static_cast<uint32_t>(width),
                           static_cast<uint32_t>(height));
        GLOG_INFO("D3D12Backend: screenshot saved to '{}' ({}x{} BMP)", path, width, height);
    }
}

// ---------------------------------------------------------------------------
// 资源工厂（M3 已接线）
// ---------------------------------------------------------------------------
// 每个工厂从对应池里分配一个对象，并把设备上下文注入进去（D3D12 的资源必须
// 在创建时就知道设备，而 RHIResourcePool::allocate 支持转发构造参数 —— 这里
// 用"先构造再 set_context"是因为 D3D12Texture/Mesh 等把 device_ 设为可后置注入，
// 便于池复用（deallocate 后对象析构，下次 allocate 会重新构造）。

RHIMeshHandle D3D12Backend::create_mesh() {
    const uint32_t index = mesh_pool_.allocate();
    D3D12Mesh* m = mesh_pool_.get(index);
    if (m) m->set_context(device_, queue_);
    return { index, mesh_pool_.generation(index) };
}

RHIShaderHandle D3D12Backend::create_shader() {
    const uint32_t index = shader_pool_.allocate();
    D3D12Shader* s = shader_pool_.get(index);
    if (s) s->set_context(this, device_);
    return { index, shader_pool_.generation(index) };
}

RHITextureHandle D3D12Backend::create_texture() {
    const uint32_t index = texture_pool_.allocate();
    D3D12Texture* t = texture_pool_.get(index);
    if (t) t->set_context(device_, queue_);
    return { index, texture_pool_.generation(index) };
}

RHIFramebufferHandle D3D12Backend::create_framebuffer() {
    const uint32_t index = framebuffer_pool_.allocate();
    D3D12Framebuffer* f = framebuffer_pool_.get(index);
    if (f) f->set_context(device_, queue_);
    return { index, framebuffer_pool_.generation(index) };
}

RHIBufferHandle D3D12Backend::create_buffer() {
    const uint32_t index = buffer_pool_.allocate();
    D3D12Buffer* b = buffer_pool_.get(index);
    if (b) b->set_context(device_, queue_);
    return { index, buffer_pool_.generation(index) };
}

// ---------------------------------------------------------------------------
// 延迟释放（D3D12 资源生命周期）
// ---------------------------------------------------------------------------
// 为什么不能立刻 deallocate：命令列表是**录制 → 提交 → 异步执行**的。引擎里
// 一帧的录制发生在 CPU 上，此时上一帧（甚至上上帧）的命令可能仍在 GPU 上跑。
// 若某资源在本帧录制期间被销毁（最典型：effect 的离屏目标在 resize / SSR
// resolution_scale 变化时被 create_targets 重建），它可能仍被**已提交未完成**
// 的命令列表引用 → 调试层报
//   "An ID3D12Resource object ... was deleted prior to executing the command list.
//    This is invalid and can result in application instability."
// 实测后果是连锁的 DXGI_ERROR_DEVICE_HUNG（TDR）。
//
// 做法：销毁时先记下"当前已交给 GPU 的围栏值"，把槽位挂进 retire 队列；等到
// GPU 的已完成围栏值追过它，才真正释放。三帧在途 → 最多滞留 3 帧，代价可忽略。
// ---------------------------------------------------------------------------
void D3D12Backend::retire_texture(uint32_t index, uint32_t generation) {
    retired_textures_.push_back({swapchain_.last_submitted_value(), index, generation});
}
void D3D12Backend::retire_framebuffer(uint32_t index, uint32_t generation) {
    retired_framebuffers_.push_back({swapchain_.last_submitted_value(), index, generation});
}
void D3D12Backend::retire_mesh(uint32_t index, uint32_t generation) {
    retired_meshes_.push_back({swapchain_.last_submitted_value(), index, generation});
}
void D3D12Backend::retire_buffer(uint32_t index, uint32_t generation) {
    retired_buffers_.push_back({swapchain_.last_submitted_value(), index, generation});
}

void D3D12Backend::drain_retired() {
    const uint64_t done = swapchain_.completed_value();

    auto drain = [done](std::vector<RetiredResource>& q, auto&& release_fn) {
        // 队列很短（≤ 几帧的销毁量），直接原地筛选。
        size_t keep = 0;
        for (size_t i = 0; i < q.size(); ++i) {
            if (q[i].fence_value <= done) {
                release_fn(q[i].index, q[i].generation);
            } else {
                if (keep != i) q[keep] = q[i];
                ++keep;
            }
        }
        q.resize(keep);
    };

    drain(retired_textures_, [this](uint32_t i, uint32_t g) { texture_pool_.deallocate(i, g); });
    drain(retired_framebuffers_,
          [this](uint32_t i, uint32_t g) { framebuffer_pool_.deallocate(i, g); });
    drain(retired_meshes_, [this](uint32_t i, uint32_t g) {
        // mesh 的 GPU 释放函数（释放顶点/索引 buffer 的引用）必须在这里补上，
        // 因为 destroy_mesh 已经不再直接调用 release()。
        D3D12Mesh* m = mesh_pool_.get_if_alive(i, g);
        if (m) m->release();
        mesh_pool_.deallocate(i, g);
    });
    drain(retired_buffers_, [this](uint32_t i, uint32_t g) { buffer_pool_.deallocate(i, g); });

    // 描述符区间归还（见 retire_descriptor_range 的说明）。
    {
        DescriptorHeapSet& heaps = descriptor_heaps_;
        size_t keep = 0;
        for (size_t i = 0; i < retired_descriptors_.size(); ++i) {
            const RetiredDescriptors& r = retired_descriptors_[i];
            if (r.fence_value <= done) {
                heaps.srv().free_range(r.srv_base, r.srv_count);
                heaps.sampler().free_range(r.smp_base, r.smp_count);
            } else {
                if (keep != i) retired_descriptors_[keep] = r;
                ++keep;
            }
        }
        retired_descriptors_.resize(keep);
    }
}

void D3D12Backend::retire_descriptor_range(uint32_t srv_base, uint32_t srv_count,
                                           uint32_t smp_base, uint32_t smp_count) {
    if (srv_count == 0 && smp_count == 0) return;
    // 挂在"最近一次已提交"的围栏值上：GPU 跑过它，就说明引用这些描述符的
    // 命令列表已经执行完，可以安全归还。
    RetiredDescriptors r{};
    r.fence_value = swapchain_.last_submitted_value();
    r.srv_base = srv_base;
    r.srv_count = srv_count;
    r.smp_base = smp_base;
    r.smp_count = smp_count;
    retired_descriptors_.push_back(r);
}

// 销毁：用带 generation 校验的 deallocate —— 旧句柄二次销毁不会误杀复用槽位。
void D3D12Backend::destroy_mesh(RHIMeshHandle handle) {
    if (!handle.is_valid()) return;
    retire_mesh(handle.index, handle.generation);
}

void D3D12Backend::destroy_shader(RHIShaderHandle handle) {
    if (!handle.is_valid()) return;
    shader_pool_.deallocate(handle.index, handle.generation);
}

void D3D12Backend::destroy_texture(RHITextureHandle handle) {
    if (!handle.is_valid()) return;
    retire_texture(handle.index, handle.generation);
}

void D3D12Backend::destroy_framebuffer(RHIFramebufferHandle handle) {
    if (!handle.is_valid()) return;
    // 若正绑定的帧缓冲被销毁，先解绑，避免悬空引用。
    if (bound_framebuffer_.index == handle.index &&
        bound_framebuffer_.generation == handle.generation) {
        bound_framebuffer_ = RHIFramebufferHandle{};
    }
    retire_framebuffer(handle.index, handle.generation);
}

void D3D12Backend::destroy_buffer(RHIBufferHandle handle) {
    if (!handle.is_valid()) return;
    retire_buffer(handle.index, handle.generation);
}

IMesh* D3D12Backend::mesh(RHIMeshHandle handle) {
    return mesh_pool_.get_if_alive(handle.index, handle.generation);
}

IShader* D3D12Backend::shader(RHIShaderHandle handle) {
    return shader_pool_.get_if_alive(handle.index, handle.generation);
}

ITexture* D3D12Backend::texture(RHITextureHandle handle) {
    return texture_pool_.get_if_alive(handle.index, handle.generation);
}

IFramebuffer* D3D12Backend::framebuffer(RHIFramebufferHandle handle) {
    return framebuffer_pool_.get_if_alive(handle.index, handle.generation);
}

IBuffer* D3D12Backend::buffer(RHIBufferHandle handle) {
    return buffer_pool_.get_if_alive(handle.index, handle.generation);
}

// ---------------------------------------------------------------------------
// 帧缓冲绑定
// ---------------------------------------------------------------------------
// 把 ID3D12InfoQueue 里累积的校验消息打进日志。
// 只有开了调试层（GRYCE_D3D12_DEBUG=1）才有内容 —— 但一旦有，它就是
// "哪条 API 调用非法"的直接答案，比任何事后推断都准。
void D3D12Backend::dump_info_queue() {
    if (!device_) return;
    ID3D12InfoQueue* iq = nullptr;
    if (FAILED(device_->QueryInterface(__mingw_uuidof<ID3D12InfoQueue>(),
                                       reinterpret_cast<void**>(&iq))) ||
        !iq) {
        return;
    }
    const UINT64 count = iq->GetNumStoredMessages();
    GLOG_ERROR("D3D12 info queue: {} stored message(s)", static_cast<unsigned long long>(count));
    // 打最后 120 条：设备移除前的尾部消息才是关键（前面多是资源创建噪声）。
    const UINT64 begin = count > 120 ? count - 120 : 0;
    for (UINT64 i = begin; i < count; ++i) {
        SIZE_T len = 0;
        if (FAILED(iq->GetMessage(i, nullptr, &len)) || len == 0) continue;
        std::vector<char> buf(len);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(iq->GetMessage(i, msg, &len))) continue;
        const char* sev = "INFO";
        switch (msg->Severity) {
            case D3D12_MESSAGE_SEVERITY_CORRUPTION: sev = "CORRUPTION"; break;
            case D3D12_MESSAGE_SEVERITY_ERROR:      sev = "ERROR"; break;
            case D3D12_MESSAGE_SEVERITY_WARNING:    sev = "WARNING"; break;
            default: break;
        }
        GLOG_ERROR("D3D12 info[{}] {}: {}", static_cast<unsigned long long>(i), sev,
                   std::string(msg->pDescription, msg->DescriptionByteLength));
    }
    iq->Release();
}

void D3D12Backend::bind_swapchain_rt() {
    if (!initialized_) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;
    if (!swapchain_.back_buffer()) return;

    // 已经就是当前 RT 且已在 RT 态 → 什么都不做。**不能重复录 PRESENT→RT 的
    // 屏障**（StateBefore 与实际不符会让调试层报错，驱动侧也可能被判非法）。
    if (back_buffer_as_rt_) return;

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = swapchain_.back_buffer();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    cmd->ResourceBarrier(1, &b);
    back_buffer_as_rt_ = true;

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_.back_buffer_rtv();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    rt_w_ = swapchain_.width();
    rt_h_ = swapchain_.height();

    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(rt_w_);
    vp.Height = static_cast<float>(rt_h_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &vp);
    D3D12_RECT sr{};
    sr.right = static_cast<LONG>(rt_w_);
    sr.bottom = static_cast<LONG>(rt_h_);
    cmd->RSSetScissorRects(1, &sr);

    bound_framebuffer_ = RHIFramebufferHandle{};
    viewport_x_ = 0;
    viewport_y_ = 0;
    viewport_w_ = static_cast<int>(rt_w_);
    viewport_h_ = static_cast<int>(rt_h_);
    // 切回交换链：颜色格式是 R8G8B8A8_UNORM（见 d3d12_swapchain.cpp:59），
    // 交换链脚手架目前没有深度附件。
    current_rt_format_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    current_ds_format_ = DXGI_FORMAT_UNKNOWN;
}

void D3D12Backend::bind_framebuffer(RHIFramebufferHandle fb) {
    if (!initialized_) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    D3D12Framebuffer* f = framebuffer_pool_.get_if_alive(fb.index, fb.generation);

    // ★ 与 GL 后端同语义（gl_backend.cpp:364）：句柄解析不到就**切回默认帧缓冲**
    // （交换链 back buffer），而不是直接 return。
    //
    // 早先这里 return 是个隐蔽的致命 bug：引擎用 bind_framebuffer({0,0}) 表示
    // "回到屏幕"，此时若保留了上一帧绑定的离屏 RT（例如 960x540 的水面反射目标），
    // 紧接着 set_viewport/set_scissor(0,0,1920,1080) 就会给一个 960x540 的 RT
    // 设 1920x1080 的 scissor —— scissor 必须完全落在 RT 内，越界直接触发
    // GetDeviceRemovedReason() = DXGI_ERROR_INVALID_CALL，设备永久失效
    // （日志里满是 0x887A0005，看起来像"随机崩溃"）。修复后这个分支再也不会
    // 把尺寸不匹配的 RT 留在原地。
    if (!f || !f->is_complete()) {
        GLOG_WARN("D3D12::bind_framebuffer: framebuffer not usable (idx={} gen={} ptr={} "
                  "colors={} depth={}) — falling back to the swapchain back buffer",
                  fb.index, fb.generation, static_cast<const void*>(f),
                  f ? f->color_attachment_count() : 0u, (f && f->has_depth()) ? 1 : 0);
        unbind_framebuffer();
        return;
    }

    // ★ 切到**另一个**离屏帧缓冲时，必须先把上一个的附件转到"shader 可读"。
    //   D3D12 没有 render pass 对象，布局过渡全靠手动；Vulkan 那边每次
    //   bind_framebuffer 都会 end_current_render_pass() 来隐式完成这件事，
    //   这里必须显式补上。漏掉的后果：上一个 pass 写出的颜色/深度仍停在
    //   RENDER_TARGET / DEPTH_WRITE 态，被下一个 pass 当纹理采样 → 读到未定义
    //   内容（Intel 上是 0）→ 表现为"后面的 pass 全黑"。
    //   （这条路径 unbind_framebuffer 覆盖不到：FB→FB 直接切换不会经过它。）
    if (bound_framebuffer_.is_valid() &&
        (bound_framebuffer_.index != fb.index ||
         bound_framebuffer_.generation != fb.generation)) {
        D3D12Framebuffer* prev = framebuffer_pool_.get_if_alive(bound_framebuffer_.index,
                                                               bound_framebuffer_.generation);
        if (prev) prev->record_transition_to_shader_readable(cmd);
        bound_framebuffer_ = RHIFramebufferHandle{};
    }

    // 从交换链切到离屏：先把 back buffer 的状态切回 PRESENT（如果它作为 RT 被用过），
    // 否则本帧 end_frame 的 RT→PRESENT 屏障会与实际状态不符。
    if (back_buffer_as_rt_) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = swapchain_.back_buffer();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmd->ResourceBarrier(1, &b);
        back_buffer_as_rt_ = false;
    }

    f->record_transition_to_rt(cmd);

    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
    const uint32_t n = f->color_attachment_count();
    rtvs.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        rtvs.push_back(f->color_rtv(i));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = f->has_depth() ? f->depth_dsv()
                                                     : D3D12_CPU_DESCRIPTOR_HANDLE{};
    cmd->OMSetRenderTargets(static_cast<UINT>(rtvs.size()),
                            rtvs.empty() ? nullptr : rtvs.data(),
                            f->has_depth() ? TRUE : FALSE,
                            f->has_depth() ? &dsv : nullptr);

    rt_w_ = static_cast<uint32_t>(f->width());
    rt_h_ = static_cast<uint32_t>(f->height());

    // 视口/剪裁按帧缓冲尺寸设置。
    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(rt_w_);
    vp.Height = static_cast<float>(rt_h_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &vp);
    D3D12_RECT sr{};
    sr.right = static_cast<LONG>(rt_w_);
    sr.bottom = static_cast<LONG>(rt_h_);
    cmd->RSSetScissorRects(1, &sr);

    bound_framebuffer_ = fb;
    viewport_x_ = 0;
    viewport_y_ = 0;
    viewport_w_ = static_cast<int>(rt_w_);
    viewport_h_ = static_cast<int>(rt_h_);
    // 记录 RT 格式：PSO 把它们编译进去，换 RT 必须换 PSO。
    current_rt_format_ = f->color_format();
    current_ds_format_ = f->has_depth() ? f->depth_format() : DXGI_FORMAT_UNKNOWN;

}

void D3D12Backend::unbind_framebuffer() {
    if (!initialized_) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    // 离屏结果转为 shader 可读（后续 pass 要采样它）。
    if (bound_framebuffer_.is_valid()) {
        D3D12Framebuffer* f = framebuffer_pool_.get_if_alive(bound_framebuffer_.index,
                                                            bound_framebuffer_.generation);
        if (f) {
            f->record_transition_to_shader_readable(cmd);
        }
    }

    // 无论此前是否绑定过离屏 FB，都保证最终落在交换链 back buffer 上。
    // （bind_swapchain_rt 自带幂等判断，已是当前 RT 时不会重复录屏障。）
    bind_swapchain_rt();
}

// ---------------------------------------------------------------------------
// 元信息
// ---------------------------------------------------------------------------

std::unique_ptr<IRenderer2D> D3D12Backend::create_renderer2d() {
    return std::make_unique<D3D12Renderer2D>(this);
}

std::unique_ptr<IImGuiBackend> D3D12Backend::create_imgui_backend() {
#ifdef GRYCE_D3D12_HAS_IMGUI
    return std::make_unique<D3D12ImGuiBackend>(this);
#else
    return nullptr;  // GRYCE_ENABLE_IMGUI=OFF 时无 ImGui 核心源，后端不可用。
#endif
}

// ImGui 绘制时会用 SetDescriptorHeaps 把它的专用 SRV 堆换成当前绑定，
// 绘制完必须换回引擎主堆（D3D12 同一时刻只认一个 shader-visible SRV 堆）。
void D3D12Backend::rebind_descriptor_heaps() {
    if (!initialized_ || device_lost_) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;
    descriptor_heaps_.bind(cmd);
}

void D3D12Backend::ensure_texture_shader_readable(D3D12Texture* tex) {
    if (!initialized_ || device_lost_ || !tex) return;
    ID3D12Resource* res = tex->resource();
    if (!res) return;
    const D3D12_RESOURCE_STATES target =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES before = tex->current_state();
    if (before == target) return;

    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    // 采样一张仍在 RENDER_TARGET / DEPTH_WRITE / COPY_DEST 态的资源是 D3D12 的
    // 非法用法（Vulkan 由 render pass 的 finalLayout 兜底，D3D12 没有等价机制）。
    // 实测症状：调试层可能只给一条被环形缓冲冲掉的警告，真机上 Intel Arc 直接
    // GPU 挂起 → DXGI_ERROR_DEVICE_HUNG。这里统一在"当 SRV 绑定"时补屏障。
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = target;
    cmd->ResourceBarrier(1, &b);
    tex->set_current_state(target);
}

const char* D3D12Backend::api_name() const { return "D3D12"; }
const char* D3D12Backend::api_version() const { return adapter_desc_; }

RenderBackendCapabilities D3D12Backend::get_capabilities() const {
    // M3 阶段：交换链 / clear / present / 资源层（buffer/texture/mesh/framebuffer）
    // 已可用；着色器与管线状态要等 M4。
    RenderBackendCapabilities caps{};
    caps.supports_compute              = false;  // M5
    caps.supports_geometry_shader      = false;
    caps.supports_dynamic_state        = false;
    caps.max_texture_slots             = 128;    // 描述符表可容纳的 SRV 数
    caps.max_push_constant_size        = 256;    // root constants 上限
    caps.supports_vsync_control        = true;   // Present(interval, 0) 可切换
    caps.supports_gpu_busy_spin        = false;
    caps.supports_nv_delay_before_swap = false;
    caps.supports_srgb                 = true;
    caps.supports_depth32f             = true;
    caps.supports_r8                   = true;
    caps.supports_rgba16f              = true;   // DXGI_FORMAT_R16G16B16A16_FLOAT
    return caps;
}

} // namespace gryce_engine::render::d3d12
