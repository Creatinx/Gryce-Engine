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
#include "d3d12_reflect.h"
#include "d3d12_texture.h"
#include "d3d12_mesh.h"
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
    {
        char buf[8] = {};
        const DWORD n = GetEnvironmentVariableA("GRYCE_D3D12_SERIALIZE", buf, sizeof(buf));
        gpu_serialize_ = (n > 0 && buf[0] != '0');
        if (gpu_serialize_) GLOG_INFO("D3D12: per-frame GPU serialize enabled (GRYCE_D3D12_SERIALIZE=1)");
    }
    if (want_debug && loader_.get_debug_interface) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(loader_.get_debug_interface(__mingw_uuidof<ID3D12Debug>(),
                                                  reinterpret_cast<void**>(&dbg))) &&
            dbg) {
            dbg->EnableDebugLayer();
            debug_layer_enabled_ = true;
            GLOG_INFO("D3D12: debug layer enabled (GRYCE_D3D12_DEBUG=1)");
            // GRYCE_D3D12_GPU_VALIDATION=1：额外打开 GPU 侧校验。
            // 专门抓"GPU 读到了尚未完成写入 / 未正确同步"这类可见性问题 ——
            // 普通调试层只校验 API 调用，对这类竞态是静默的（而它正是"同一批命令
            // 有时出对有时出错"的典型成因）。
            {
                char vb[8] = {};
                const DWORD n = GetEnvironmentVariableA("GRYCE_D3D12_GPU_VALIDATION", vb, sizeof(vb));
                if (n > 0 && vb[0] != '0') {
                    ID3D12Debug1* dbg1 = nullptr;
                    if (SUCCEEDED(dbg->QueryInterface(__mingw_uuidof<ID3D12Debug1>(),
                                                      reinterpret_cast<void**>(&dbg1))) && dbg1) {
                        dbg1->SetEnableGPUBasedValidation(TRUE);
                        GLOG_INFO("D3D12: GPU-based validation enabled "
                                  "(GRYCE_D3D12_GPU_VALIDATION=1)");
                        dbg1->Release();
                    } else {
                        GLOG_WARN("D3D12: ID3D12Debug1 unavailable, GPU validation skipped");
                    }
                }
            }
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

    // 8) 上传环（M3）：UBO / 每帧实例数据 / HiZ 回写用。
    //
    // ★ 容量必须按**单次最大分配**反推，否则会出现"看起来只是慢一点、实际上
    //   数据被丢掉"的静默故障：UploadRing 按 frames_in_flight 切成独立分段，
    //   单次分配超过一个分段就是**永远失败**，调用方的降级路径各不相同。
    //   本引擎里最大的一笔是 GPU 遮挡剔除的 HiZ 纹理→SSBO 回写：
    //   w*h 字节（1920x1080 = 2,073,600B）。旧的固定 4MB → 每槽 1.4MB，
    //   于是每帧必然告警一次 "upload ring exhausted (2073600 bytes)"，
    //   而 drain_hiz_repacks 拿不到空间就把这条 HiZ 数据**直接丢弃**
    //   → GPU 剔除用的 HiZ 层级长期是旧内容/零，遮挡剔除静默失效。
    //   现在按"一个槽 ≥ 一帧 HiZ + 余量"给足容量（槽数 = frames_in_flight）。
    const size_t hiz_bytes = static_cast<size_t>(swapchain_.width()) *
                             static_cast<size_t>(swapchain_.height());
    // 一个槽 = 一帧 HiZ（w*h） + 该帧全部常量/实例数据（实测 1080p 下约 0.5~1MB，
    // 给 2MB 余量；4K 时 HiZ 涨到 8MB，同理跟着涨）。
    // 4MB 余量覆盖：一帧的 UBO/实例数据（实测 1080p 约 0.5~1.4MB）+
    // Play 模式下额外的逐帧动态上传（粒子实例、探针重烘的常量）。
    const size_t per_slot = hiz_bytes + 4u * 1024u * 1024u;
    const size_t ring_capacity = per_slot * swapchain_.buffer_count();
    if (!upload_ring_.init(device_, ring_capacity)) {
        GLOG_ERROR("D3D12::init: upload ring init failed");
        release_device_objects();
        return false;
    }
    GLOG_INFO("D3D12: upload ring {} MB ({} slots x {} KB, HiZ={} KB)", 
              ring_capacity / (1024 * 1024), swapchain_.buffer_count(),
              per_slot / 1024, hiz_bytes / 1024);

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
    release_gpu_profiling_objects();
    mesh_pool_.clear();
    texture_pool_.clear();
    framebuffer_pool_.clear();
    buffer_pool_.clear();

    // 延迟释放队列里剩余的网格缓冲：走到这里调用方已经 wait_gpu_idle
    // （D3D12::shutdown 的既定顺序），可以安全销毁。
    for (RetiredRawResource& r : retired_raw_resources_) {
        if (r.res) r.res->Release();
    }
    retired_raw_resources_.clear();

    upload_ring_.shutdown();
    descriptor_heaps_.shutdown();

    swapchain_.shutdown();

    // present 目标与翻转 blit 的自带管线（必须在设备销毁之前）
    release_present_target();
    if (blit_pso_)  { blit_pso_->Release();  blit_pso_  = nullptr; }
    if (blit_root_) { blit_root_->Release(); blit_root_ = nullptr; }
    blit_pipeline_failed_ = false;

    if (queue_)   { queue_->Release();   queue_   = nullptr; }
    if (device_)  { device_->Release();  device_  = nullptr; }
    if (adapter_) { adapter_->Release(); adapter_ = nullptr; }
    if (loader_.factory) { loader_.factory->Release(); loader_.factory = nullptr; }
    shutdown_loader(loader_);

    hwnd_ = nullptr;
    initialized_ = false;
    frame_aborted_ = false;
    back_buffer_as_rt_ = false;
    backbuffer_as_rt_ = false;
    present_dirty_ = false;
    present_flushed_ = false;
    using_present_target_ = false;
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
    backbuffer_as_rt_ = false;
    present_dirty_ = false;
    present_flushed_ = false;

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
            // 设备挂起时把 GPU 计时段打出来：最后一个有值（且已结束）的段是
            // 挂起前完成到的位置，配合 gpu_profile_flush_pending 的"挂起点"输出
            // 就能把 TDR 钉到具体 pass（后端自带这套定位机制的用途）。
            if (gpu_profiling_enabled_) gpu_profile_dump();
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
        // present 目标与交换链同尺寸：尺寸变了就重建（旧资源已被 GPU 引用完，
        // 本帧还没提交任何命令，这里直接 Release 是安全的）。
        release_present_target();
    }

    if (!swapchain_.begin_frame()) {
        GLOG_ERROR("D3D12Backend::begin_frame: swapchain begin_frame failed");
        frame_aborted_ = true;
        return;
    }
    // 帧号自增（从 1 开始，0 保留给"从未重置"）。
    ++frame_counter_;

    // ★ GPU 计时帧边界（本帧命令列表已 Reset，但**上一帧已提交**）：
    //   结算上一轮的结果、推进槽位。必须在录制新命令之前 —— 时间戳要落在
    //   新帧的命令列表里（QueryHeap 槽轮转）。
    gpu_profile_frame_boundary();

    // ★ 延迟释放：本帧 GPU 已跑完 frames_in_flight 之前的提交，把那些
    //   已被 GPU 完成引用的待销毁资源真正释放掉（见 retire_* 的说明）。
    //   必须在录制本帧命令**之前**做 —— 这样即使槽位被本帧的 create_* 复用，
    //   也不会和仍在 GPU 上的旧资源撞车。
    drain_retired();

    // 命令列表已 Reset → 所有状态（含描述符堆绑定）都丢了，必须重新绑定。
    // D3D12 要求一次性设置两个堆（SRV/CBV/UAV + Sampler）。
    descriptor_heaps_.bind(swapchain_.command_list());

    // 上传环游标推进到本帧槽的分段：UBO / 实例数据只在本槽分段内分配。
    // ★ 绝不能用"整块 reset"—— 3 帧在飞时帧 N 会覆写帧 N-1 仍在读的内存，
    //   是 DEVICE_HUNG 的根因（详见 UploadRing 的说明）。
    upload_ring_.set_frame_slots(static_cast<uint32_t>(swapchain_.buffer_count()));
    upload_ring_.begin_slot(static_cast<uint32_t>(swapchain_.current_frame_index()));
    // 调试：Ring 分配流水的帧标签（仅 GRYCE_DX12_PROBE_RING=1 时打日志）。
    upload_ring_.set_frame_tag(frame_counter_);

    // HiZ 纹理→SSBO 的延迟重排：上一帧按 256 对齐行距拷进 READBACK 的暂存，
    // 此刻 GPU 已跑完 → 在 CPU 重排成紧凑行并写回 SSBO（必须在 begin_slot 之后，
    // 因为要经 UploadRing）。
    drain_hiz_repacks();

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
    // 清的是"当前屏幕 RT"：present 目标（正常路径）或真实 back buffer（降级路径）。
    cmd->ClearRenderTargetView(screen_rtv(), c, 0, nullptr);
}
// ===========================================================================
// present 目标 + 帧末翻转 blit
// ===========================================================================
// 背景（见头文件与 docs/d3d12-backend-plan.md §5 的"渲染目标 Y 方向"）：
//   本后端用 SPIRV-Cross 的 FLIP_VERTEX_Y 把整条管线等效成 **OpenGL 的坐标/
//   纹理约定**（含 gl_FragCoord 与 RT 采样方向）。这样做的好处是引擎全部
//   GL 版着色器里的 V 翻转、屏幕空间 UV 推导都原样成立 —— 实测画面与 GL
//   逐像素一致（|Δ| ≈ 0.06/255）。
//   代价只有一个：最终成品相对 D3D 的呈现方向是**上下镜像**的（D3D 的 back
//   buffer 第 0 行在屏幕顶部，GL 约定的第 0 行在屏幕底部）。
//   所以：所有"画到屏幕"的 pass 先落在下面的 present 目标上，帧末由一条自带
//   的 blit 通道翻转着写进真实 back buffer。
//   ★ 关键点：翻转只发生一次，且与"这一帧开了哪些 pass"无关。早先试过直接
//     关掉 FLIP_VERTEX_Y（让几何方向天然正确），结果是每个全屏 pass 都会多翻
//     一次 → 镜像次数的奇偶随 pass 数量变化，开/关 SSR 的画面方向都不一样；
//     而且引擎着色器里那些 GL 约定的 V 翻转全部失效（金属反射、屏幕空间效果
//     全错）。集中在帧末翻一次是唯一稳的做法。
//
// blit 通道本身**不做** Y 翻转：把 present 目标按 D3D 原生方向铺满 back buffer
// 恰好把 GL 约定转回 D3D 呈现方向。
// ===========================================================================

bool D3D12Backend::ensure_present_target() {
    if (!device_) return false;
    const uint32_t w = swapchain_.width();
    const uint32_t h = swapchain_.height();
    if (w == 0 || h == 0) return false;
    if (present_tex_ && present_w_ == w && present_h_ == h) return true;

    release_present_target();

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    // 与交换链同格式（d3d12_swapchain.cpp:59 设的就是这个），blit 是逐像素拷贝。
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.SampleDesc.Quality = 0;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // 创建时给一个 CLEAR_VALUE，且用**当前帧的清屏色**：调试层对"带 CLEAR_VALUE
    // 的资源被清成另一个值"以及"没有 CLEAR_VALUE 的 RT 被清"都会报
    // "clear values do not match those passed to resource creation"。
    // 引擎的清屏色是稳定值（场景背景色），这里跟着它走即可消掉这条告警。
    D3D12_CLEAR_VALUE cv{};
    cv.Format = rd.Format;
    cv.Color[0] = clear_r_;
    cv.Color[1] = clear_g_;
    cv.Color[2] = clear_b_;
    cv.Color[3] = clear_a_;
    HRESULT hr = device_->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
        __mingw_uuidof<ID3D12Resource>(), reinterpret_cast<void**>(&present_tex_));
    if (FAILED(hr) || !present_tex_) {
        GLOG_ERROR("D3D12Backend: present target creation failed 0x{:08X} ({}x{})",
                   static_cast<uint32_t>(hr), w, h);
        present_tex_ = nullptr;
        return false;
    }
    present_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // RTV 堆：RTV 必须放在 NON_SHADER_VISIBLE 堆里（放 SHADER_VISIBLE 会被
    // CreateDescriptorHeap 直接拒绝）。
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hr = device_->CreateDescriptorHeap(&hd, __mingw_uuidof<ID3D12DescriptorHeap>(),
                                       reinterpret_cast<void**>(&present_rtv_heap_));
    if (FAILED(hr) || !present_rtv_heap_) {
        GLOG_ERROR("D3D12Backend: present target RTV heap creation failed 0x{:08X}",
                   static_cast<uint32_t>(hr));
        release_present_target();
        return false;
    }
    present_rtv_ = present_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateRenderTargetView(present_tex_, nullptr, present_rtv_);

    present_w_ = w;
    present_h_ = h;
    return true;
}

void D3D12Backend::release_present_target() {
    if (present_tex_) {
        present_tex_->Release();
        present_tex_ = nullptr;
    }
    if (present_rtv_heap_) {
        present_rtv_heap_->Release();
        present_rtv_heap_ = nullptr;
    }
    present_rtv_ = D3D12_CPU_DESCRIPTOR_HANDLE{};
    present_w_ = present_h_ = 0;
    present_state_ = D3D12_RESOURCE_STATE_COMMON;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Backend::screen_rtv() const {
    if (using_present_target_) return present_rtv_;
    return swapchain_.back_buffer_rtv();
}

bool D3D12Backend::ensure_blit_pipeline() {
    if (blit_pso_ && blit_root_) return true;
    if (blit_pipeline_failed_) return false;
    if (!device_ || !loader_.d3d_compile || !loader_.serialize_root_signature) {
        // 没有 d3dcompiler（极少见：ImGui 后端也要它）时退化成旧行为：
        // 直接画进 back buffer（上下镜像），至少不会崩。
        GLOG_WARN("D3D12Backend: d3dcompiler unavailable -> present flip disabled "
                  "(final image will be vertically mirrored)");
        blit_pipeline_failed_ = true;
        return false;
    }

    // ---- 根签名：参数 0 = 1 个 SRV 描述符表(t0)，参数 1 = 静态采样器(s0) ----
    D3D12_DESCRIPTOR_RANGE srv_range{};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 1;
    srv_range.BaseShaderRegister = 0;
    srv_range.RegisterSpace = 0;
    srv_range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER params[1]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &srv_range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC smp{};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;   // 逐像素拷贝，点采样
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    smp.MaxAnisotropy = 1;
    smp.MinLOD = 0.0f;
    smp.MaxLOD = D3D12_FLOAT32_MAX;
    smp.ShaderRegister = 0;
    smp.RegisterSpace = 0;
    smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 1;
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &smp;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ID3DBlob* sig_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    HRESULT hr = loader_.serialize_root_signature(
        &rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob);
    if (FAILED(hr) || !sig_blob) {
        GLOG_ERROR("D3D12Backend: blit root signature serialize failed 0x{:08X}{}",
                   static_cast<uint32_t>(hr),
                   err_blob ? std::string(": ") +
                                  static_cast<const char*>(err_blob->GetBufferPointer())
                            : std::string());
        if (sig_blob) sig_blob->Release();
        if (err_blob) err_blob->Release();
        blit_pipeline_failed_ = true;
        return false;
    }
    hr = device_->CreateRootSignature(0, sig_blob->GetBufferPointer(),
                                      sig_blob->GetBufferSize(),
                                      __mingw_uuidof<ID3D12RootSignature>(),
                                      reinterpret_cast<void**>(&blit_root_));
    sig_blob->Release();
    if (err_blob) err_blob->Release();
    if (FAILED(hr) || !blit_root_) {
        GLOG_ERROR("D3D12Backend: blit CreateRootSignature failed 0x{:08X}",
                   static_cast<uint32_t>(hr));
        blit_pipeline_failed_ = true;
        return false;
    }

    // ---- 内嵌 HLSL：VS 由 SV_VertexID 生成全屏三角形，PS 直取纹理 ----
    // ★ 两者都**不**做 Y 翻转 —— 这正是"翻回 D3D 呈现方向"的那一次翻转
    //   （引擎侧已经被 FLIP_VERTEX_Y 整体反过来了）。
    static const char* k_blit_hlsl = R"(
Texture2D    g_present : register(t0);
SamplerState g_sampler : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut vs_main(uint vid : SV_VertexID) {
    // (0,0) (2,0) (0,2) -> 覆盖整屏的大三角形
    float2 p = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv  = p;
    return o;
}

float4 ps_main(VSOut i) : SV_Target {
    return float4(g_present.Sample(g_sampler, i.uv).rgb, 1.0);
}
)";

    auto compile = [&](const char* entry, const char* target, ID3DBlob** out) -> bool {
        ID3DBlob* err = nullptr;
        const HRESULT h = loader_.d3d_compile(k_blit_hlsl, std::strlen(k_blit_hlsl), nullptr,
                                              nullptr, nullptr, entry, target, 0, 0, out, &err);
        if (FAILED(h) || !*out) {
            GLOG_ERROR("D3D12Backend: blit shader '{}' compile failed 0x{:08X}{}",
                       entry, static_cast<uint32_t>(h),
                       err ? std::string(": ") +
                                 static_cast<const char*>(err->GetBufferPointer())
                           : std::string());
            if (err) err->Release();
            return false;
        }
        if (err) err->Release();
        return true;
    };

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    if (!compile("vs_main", "vs_5_0", &vs_blob) || !compile("ps_main", "ps_5_0", &ps_blob)) {
        if (vs_blob) vs_blob->Release();
        if (ps_blob) ps_blob->Release();
        blit_pipeline_failed_ = true;
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = blit_root_;
    pd.VS.pShaderBytecode = vs_blob->GetBufferPointer();
    pd.VS.BytecodeLength = vs_blob->GetBufferSize();
    pd.PS.pShaderBytecode = ps_blob->GetBufferPointer();
    pd.PS.BytecodeLength = ps_blob->GetBufferSize();
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;

    hr = device_->CreateGraphicsPipelineState(&pd, __mingw_uuidof<ID3D12PipelineState>(),
                                              reinterpret_cast<void**>(&blit_pso_));
    vs_blob->Release();
    ps_blob->Release();
    if (FAILED(hr) || !blit_pso_) {
        GLOG_ERROR("D3D12Backend: blit CreateGraphicsPipelineState failed 0x{:08X}",
                   static_cast<uint32_t>(hr));
        blit_pso_ = nullptr;
        blit_pipeline_failed_ = true;
        return false;
    }
    return true;
}

void D3D12Backend::record_present_blit(ID3D12GraphicsCommandList* cmd) {
    if (!cmd) return;
    if (!ensure_present_target()) return;
    if (!ensure_blit_pipeline()) return;

    ID3D12Resource* back = swapchain_.back_buffer();
    if (!back) return;

    // present 目标：RENDER_TARGET → PIXEL_SHADER_RESOURCE（采样前必须切态）。
    if (present_state_ != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = present_tex_;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = present_state_;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(1, &b);
        present_state_ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    // back buffer：PRESENT → RENDER_TARGET（本帧此前一直没动过它）。
    if (!backbuffer_as_rt_) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = back;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd->ResourceBarrier(1, &b);
        backbuffer_as_rt_ = true;
    }

    // SRV 写进引擎主堆。★ 用**按帧槽固定复用**的槽位，绝不每帧新申请：
    //   每帧泄漏一个槽位会在堆满时触发 DescriptorAllocator 的"重建+拷贝"，
    //   让在飞命令列表里的描述符句柄悬空 → DEVICE_HUNG（见头文件成员处的说明）。
    const uint32_t frame_slot = static_cast<uint32_t>(swapchain_.current_frame_index());
    const size_t slot_count = swapchain_.buffer_count();
    if (present_srv_slots_.size() < slot_count) {
        present_srv_slots_.assign(slot_count, UINT32_MAX);
    }
    uint32_t srv_base = present_srv_slots_[frame_slot];
    if (srv_base == UINT32_MAX) {
        if (!descriptor_heaps_.srv().allocate_range(1, srv_base)) {
            GLOG_WARN("D3D12Backend: present blit SRV heap exhausted; frame left mirrored");
            return;
        }
        present_srv_slots_[frame_slot] = srv_base;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(present_tex_, &sd,
                                      descriptor_heaps_.srv().cpu_handle(srv_base));

    // 描述符表要求"引擎主堆"是当前堆（ImGui 可能刚把自己的堆绑上去过）。
    descriptor_heaps_.bind(cmd);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_.back_buffer_rtv();
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT vp{};
    vp.Width = static_cast<float>(present_w_);
    vp.Height = static_cast<float>(present_h_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &vp);
    D3D12_RECT sr{};
    sr.right = static_cast<LONG>(present_w_);
    sr.bottom = static_cast<LONG>(present_h_);
    cmd->RSSetScissorRects(1, &sr);

    cmd->SetGraphicsRootSignature(blit_root_);
    cmd->SetPipelineState(blit_pso_);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->SetGraphicsRootDescriptorTable(
        0, descriptor_heaps_.srv().gpu_handle(srv_base));
    cmd->DrawInstanced(3, 1, 0, 0);

    present_flushed_ = true;
}

void D3D12Backend::flush_present_blit_for_overlay() {
    if (!initialized_ || present_flushed_) return;
    if (!present_dirty_) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;
    record_present_blit(cmd);
}

void D3D12Backend::end_frame() {
    if (!initialized_) return;
    if (frame_aborted_) return;

    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();

    // ---- 截图（M5.3）：把拷贝**录制进本帧的命令列表** ----
    //
    //   ★ 为什么必须录进本帧列表，而不能用"自建命令列表 + 立刻回读"：
    //     本帧的场景绘制命令此刻还**躺在 swapchain_.command_list() 里没提交**。
    //     若现在自建一个列表去 CopyTextureRegion，GPU 看到的 back buffer 里
    //     根本还没有本帧的内容（要么是清的底色、要么是 3 帧前某个 buffer 的
    //     残留），得到的就是确定性 period-3 的黑帧 / 陈旧帧。
    //     唯一正确的时序是：拷贝和绘制在**同一条命令列表**里，绘制在前、拷贝
    //     在后，一起提交 —— GPU 保证按提交顺序执行，拷到的必然是本帧结果。
    //
    //   拷贝完成后再做 RT→PRESENT，然后 Close/Execute/Present + 等 GPU，
    //   最后从 readback 缓冲把像素取回来（见 end_frame 尾部的 finish_screenshot）。
    // ★ 截图请求**只在真的拷成功了才消费**。早先的写法是
    //     want_shot = !path.empty() && frame_counter_ >= screenshot_frame_
    //   然后无条件 finish_screenshot() —— 而 finish_screenshot() 一进来就
    //   `screenshot_path_.clear()`。于是只要**目标帧**恰好没有把交换链当作渲染
    //   目标（back_buffer_as_rt_ == false，例如某些 pass 收尾留在离屏 RT 上），
    //   begin_screenshot_copy 被跳过、请求却照样被丢弃 → 上层只看到
    //   "截图未生成（渲染线程未写盘）"，且该帧数与分辨率有关，表现为"时好时坏"。
    //   现在把 back_buffer_as_rt_ 纳入条件、并用 begin 的返回值决定是否消费，
    //   没拷成就不清请求，下一帧继续尝试。
    bool shot_now = false;

    // ★ 帧末翻转 blit：本帧所有"画到屏幕"的 pass 都落在 present 目标上（GL 约定，
    //   上下镜像），这里翻一次写进真实 back buffer —— 之后的截图拷贝与 Present
    //   看到的都是与 GL/Vulkan 同方向的成品。
    if (cmd && present_dirty_ && !present_flushed_) {
        record_present_blit(cmd);
    }

    if (!screenshot_path_.empty() && frame_counter_ >= screenshot_frame_ && cmd &&
        back_buffer_as_rt_) {
        shot_now = begin_screenshot_copy(cmd);
    }

    if (cmd && backbuffer_as_rt_) {
        // RENDER_TARGET → PRESENT，Present 前必须完成
        D3D12_RESOURCE_BARRIER to_present{};
        to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_present.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        to_present.Transition.pResource = swapchain_.back_buffer();
        to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cmd->ResourceBarrier(1, &to_present);
        backbuffer_as_rt_ = false;
    }

    if (!swapchain_.end_frame()) {
        GLOG_ERROR("D3D12Backend::end_frame: swapchain end_frame failed");
    }

    // 本帧已提交 → 等 GPU 跑完 → 从 readback 缓冲取回像素并落盘。
    if (shot_now) {
        finish_screenshot();
    }

    // 调试探针回读（env 门控；无挂起探针时零成本）。
    probe_drain();

    // ★ 调试用串行化（GRYCE_D3D12_SERIALIZE=1）：每帧结束等 GPU 全部跑完。
    //   默认 3 帧在飞时 CPU 会跑到 GPU 前面好几帧，TDR 在 begin_frame 才被发现，
    //   于是"检测到挂起的帧号"和"真正挂掉的那一帧"对不上，无法定位。串行化后
    //   CPU 永远领先 GPU 0 帧，pass 轨迹的最后一条就是 GPU 真正卡住的位置。
    //   仅调试用 —— 会彻底牺牲 CPU/GPU 并行度。
    if (gpu_serialize_) {
        swapchain_.wait_all();
        if (device_) {
            const HRESULT reason = device_->GetDeviceRemovedReason();
            if (reason != S_OK && !device_lost_) {
                device_lost_ = true;
                GLOG_ERROR("D3D12Backend: [serialize] device removed after frame {} (reason=0x{:08X})",
                           frame_counter_, static_cast<uint32_t>(reason));
                dump_info_queue();
            }
        }
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
void D3D12Backend::bind_vertex_slots(ID3D12GraphicsCommandList* cmd, D3D12Mesh* m) {
    if (!cmd) return;
    D3D12_VERTEX_BUFFER_VIEW vb{};
    D3D12_VERTEX_BUFFER_VIEW ivb{};
    if (m) {
        vb = m->vertex_view();
        if (m->is_instance_stream_valid()) ivb = m->instance_view();
    }
    cmd->IASetVertexBuffers(0, 1, &vb);
    // slot 1 始终显式设置（可能为空 view）。PSO 里带 PER_INSTANCE_DATA 元素时
    // 必须先绑好实例缓冲，否则调试层报 "no vertex buffer bound to slot 1"，
    // 且在 GPU 上读到的是未定义数据。
    cmd->IASetVertexBuffers(1, 1, &ivb);
}

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
    key.samples = static_cast<uint8_t>(current_rt_samples_);
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
    bind_vertex_slots(cmd, m);
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
    key.samples = static_cast<uint8_t>(current_rt_samples_);
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
    bind_vertex_slots(cmd, m);

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
    // 64-bit 计算行/数据尺寸：uint32 乘法在极端分辨率下会回绕，
    // 导致 BMP 头里的尺寸字段与实际写出数据不一致。
    const uint64_t row_size64 = (static_cast<uint64_t>(width) * 3 + 3) & ~3ull;
    const uint64_t data_size64 = row_size64 * height;
    if (data_size64 > 0xFFFFFFFFull - 54) {
        GLOG_ERROR("D3D12 save_bmp: frame {}x{} too large for BMP ({} bytes)",
                   width, height, data_size64);
        return;
    }
    const uint32_t row_size = static_cast<uint32_t>(row_size64);
    const uint32_t data_size = static_cast<uint32_t>(data_size64);
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

bool D3D12Backend::begin_screenshot_copy(ID3D12GraphicsCommandList* cmd) {
    if (!initialized_ || !device_ || !cmd) return false;

    // 只在本帧确实渲染进了交换链时才截（否则 back buffer 里没有本帧内容）。
    const uint32_t w = swapchain_.width();
    const uint32_t h = swapchain_.height();
    if (w == 0 || h == 0) return false;

    release_screenshot_staging();

    ID3D12Resource* back = swapchain_.back_buffer_at(rendered_buffer_index_);
    if (!back) back = swapchain_.back_buffer();
    if (!back) {
        GLOG_ERROR("D3D12Backend::begin_screenshot_copy: swapchain back buffer is null");
        return false;
    }

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
        GLOG_ERROR("D3D12Backend::begin_screenshot_copy: GetCopyableFootprints returned 0");
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
        GLOG_ERROR("D3D12Backend::begin_screenshot_copy: CreateCommittedResource(readback) "
                   "failed 0x{:08X} ({} bytes)", static_cast<uint32_t>(hr), total);
        return false;
    }

    // ★ 录制进**本帧已有的命令列表**（不能另建列表）：
    //   本帧的绘制命令此刻还没提交，另建列表立刻拷贝只会拷到空 buffer。
    //   这里插入的屏障/拷贝会和本帧绘制一起被提交，GPU 保证顺序。
    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before,
                          D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = res;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &b);
    };

    // 此刻 back buffer 处于本帧渲染留下的 RENDER_TARGET 态。
    transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);

    // ★ 纹理 → 缓冲必须用 CopyTextureRegion + 显式 footprint，
    //   **不能用 CopyResource**：CopyResource 要求源与目标的描述完全一致
    //   （同类型、同尺寸、同格式），而这里是 2D 纹理 → 缓冲区，属于非法组合。
    //   实测该调用不报错也不移除设备，只是**静默什么都不拷**，回读缓冲保持全 0
    //   —— 表现就是"截图永远全黑"，让人误判成渲染管线没生效。
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layout;

    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = back;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src_loc, nullptr);

    // 拷完还原成 RENDER_TARGET —— 紧接着 end_frame 还要做 RT→PRESENT，
    // 这里不能越权直接改成 PRESENT（否则连着两条屏障态不符）。
    transition(back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // 拷贝所需信息留到 finish_screenshot（本帧提交并等 GPU 完成后）再用。
    shot_readback_ = readback;
    shot_footprint_ = layout;
    shot_total_ = total;
    shot_w_ = w;
    shot_h_ = h;
    return true;
}

void D3D12Backend::finish_screenshot() {
    const std::string path = screenshot_path_;
    screenshot_path_.clear();

    if (!shot_readback_) return;

    // ★ 本帧的命令列表已经 Close + Execute + Present 过了。这里 wait_all()
    //   只是"排空在飞帧"，确保拷贝真的执行完（Map 前必须）。
    swapchain_.wait_all();

    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(shot_total_)};
    HRESULT hr = shot_readback_->Map(0, &read_range, &mapped);
    if (FAILED(hr) || !mapped) {
        GLOG_ERROR("D3D12Backend::finish_screenshot: Map failed 0x{:08X}",
                   static_cast<uint32_t>(hr));
        release_screenshot_staging();
        return;
    }

    // 按 footprint 的行距逐行拆包（行距 ≥ w*4，多出来的是对齐填充）。
    const uint32_t w = shot_w_;
    const uint32_t h = shot_h_;
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mapped);
    const uint32_t row_pitch = shot_footprint_.Footprint.RowPitch;
    const uint32_t copy_bytes = w * 4;
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(rgba.data() + static_cast<size_t>(y) * copy_bytes,
                    src + static_cast<size_t>(y) * row_pitch, copy_bytes);
    }
    // 回读堆不需要写回 CPU 侧的修改。
    const D3D12_RANGE written{0, 0};
    shot_readback_->Unmap(0, &written);
    release_screenshot_staging();

    if (path.empty()) return;
    if (d3d12_path_ends_with(path, ".png")) {
        if (stbi_write_png(path.c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                           rgba.data(), static_cast<int>(w) * 4)) {
            GLOG_INFO("D3D12Backend: screenshot saved to '{}' ({}x{} PNG)", path, w, h);
        } else {
            GLOG_ERROR("D3D12Backend: stbi_write_png failed for '{}'", path);
        }
    } else {
        save_bmp_from_rgba(path, rgba.data(), w, h);
        GLOG_INFO("D3D12Backend: screenshot saved to '{}' ({}x{} BMP)", path, w, h);
    }
}

void D3D12Backend::release_screenshot_staging() {
    if (shot_readback_) {
        shot_readback_->Release();
        shot_readback_ = nullptr;
    }
    shot_total_ = 0;
    shot_w_ = 0;
    shot_h_ = 0;
}

// ---------------------------------------------------------------------------
// 调试探针（SSR 竞态排查）
//
// 背景：D3D12 上 SSR 偶发"整帧丢失"（合法序列、GPU 校验零报错、时序相关）。
// 本探针在指定帧把 SSR 链上的纹理拷回 CPU，输出内容统计（均值 / 非零占比 /
// FNV 哈希），与同帧截图对比即可定位丢失发生在哪一段：
//   * composite_out 哈希好坏帧一致、截图不同 → 丢失在 composite **之后**
//     （tonemap 采样侧：描述符/状态问题）
//   * composite_out 哈希好坏帧不同 → 丢失在 composite **之前**（继续看
//     trace_out / blur_out / hdr_in 定位具体输入）
// 门控：GRYCE_DX12_PROBE_SSR=1 启用；GRYCE_DX12_PROBE_FRAME=<N> 指定帧
// （默认 60）。拷贝录进本帧命令列表，帧提交后等 GPU 再回读（与截图同款
// 两阶段模式，保证读到的是本帧内容）。
// ---------------------------------------------------------------------------
void D3D12Backend::probe_texture(RHITextureHandle tex, const char* tag) {
    if (!initialized_ || device_lost_) return;
    if (!std::getenv("GRYCE_DX12_PROBE_SSR")) return;

    uint64_t want_frame = 60;
    if (const char* f = std::getenv("GRYCE_DX12_PROBE_FRAME")) {
        want_frame = std::strtoull(f, nullptr, 10);
    }
    if (frame_counter_ != want_frame) return;

    D3D12Texture* t = texture_pool_.get_if_alive(tex.index, tex.generation);
    if (!t || !t->is_valid() || !t->resource()) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    const D3D12_RESOURCE_DESC desc = t->resource()->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    const uint32_t w = static_cast<uint32_t>(desc.Width);
    const uint32_t h = static_cast<uint32_t>(desc.Height);
    if (w == 0 || h == 0) return;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total = 0;
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);
    if (total == 0) return;

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
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
        GLOG_ERROR("D3D12Backend::probe_texture: readback alloc failed 0x{:08X} ({} bytes)",
                   static_cast<uint32_t>(hr), static_cast<uint64_t>(total));
        return;
    }

    // before → COPY_SOURCE，拷完还原（净状态不变，不影响后续 pass）。
    const D3D12_RESOURCE_STATES before = t->current_state();
    auto transition = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = t->resource();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cmd->ResourceBarrier(1, &b);
    };
    transition(before, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layout;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = t->resource();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    transition(D3D12_RESOURCE_STATE_COPY_SOURCE, before);

    PendingProbe p{};
    p.readback = readback;
    p.footprint = layout;
    p.total = total;
    p.w = w;
    p.h = h;
    p.format = desc.Format;
    p.tag = tag;
    p.frame = frame_counter_;
    pending_probes_.push_back(p);
}

void D3D12Backend::probe_drain() {
    if (pending_probes_.empty()) return;
    // 本帧已提交 → 排空在飞帧，保证拷贝执行完（Map 前必须）。
    swapchain_.wait_all();

    for (const PendingProbe& p : pending_probes_) {
        if (!p.readback) continue;
        void* mapped = nullptr;
        const D3D12_RANGE read_range{0, static_cast<SIZE_T>(p.total)};
        HRESULT hr = p.readback->Map(0, &read_range, &mapped);
        if (FAILED(hr) || !mapped) {
            GLOG_ERROR("D3D12Backend::probe_drain: Map failed 0x{:08X}", static_cast<uint32_t>(hr));
            p.readback->Release();
            continue;
        }
        const uint8_t* base = static_cast<const uint8_t*>(mapped);
        const uint32_t row_pitch = p.footprint.Footprint.RowPitch;

        // FNV-1a 64 位内容哈希（逐行跳过对齐填充）。
        uint64_t hash = 1469598103934665603ull;
        auto mix = [&](const uint8_t* data, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                hash ^= data[i];
                hash *= 1099511628211ull;
            }
        };
        // half → float（仅用于均值展示；哈希走原始位）。
        auto half_to_float = [](uint16_t h) -> float {
            const uint32_t sign = (h >> 15) & 0x1u;
            uint32_t exp = (h >> 10) & 0x1Fu;
            uint32_t man = h & 0x3FFu;
            uint32_t bits = 0;
            if (exp == 0) {
                if (man == 0) {
                    bits = sign << 31;
                } else {
                    exp = 127 - 15 + 1;
                    while ((man & 0x400u) == 0) { man <<= 1; --exp; }
                    man &= 0x3FFu;
                    bits = (sign << 31) | (exp << 23) | (man << 13);
                }
            } else if (exp == 31) {
                bits = (sign << 31) | (0xFFu << 23) | (man << 13);
            } else {
                bits = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
            }
            float out = 0.0f;
            std::memcpy(&out, &bits, sizeof(out));
            return out;
        };

        double ch_sum[4] = {0, 0, 0, 0};
        size_t nonzero = 0;
        size_t counted = 0;
        const bool is_rgba16f = (p.format == DXGI_FORMAT_R16G16B16A16_FLOAT);
        const uint32_t bpp = is_rgba16f ? 8u : 4u;
        for (uint32_t y = 0; y < p.h; ++y) {
            const uint8_t* row = base + static_cast<size_t>(y) * row_pitch;
            mix(row, static_cast<size_t>(p.w) * bpp);
            for (uint32_t x = 0; x < p.w; ++x) {
                const uint8_t* px = row + static_cast<size_t>(x) * bpp;
                bool nz = false;
                for (uint32_t c = 0; c < 4; ++c) {
                    float v = 0.0f;
                    if (is_rgba16f) {
                        uint16_t bits = 0;
                        std::memcpy(&bits, px + c * 2, 2);
                        v = half_to_float(bits);
                    } else {
                        v = static_cast<float>(px[c]) / 255.0f;
                    }
                    ch_sum[c] += v;
                    if (v != 0.0f) nz = true;
                }
                if (nz) ++nonzero;
                ++counted;
            }
        }
        const size_t n = counted ? counted : 1;
        GLOG_WARN("[probe] frame={} tag={} {}x{} fmt={} mean=({:.4f},{:.4f},{:.4f},{:.4f}) "
                  "nonzero={:.3f} hash=0x{:016X}",
                  p.frame, p.tag ? p.tag : "?", p.w, p.h,
                  static_cast<int>(p.format), ch_sum[0] / n, ch_sum[1] / n, ch_sum[2] / n,
                  ch_sum[3] / n, static_cast<double>(nonzero) / static_cast<double>(n), hash);

        const D3D12_RANGE written{0, 0};
        p.readback->Unmap(0, &written);
        p.readback->Release();
    }
    pending_probes_.clear();
}

bool D3D12Backend::capture_frame_rgba(std::vector<uint8_t>& out, int& width, int& height) {
    // 同步回读接口（非渲染线程帧内路径；引擎正式截图走 request_screenshot +
    // finish_screenshot 的两阶段异步路径）。这里用自建命令列表立刻拷贝：
    // 前提是**本帧命令已提交**（否则 back buffer 里没有本帧内容 —— 那正是这条
    // 路径的固有局限，正式路径正是为绕开它而生）。用于调试/兼容调用。
    if (!initialized_ || !device_ || !queue_) return false;

    const uint32_t w = swapchain_.width();
    const uint32_t h = swapchain_.height();
    if (w == 0 || h == 0) return false;

    ID3D12Resource* back = swapchain_.back_buffer_at(swapchain_.current_back_buffer_index());
    if (!back) back = swapchain_.back_buffer();
    if (!back) return false;

    const D3D12_RESOURCE_DESC tex_desc = back->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total = 0;
    device_->GetCopyableFootprints(&tex_desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);
    if (total == 0 || layout.Footprint.RowPitch == 0) return false;

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

    ID3D12Resource* readback = nullptr;
    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  __uuidof(ID3D12Resource),
                                                  reinterpret_cast<void**>(&readback));
    if (FAILED(hr) || !readback) return false;

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
        if (alloc) alloc->Release();
        readback->Release();
        return false;
    }
    // ★ CreateCommandList 返回即 recording 态，**不要** Close()（见 begin_ 分支注释）。

    auto transition = [&](D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = back;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cl->ResourceBarrier(1, &b);
    };
    transition(D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = layout;
    D3D12_TEXTURE_COPY_LOCATION src_loc{};
    src_loc.pResource = back;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src_loc.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src_loc, nullptr);
    transition(D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);

    if (FAILED(cl->Close())) {
        GLOG_ERROR("D3D12Backend::capture_frame_rgba: command list Close failed");
    }
    ID3D12CommandList* lists[] = { cl };
    queue_->ExecuteCommandLists(1, lists);
    swapchain_.wait_all();

    void* mapped = nullptr;
    const D3D12_RANGE read_range{0, static_cast<SIZE_T>(total)};
    hr = readback->Map(0, &read_range, &mapped);
    if (FAILED(hr) || !mapped) {
        cl->Release(); alloc->Release(); readback->Release();
        return false;
    }
    out.resize(static_cast<size_t>(w) * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mapped);
    const uint32_t row_pitch = layout.Footprint.RowPitch;
    const uint32_t copy_bytes = w * 4;
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * copy_bytes,
                    src + static_cast<size_t>(y) * row_pitch, copy_bytes);
    }
    const D3D12_RANGE written{0, 0};
    readback->Unmap(0, &written);
    cl->Release(); alloc->Release(); readback->Release();
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
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
    // 传入 retire sink：网格缓冲容量不足而重建时，旧资源要按围栏值延迟释放
    // （见 D3D12Mesh::update_or_create_buffer 的说明）。
    if (m) m->set_context(device_, queue_, this);
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
    // 注入 UploadSink（this）：DEFAULT 堆 SSBO 的 update() 需要把拷贝录制进
    // 当前帧命令列表，而不是"提交+等待"。
    if (b) b->set_context(device_, queue_, this);
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

    // 被替换掉的网格缓冲（见 retire_gpu_resource）。
    {
        size_t keep = 0;
        for (size_t i = 0; i < retired_raw_resources_.size(); ++i) {
            RetiredRawResource& r = retired_raw_resources_[i];
            if (r.fence_value <= done) {
                if (r.res) r.res->Release();
            } else {
                if (keep != i) retired_raw_resources_[keep] = r;
                ++keep;
            }
        }
        retired_raw_resources_.resize(keep);
    }

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

void D3D12Backend::retire_gpu_resource(ID3D12Resource* res) {
    if (!res) return;
    // 与 retire_descriptor_range 同一时间基准：GPU 跑过"最近一次已提交"的围栏值，
    // 就说明引用该资源的命令列表已经执行完，可以安全销毁。
    RetiredRawResource r{};
    r.fence_value = swapchain_.last_submitted_value();
    r.res = res;
    retired_raw_resources_.push_back(r);
}

// 销毁：用带 generation 校验的 deallocate —— 旧句柄二次销毁不会误杀复用槽位。
void D3D12Backend::destroy_mesh(RHIMeshHandle handle) {
    if (!handle.is_valid()) return;
    retire_mesh(handle.index, handle.generation);
}

void D3D12Backend::destroy_shader(RHIShaderHandle handle) {
    if (!handle.is_valid()) return;
    if (is_compute_shader(handle)) {
        compute_pool_.deallocate(handle.index, compute_pool_generation(handle));
        return;
    }
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
    // 计算句柄不是 IShader（另行成池 + 标志位），返回 nullptr 让调用方走
    // dispatch_compute 那条路。这与 Vulkan 后面的处理一致。
    if (is_compute_shader(handle)) return nullptr;
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

    // ★ 引擎的"画到屏幕"落在后端内部的 present 目标上（帧末统一翻转 blit 进
    //   真实 back buffer），见 ensure_present_target 的详解。
    //   若该目标不可用（显存不足/d3dcompiler 缺失），退回旧的"直接画进 back
    //   buffer"行为：画面上下镜像但不会崩。
    // ★ present_flushed_ == true 表示本帧已经做过翻转 blit（UI 覆盖层绘制前那一刷），
    //   此后的"画到屏幕"必须直接画在**真实 back buffer** 上（它此刻已在
    //   RENDER_TARGET 态）：否则会画进 present 目标却再也不会被拷出去 —— 画面丢内容。
    const bool use_present_target =
        !present_flushed_ && ensure_present_target() && !blit_pipeline_failed_;
    using_present_target_ = use_present_target;
    ID3D12Resource* target = use_present_target ? present_tex_ : swapchain_.back_buffer();
    if (!target) return;

    if (use_present_target) {
        if (present_state_ != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = present_tex_;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = present_state_;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cmd->ResourceBarrier(1, &b);
            present_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        present_dirty_ = true;
    } else {
        // 降级路径，或"UI 之后又画屏幕"：back buffer 可能已经是 RT（blit 留下的），
        // 只有还停在 PRESENT 时才需要这一次转换。
        if (!backbuffer_as_rt_) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = swapchain_.back_buffer();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cmd->ResourceBarrier(1, &b);
            backbuffer_as_rt_ = true;
        }
    }
    back_buffer_as_rt_ = true;
    // 记下本帧实际渲染进的 back buffer 索引（截图回读用，见 end_frame 说明）。
    rendered_buffer_index_ = swapchain_.current_back_buffer_index();

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = use_present_target ? present_rtv_
                                                         : swapchain_.back_buffer_rtv();
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
    // 交换链 RT 恒为单样本。
    current_rt_samples_ = 1;
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
        // 句柄**完全无效**是引擎的约定写法：bind_framebuffer({0,0}) = "回到屏幕"。
        // 这是极高频的正常路径（2D overlay / 后处理收尾都会走），不能刷 WARN ——
        // 否则真正的错误会被淹没。只有"句柄非空却不可用"才是异常。
        if (fb.is_valid()) {
            GLOG_WARN("D3D12::bind_framebuffer: framebuffer not usable (idx={} gen={} ptr={} "
                      "colors={} depth={}) — falling back to the swapchain back buffer",
                      fb.index, fb.generation, static_cast<const void*>(f),
                      f ? f->color_attachment_count() : 0u, (f && f->has_depth()) ? 1 : 0);
        }
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

    // 从"屏幕"切到离屏：这里**不要**去动真实 back buffer。
    // 引擎的屏幕 RT 现在是后端内部的 present 目标（见 ensure_present_target），
    // 真实 back buffer 本帧一直停在 PRESENT 态，对它发 RT→PRESENT 的屏障会与
    // 实际状态不符（调试层每条 ExecuteCommandLists 报一次 ERROR）。
    // present 目标自己的状态由帧末的翻转 blit 统一处理（RT → PIXEL_SHADER_RESOURCE）。
    back_buffer_as_rt_ = false;

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
    // 记录 RT 采样数：PSO 的 SampleDesc.Count / MultisampleEnable 必须与之一致。
    current_rt_samples_ = static_cast<uint32_t>(f->color_sample_count());

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
// MSAA resolve（ResolveSubresource）
//
// D3D12 没有 render pass 式的自动解析（Vulkan 的 pResolveAttachments），也
// 没有 GL 的 glBlitFramebuffer：多样本颜色必须在命令列表上显式
// ResolveSubresource 到单样本纹理，后续 pass 才能像往常一样采样它。
//
// 硬性约束（违反即 E_INVALIDARG / 调试层错误）：
//   * src 必须处于 RENDER_TARGET 态、dst 必须处于 RESOLVE_DEST 态；
//   * 两者同格式、同尺寸，dst 必须单样本；
//   * 命令录在 pass 之外（D3D12 无 render pass 概念，天然满足）。
// ---------------------------------------------------------------------------
void D3D12Backend::resolve_framebuffer(RHIFramebufferHandle src, RHIFramebufferHandle dst) {
    if (!initialized_ || frame_aborted_) return;

    D3D12Framebuffer* src_fb = framebuffer_pool_.get_if_alive(src.index, src.generation);
    D3D12Framebuffer* dst_fb = framebuffer_pool_.get_if_alive(dst.index, dst.generation);
    if (!src_fb || !dst_fb) return;
    // 非多样本源：调用方不应发起；直接返回（无意义且 ResolveSubresource 会拒绝）。
    if (src_fb->color_sample_count() <= 1) return;

    D3D12Texture* src_tex = src_fb->color_texture(0);
    // dst 取解析目标（attach_resolve_texture 指定）；未指定则退回其颜色附件 0。
    D3D12Texture* dst_tex = dst_fb->resolve_texture();
    if (!dst_tex) dst_tex = dst_fb->color_texture(0);
    if (!src_tex || !dst_tex || !src_tex->resource() || !dst_tex->resource()) {
        GLOG_WARN("D3D12Backend::resolve_framebuffer: missing color attachments (src={} dst={})",
                  static_cast<const void*>(src_tex), static_cast<const void*>(dst_tex));
        return;
    }
    if (dst_tex->is_multisampled()) {
        GLOG_ERROR("D3D12Backend::resolve_framebuffer: destination is multisampled");
        return;
    }

    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    auto transition = [cmd](D3D12Texture* tex, D3D12_RESOURCE_STATES after) {
        const D3D12_RESOURCE_STATES before = tex->current_state();
        if (before == after) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex->resource();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        cmd->ResourceBarrier(1, &b);
        tex->set_current_state(after);
    };

    // 1) 多样本源 → RENDER_TARGET（bind_framebuffer 通常已切过，幂等）。
    transition(src_tex, D3D12_RESOURCE_STATE_RENDER_TARGET);
    // 2) 单样本目标 → RESOLVE_DEST。
    transition(dst_tex, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    // 3) 解析。格式取源附件（两者同格式，调用方保证）。
    cmd->ResolveSubresource(dst_tex->resource(), 0, src_tex->resource(), 0,
                            src_tex->dxgi_format());
    // 4) 目标转回 shader 可读：后续 pass 直接采样（ensure_texture_shader_readable
    //    因此变成 no-op，不会补第二条屏障）。
    transition(dst_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
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

// ===========================================================================
// M6：计算着色器 / 间接绘制（GPU 剔除）
// ===========================================================================
// 引擎侧（RenderPipeline）的 GPU 剔除流程，D3D12 必须逐条满足：
//   1. create_compute_shader("cull_instances", dir)   → 计算 PSO + 根签名
//   2. bind_storage_buffer(shader, "<块名>", buffer)  × 6（按 **块名**！）
//   3. dispatch_compute(shader, ceil(n/64), 1, 1)
//   4. bind_instance_stream(dst_models)               → instanced.vert 读 SSBO
//   5. draw_arrays_indirect(mesh, shader, args, off)  → ExecuteIndirect
//   6. copy_texture_to_buffer(hiz_tex, hiz_ssbo, off) → HiZ 金字塔进 SSBO
//
// 与 GL/Vulkan 的两处根本差异（都源自 D3D12 的显式模型）：
//   * **资源状态**必须显式翻转。GL 没有状态、Vulkan 靠 render pass 的 layout
//     传递；D3D12 里"当 UAV 用"必须是 UNORDERED_ACCESS，"当间接参数"必须是
//     INDIRECT_ARGUMENT，猜错轻则调试层报错、重则设备移除。
//   * **间接绘制参数来自缓冲区**（ExecuteIndirect + 命令签名），签名必须与
//     cull_instances.comp 写的 DrawArraysIndirectCommand 布局逐字节一致（16 字节）。
// ---------------------------------------------------------------------------

bool D3D12Backend::supports_compute_shaders() const {
    return initialized_ && device_ != nullptr;
}

RHIShaderHandle D3D12Backend::create_compute_shader(const char* name, const char* shader_dir) {
    if (!supports_compute_shaders()) return RHIShaderHandle{};
    if (!name || !*name || !shader_dir) return RHIShaderHandle{};

    const uint32_t index = compute_pool_.allocate();
    RHIShaderHandle handle{index, compute_pool_.generation(index) | kComputeShaderFlag};
    D3D12ComputeShader* cp = compute_pool_.get_if_alive(index, compute_pool_generation(handle));
    if (!cp) return RHIShaderHandle{};

    cp->set_context(this, device_);
    if (!cp->load(name, shader_dir)) {
        GLOG_WARN("D3D12Backend::create_compute_shader: failed to load '{}.comp' in '{}'",
                  name, shader_dir);
        compute_pool_.deallocate(index, compute_pool_generation(handle));
        return RHIShaderHandle{};
    }
    GLOG_INFO("D3D12Backend: compute pipeline '{}' ready", name);
    return handle;
}

D3D12ComputeShader* D3D12Backend::compute_shader(RHIShaderHandle h) {
    if (!is_compute_shader(h)) return nullptr;
    return compute_pool_.get_if_alive(h.index, compute_pool_generation(h));
}

void D3D12Backend::bind_storage_buffer(RHIShaderHandle shader, const char* name,
                                       RHIBufferHandle buffer) {
    if (!supports_compute_shaders() || !name) return;
    if (!is_compute_shader(shader)) return;   // 图形句柄：SSBO SRV 走 bind_instance_stream
    storage_bindings_[name] = buffer;
}

void D3D12Backend::transition_buffer(D3D12Buffer* b, D3D12_RESOURCE_STATES after) {
    if (!b || !b->resource()) return;
    const D3D12_RESOURCE_STATES before = b->state();
    if (before == after) return;
    // ★ UPLOAD 堆资源常驻 GENERIC_READ，**不能**出现在 barrier 里（GENERIC_READ 是
    //   提升态，不能当 StateBefore；写进去会 E_INVALIDARG）。UPLOAD 缓冲本来就能
    //   直接当 SRV/拷贝源用，无需转换。
    if (before == D3D12_RESOURCE_STATE_GENERIC_READ) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;
    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    bar.Transition.pResource = b->resource();
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = before;
    bar.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &bar);
    b->set_state(after);
}

void D3D12Backend::dispatch_compute(RHIShaderHandle shader, uint32_t gx, uint32_t gy, uint32_t gz) {
    if (!supports_compute_shaders() || frame_aborted_) return;
    D3D12ComputeShader* cp = compute_shader(shader);
    if (!cp || !cp->valid()) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    cp->clear_bindings();
    for (auto& [name, handle] : storage_bindings_) {
        D3D12Buffer* b = buffer_pool_.get_if_alive(handle.index, handle.generation);
        if (!b || !b->resource()) continue;
        // 按该块在 HLSL 里的用途决定目标状态：UAV（写）→ UNORDERED_ACCESS；
        // 只读 → NON_PIXEL_SHADER_RESOURCE（计算着色器不是像素阶段）。
        const SsboBinding* sb = find_ssbo(cp->reflection().bindings, name);
        const D3D12_RESOURCE_STATES want = (sb && sb->is_uav)
                                               ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                               : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        transition_buffer(b, want);
        cp->bind_storage(name.c_str(), b->resource(), b->size());
    }

    if (!cp->dispatch(cmd, gx, gy, gz)) {
        GLOG_WARN("D3D12Backend::dispatch_compute: dispatch failed for '{}'", cp->debug_name());
        return;
    }

    // ★ dispatch 之后必须插一个 UAV 屏障：写入的 DstModels / DrawArgsBuffer 紧接着
    //   要被 **顶点着色器**（实例流 SSBO）和 **ExecuteIndirect**（间接参数）读。
    //   只靠资源状态转换不够 —— UAV 的写完成顺序对后续非 UAV 访问不保证可见。
    //   用 pResource=nullptr 的"全局 UAV 屏障"：保守但一次覆盖全部 UAV。
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    uav.UAV.pResource = nullptr;
    cmd->ResourceBarrier(1, &uav);

    // 清空本帧绑定记录，避免下一次 dispatch 误用上一帧的缓冲。
    storage_bindings_.clear();
}

bool D3D12Backend::ensure_command_signatures() {
    if (sig_draw_ && sig_draw_indexed_) return true;
    if (!device_) return false;

    if (!sig_draw_) {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = sizeof(uint32_t) * 4;   // DrawArraysIndirectCommand = 16B
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &arg;
        desc.NodeMask = 0;
        const HRESULT hr = device_->CreateCommandSignature(
            &desc, nullptr, __mingw_uuidof<ID3D12CommandSignature>(),
            reinterpret_cast<void**>(&sig_draw_));
        if (FAILED(hr)) {
            GLOG_ERROR("D3D12Backend: CreateCommandSignature(DRAW) failed 0x{:08X}",
                       static_cast<uint32_t>(hr));
            sig_draw_ = nullptr;
            return false;
        }
    }
    if (!sig_draw_indexed_) {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = sizeof(uint32_t) * 5;   // DrawElementsIndirectCommand = 20B
        desc.NumArgumentDescs = 1;
        desc.pArgumentDescs = &arg;
        desc.NodeMask = 0;
        const HRESULT hr = device_->CreateCommandSignature(
            &desc, nullptr, __mingw_uuidof<ID3D12CommandSignature>(),
            reinterpret_cast<void**>(&sig_draw_indexed_));
        if (FAILED(hr)) {
            GLOG_ERROR("D3D12Backend: CreateCommandSignature(DRAW_INDEXED) failed 0x{:08X}",
                       static_cast<uint32_t>(hr));
            sig_draw_indexed_ = nullptr;
            return false;
        }
    }
    return true;
}

void D3D12Backend::bind_instance_stream(RHIBufferHandle buffer) {
    instance_stream_ = buffer;
    if (!buffer.is_valid()) return;
    // 可见实例矩阵由 compute 写进 dst_models，顶点着色器按 SSBO 读（uGpuCulled=1）。
    // 它此刻可能仍是 UNORDERED_ACCESS（刚被 compute 写完）→ 必须转到
    // NON_PIXEL_SHADER_RESOURCE（顶点阶段按 SRV 读）。
    D3D12Buffer* b = buffer_pool_.get_if_alive(buffer.index, buffer.generation);
    if (b) transition_buffer(b, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

void D3D12Backend::draw_arrays_indirect(RHIMeshHandle mesh, RHIShaderHandle shader,
                                        RHIBufferHandle args, uint32_t offset) {
    draw_indirect_impl(mesh, shader, args, offset, /*indexed=*/false);
}

void D3D12Backend::draw_indexed_indirect(RHIMeshHandle mesh, RHIShaderHandle shader,
                                         RHIBufferHandle args, uint32_t offset) {
    draw_indirect_impl(mesh, shader, args, offset, /*indexed=*/true);
}

void D3D12Backend::draw_indirect_impl(RHIMeshHandle mesh_handle, RHIShaderHandle shader_handle,
                                      RHIBufferHandle args_handle, uint32_t offset, bool indexed) {
    if (!initialized_ || frame_aborted_) return;

    D3D12Mesh* m = mesh_pool_.get_if_alive(mesh_handle.index, mesh_handle.generation);
    D3D12Shader* s = shader_pool_.get_if_alive(shader_handle.index, shader_handle.generation);
    D3D12Buffer* args = buffer_pool_.get_if_alive(args_handle.index, args_handle.generation);
    if (!m || !s || !s->is_valid() || !args || !args->resource()) return;
    if (!ensure_command_signatures()) return;

    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    s->ensure_frame();

    const D3D12_VERTEX_BUFFER_VIEW vb = m->vertex_view();
    const D3D12_INDEX_BUFFER_VIEW ib = m->index_view();
    if (vb.BufferLocation == 0 || !m->has_layout()) {
        GLOG_WARN("D3D12::draw_arrays_indirect: mesh missing vertex buffer/layout; skipping");
        return;
    }
    if (indexed && ib.BufferLocation == 0) {
        GLOG_WARN("D3D12::draw_indexed_indirect: mesh has no index buffer; skipping");
        return;
    }

    PsoKey key;
    key.layout = make_layout_key(m->layout(), m->instance_layout());
    key.rt_format = static_cast<uint32_t>(current_rt_format_);
    key.ds_format = static_cast<uint32_t>(current_ds_format_);
    key.samples = static_cast<uint8_t>(current_rt_samples_);
    key.depth_test = depth_test_ ? 1 : 0;
    key.depth_write = (s->skybox() ? false : depth_write_) ? 1 : 0;
    key.blend = blend_ ? (s->blending() ? 1 : 0) : 0;
    key.cull = (cull_ == CullMode::None) ? 1 : (cull_ == CullMode::Front ? 2 : 0);
    key.primitive_topology = 0;
    key.instance_stream = (m->instance_count() > 0) ? 1 : 0;

    ID3D12PipelineState* pso = s->pipeline_for(key);
    if (!pso) {
        GLOG_ERROR("D3D12::draw_arrays_indirect: no pipeline for this state/layout");
        return;
    }

    cmd->SetGraphicsRootSignature(s->root_signature());
    cmd->SetPipelineState(pso);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    bind_vertex_slots(cmd, m);
    if (indexed) cmd->IASetIndexBuffer(&ib);

    // ★ 把"可见实例矩阵 SSBO"作为 SRV 绑到 instanced.vert 的 DstModels 寄存器。
    //   必须在 commit_textures() 之前登记 —— 表的构建/复用路径会把它写进槽位。
    D3D12Buffer* stream = instance_stream_.is_valid()
                              ? buffer_pool_.get_if_alive(instance_stream_.index,
                                                          instance_stream_.generation)
                              : nullptr;
    if (stream && stream->resource()) {
        s->set_external_buffer_srv("DstModels", stream->resource(), stream->size());
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

    // 间接参数缓冲必须是 INDIRECT_ARGUMENT 态（不能直接拿 UAV 态用）。
    transition_buffer(args, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    ID3D12CommandSignature* sig = indexed ? sig_draw_indexed_ : sig_draw_;
    cmd->ExecuteIndirect(sig, 1, args->resource(), offset, nullptr, 0);
}

// ---------------------------------------------------------------------------
// 纹理 → SSBO（HiZ 金字塔）
// ---------------------------------------------------------------------------
// ★ 为什么不能"一次 GPU 拷贝搞定"（这是 D3D12 与 GL/Vulkan 的实质差异）：
//   引擎的 HiZ SSBO 布局是**逐行紧密**的（row = width * sizeof(R32F)，着色器里
//   `hiz[offset + j*width + i]` 直接索引），而 D3D12 的 CopyTextureRegion 要求
//   目标 footprint 的 RowPitch 是 **256 字节的整数倍**（D3D12_TEXTURE_DATA_PITCH_
//   ALIGNMENT）。viewport 宽度取 2 的幂递减后几乎不可能满足（320→160 就是 640 字节，
//   不是 256 的倍数）→ 整块拷贝必然写进带填充的行距，着色器的行索引随之错位。
//   GL（glReadPixels→PBO）与 Vulkan（vkCmdCopyImageToBuffer 允许紧凑 rowLength）
//   都没有这个限制，所以只有 D3D12 需要绕。
//
// 做法：把纹理先按 256 对齐的行距拷进一块 READBACK 暂存，**下一帧**（GPU 已跑完、
// 有围栏保证）在 CPU 上重排成紧凑行，再用 UploadRing 拷进目标 SSBO。多一帧延迟
// 对 HiZ 无影响 —— 金字塔本来就允许滞后（引擎自己也是分帧逐级构建的）。
void D3D12Backend::copy_texture_to_buffer(RHITextureHandle texture, RHIBufferHandle buffer,
                                          uint32_t offset) {
    if (!initialized_ || frame_aborted_) return;
    D3D12Texture* t = texture_pool_.get_if_alive(texture.index, texture.generation);
    D3D12Buffer* b = buffer_pool_.get_if_alive(buffer.index, buffer.generation);
    if (!t || !b || !t->resource() || !b->resource()) return;
    const uint32_t w = static_cast<uint32_t>(t->width());
    const uint32_t h = static_cast<uint32_t>(t->height());
    if (w == 0 || h == 0) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;

    const uint32_t pitch = static_cast<uint32_t>(align_up(static_cast<size_t>(w) * 4u, 256));
    const uint64_t staging_size = static_cast<uint64_t>(pitch) * h;

    // READBACK 暂存。
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = staging_size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* staging = nullptr;
    HRESULT hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  __mingw_uuidof<ID3D12Resource>(),
                                                  reinterpret_cast<void**>(&staging));
    if (FAILED(hr) || !staging) {
        GLOG_ERROR("copy_texture_to_buffer: staging alloc failed 0x{:08X}", static_cast<uint32_t>(hr));
        return;
    }

    // 源纹理状态：本帧刚被渲染成 RT → COPY_SOURCE → 还原。
    const D3D12_RESOURCE_STATES tex_before = t->current_state();
    auto tex_barrier = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER bar{};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = t->resource();
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore = from;
        bar.Transition.StateAfter = to;
        cmd->ResourceBarrier(1, &bar);
    };
    if (tex_before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        tex_barrier(tex_before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = staging;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = w;
    dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = t->resource();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (tex_before != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        tex_barrier(D3D12_RESOURCE_STATE_COPY_SOURCE, tex_before);
    }

    // 本帧提交后的围栏值 → 下一帧 drain 时用它判断 GPU 是否跑完。
    PendingHizCopy p;
    p.staging = staging;
    p.w = w;
    p.h = h;
    p.pitch = pitch;
    p.dst = buffer;
    p.dst_offset = offset;
    p.fence = swapchain_.last_submitted_value() + 1;
    pending_hiz_.push_back(p);
}

// 在 begin_frame 里调用：把已完成（GPU 跑完）的 HiZ 暂存重排成紧凑行并拷进 SSBO。
void D3D12Backend::drain_hiz_repacks() {
    if (pending_hiz_.empty()) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;
    const uint64_t done = swapchain_.completed_value();

    for (size_t i = 0; i < pending_hiz_.size();) {
        PendingHizCopy& p = pending_hiz_[i];
        if (done < p.fence) { ++i; continue; }

        const size_t tight_row = static_cast<size_t>(p.w) * 4u;
        const size_t tight_size = tight_row * p.h;
        std::vector<uint8_t> tight(tight_size);
        D3D12_RANGE range{0, static_cast<SIZE_T>(static_cast<uint64_t>(p.pitch) * p.h)};
        void* mapped = nullptr;
        if (SUCCEEDED(p.staging->Map(0, &range, &mapped)) && mapped) {
            const uint8_t* s = static_cast<const uint8_t*>(mapped);
            for (uint32_t y = 0; y < p.h; ++y) {
                std::memcpy(tight.data() + static_cast<size_t>(y) * tight_row,
                            s + static_cast<size_t>(y) * p.pitch, tight_row);
            }
            const D3D12_RANGE written{0, 0};
            p.staging->Unmap(0, &written);

            D3D12Buffer* b = buffer_pool_.get_if_alive(p.dst.index, p.dst.generation);
            if (b && b->resource()) {
                D3D12_RESOURCE_STATES state = b->state();
                if (record_copy(b->resource(), p.dst_offset, tight.data(), tight_size, state)) {
                    b->set_state(state);
                }
                // 失败（上传环单段装不下）时只能丢掉这一层级 —— 容量已按
                // "一帧 HiZ + 常量/实例数据" 定标，正常不会再走到这里；
                // record_copy 内部会打 WARN。**不能**保留重试：留下的一条会在
                // 下一帧继续抢占整段，把该帧常量/实例数据的空间挤光，雪崩成
                // 满屏 "upload ring exhausted"。
            }
        }
        p.staging->Release();
        pending_hiz_.erase(pending_hiz_.begin() + static_cast<long>(i));
    }
}

// D3D12Buffer::UploadSink：CPU → DEFAULT 堆 SSBO。拷贝录制进当前帧命令列表，
// 不提交、不等待（每帧多次调用也不会产生 GPU 停顿）。
bool D3D12Backend::record_copy(ID3D12Resource* dst, uint64_t dst_offset, const void* data,
                               size_t size, D3D12_RESOURCE_STATES& inout_state) {
    if (!dst || !data || size == 0) return false;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd || !upload_ring_.is_valid()) return false;

    size_t ring_offset = 0, aligned = 0;
    uint8_t* p = upload_ring_.allocate(size, ring_offset, aligned);
    if (!p) {
        GLOG_WARN("D3D12Backend::record_copy: upload ring exhausted ({} bytes)", size);
        return false;
    }
    std::memcpy(p, data, size);

    auto barrier = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER bar{};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = dst;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bar.Transition.StateBefore = from;
        bar.Transition.StateAfter = to;
        cmd->ResourceBarrier(1, &bar);
    };

    const bool need_barrier = inout_state != D3D12_RESOURCE_STATE_COPY_DEST &&
                              inout_state != D3D12_RESOURCE_STATE_GENERIC_READ;
    if (need_barrier) barrier(inout_state, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyBufferRegion(dst, dst_offset, upload_ring_.resource(), ring_offset, size);
    if (need_barrier) barrier(D3D12_RESOURCE_STATE_COPY_DEST, inout_state);
    return true;
}

RenderBackendCapabilities D3D12Backend::get_capabilities() const {
    // M3 阶段：交换链 / clear / present / 资源层（buffer/texture/mesh/framebuffer）
    // 已可用；着色器与管线状态要等 M4。
    RenderBackendCapabilities caps{};
    caps.supports_compute              = true;   // M6：compute 剔除 + 间接绘制
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
    // D3D12 强制 OMSetRenderTargets 的 DSV 与全部 RTV 采样数一致
    // （spec / debug layer：multisample count and quality must match），
    // 与 OpenGL 同属"深度必须同采样数"阵营；Vulkan 允许混合。
    caps.msaa_depth_must_match_samples = true;
    // MSAA 上限：对 HDR 颜色格式逐级查询质量级别（NumQualityLevels>0 即支持）。
    // 标准多重采样模式恒占质量级别 0，Query Quality=0 总是成立。
    caps.max_msaa_color_samples        = 1;
    if (device_) {
        static const DXGI_FORMAT kMsaaFormats[] = {
            DXGI_FORMAT_R16G16B16A16_FLOAT,  // HDR 主目标（MSAA 的实际用途）
            DXGI_FORMAT_R8G8B8A8_UNORM,      // LDR / 交换链等价格式
        };
        for (DXGI_FORMAT fmt : kMsaaFormats) {
            for (UINT count = 16; count >= 2; count /= 2) {
                D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{};
                q.Format = fmt;
                q.SampleCount = count;
                q.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
                if (SUCCEEDED(device_->CheckFeatureSupport(
                        D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q))) &&
                    q.NumQualityLevels > 0) {
                    caps.max_msaa_color_samples = count;
                    break;
                }
            }
            if (caps.max_msaa_color_samples > 1) break;
        }
    }
    return caps;
}

// ---------------------------------------------------------------------------
// GPU 分段计时（D3D12 时间戳查询）
//
// 为什么这套计时对当前调试特别重要：D3D12 在 frame 5 会 DEVICE_HUNG（TDR）。
// TDR 是"GPU 某个 pass 执行超时（默认 2s）被驱动重置"，而 CPU 侧日志只能看到
// 提交了哪些命令，看不到 GPU 真正卡在哪一段。时间戳查询可以在设备被移除后，
// 把"最后一次成功推进到的时间戳"读出来 —— 那个段就是挂起点。
//
// 实现要点：
//   * 一个 QueryHeap(TIMESTAMP) 覆盖全部槽，每槽 k_gpu_ts_per_slot 个查询；
//   * begin/end 只做 vkCmdWriteTimestamp 的等价物：
//       cmd->EndQuery(heap, TIMESTAMP, idx)   —— 无 GPU 停顿；
//   * 结算放在"槽被复用"时：先把该槽的查询 ResolveQueryData 到一段 READBACK
//     缓冲，再 Map 读取。此时 GPU 已跑过那帧（帧槽轮转 + 围栏保证），无阻塞。
//   * 设备挂起后 ResolveQueryData/Map 仍可用（它们是 CPU 侧引擎命令），
//     因此 dump 仍能给出"最后完成到哪一段"的证据。
// ---------------------------------------------------------------------------
bool D3D12Backend::ensure_gpu_profiling_objects() {
    if (gpu_profiling_ready_) return true;
    if (!device_ || !queue_) return false;

    // 时间戳频率（次/秒）。用于把 tick 差换算成毫秒。
    if (gpu_timestamp_freq_ == 0) {
        if (FAILED(queue_->GetTimestampFrequency(&gpu_timestamp_freq_)) ||
            gpu_timestamp_freq_ == 0) {
            gpu_timestamp_freq_ = 1;   // 退化：至少不除零
        }
    }

    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = static_cast<UINT>(k_gpu_slots * k_gpu_ts_per_slot);
    qd.NodeMask = 0;
    if (FAILED(device_->CreateQueryHeap(&qd, __mingw_uuidof<ID3D12QueryHeap>(),
                                        reinterpret_cast<void**>(&gpu_query_heap_)))) {
        GLOG_WARN("D3D12Backend: CreateQueryHeap(TIMESTAMP) failed; GPU profiling disabled");
        gpu_query_heap_ = nullptr;
        return false;
    }

    // READBACK 缓冲：每槽存 "时间戳值 + 可用标志"（D3D12 无 availability 标志，
    // 这里只存值，可用性由帧槽轮转保证）。
    const UINT64 bytes = sizeof(uint64_t) *
                         static_cast<UINT64>(k_gpu_slots * k_gpu_ts_per_slot);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    hp.CreationNodeMask = 1;
    hp.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            __mingw_uuidof<ID3D12Resource>(), reinterpret_cast<void**>(&gpu_readback_)))) {
        GLOG_WARN("D3D12Backend: GPU profiling readback buffer creation failed; disabled");
        gpu_query_heap_->Release();
        gpu_query_heap_ = nullptr;
        return false;
    }
    for (auto& f : gpu_slot_fence_) f = 0;
    gpu_profiling_ready_ = true;
    GLOG_INFO("D3D12Backend: GPU timestamp profiling ready (freq={} Hz)",
              gpu_timestamp_freq_);
    return true;
}

void D3D12Backend::release_gpu_profiling_objects() {
    if (gpu_flush_fence_) { gpu_flush_fence_->Release(); gpu_flush_fence_ = nullptr; }
    if (gpu_readback_) { gpu_readback_->Release(); gpu_readback_ = nullptr; }
    if (gpu_query_heap_) { gpu_query_heap_->Release(); gpu_query_heap_ = nullptr; }
    gpu_profiling_ready_ = false;
}

void D3D12Backend::set_gpu_profiling(bool enabled) {
    if (enabled == gpu_profiling_enabled_) return;
    gpu_profiling_enabled_ = enabled;
    gpu_pending_scope_ = -1;
    gpu_stats_.reset();
    for (auto& c : gpu_scope_count_) c = 0;
    if (enabled) {
        if (!ensure_gpu_profiling_objects()) {
            gpu_profiling_enabled_ = false;
            return;
        }
        gpu_slot_ = 0;
    }
    GLOG_INFO("D3D12Backend: GPU profiling {}{}", enabled ? "enabled" : "disabled",
              gpu_profiling_ready_ ? "" : " (unavailable)");
}

void D3D12Backend::gpu_profile_begin(const char* name) {
    if (!gpu_profiling_enabled_ || !gpu_profiling_ready_) return;
    if (gpu_pending_scope_ >= 0) {
        if (!gpu_nested_warned_) {
            gpu_nested_warned_ = true;
            GLOG_WARN("D3D12Backend: gpu_profile_begin('{}') 嵌套调用被忽略（计时区间必须平铺）",
                      name ? name : "?");
        }
        return;
    }
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (!cmd) return;
    const int count = gpu_scope_count_[gpu_slot_];
    if (count >= k_gpu_max_scopes) return;
    const UINT idx = static_cast<UINT>(gpu_slot_ * k_gpu_ts_per_slot + count * 2);
    cmd->EndQuery(gpu_query_heap_, D3D12_QUERY_TYPE_TIMESTAMP, idx);
    gpu_scope_names_[gpu_slot_][count] = name ? name : "?";
    gpu_scope_count_[gpu_slot_] = count + 1;
    gpu_pending_scope_ = count;
}

void D3D12Backend::gpu_profile_end() {
    if (gpu_pending_scope_ < 0 || !gpu_profiling_ready_) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (cmd) {
        const UINT idx = static_cast<UINT>(gpu_slot_ * k_gpu_ts_per_slot +
                                           gpu_pending_scope_ * 2 + 1);
        cmd->EndQuery(gpu_query_heap_, D3D12_QUERY_TYPE_TIMESTAMP, idx);
    }
    gpu_pending_scope_ = -1;
}

void D3D12Backend::gpu_profile_frame_boundary() {
    if (!gpu_profiling_enabled_ || !gpu_profiling_ready_) return;
    // 上一帧若有区间未闭合（提前 return 等），丢弃挂起状态，保证配对不错位。
    gpu_pending_scope_ = -1;

    // 当前帧槽已录完，把它的查询区间在本帧命令列表里 Resolve 到 READBACK 缓冲。
    // 这不阻塞 GPU（只是一个 GPU→GPU 拷贝命令），读回留到后面"槽被复用"时。
    const int cur = gpu_slot_;
    const int count = gpu_scope_count_[cur];
    if (count > 0) {
        ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
        if (cmd) {
            const UINT first = static_cast<UINT>(cur * k_gpu_ts_per_slot);
            const UINT n = static_cast<UINT>(count) * 2;
            cmd->ResolveQueryData(gpu_query_heap_, D3D12_QUERY_TYPE_TIMESTAMP, first, n,
                                  gpu_readback_,
                                  static_cast<UINT64>(cur * k_gpu_ts_per_slot) *
                                      sizeof(uint64_t));
            gpu_slot_fence_[cur] = swapchain_.last_submitted_value() + 1;
        }
    }

    // 推进到下一个槽，并结算"刚好要被覆盖的那个槽"（GPU 必然已跑完它）。
    const int next = (cur + 1) % k_gpu_slots;
    const int done = next;
    const int done_count = gpu_scope_count_[done];
    if (done_count > 0 && gpu_slot_fence_[done] != 0 &&
        swapchain_.completed_value() >= gpu_slot_fence_[done]) {
        void* mapped = nullptr;
        D3D12_RANGE range{ static_cast<SIZE_T>(done * k_gpu_ts_per_slot) * sizeof(uint64_t),
                           static_cast<SIZE_T>(done * k_gpu_ts_per_slot + k_gpu_ts_per_slot) *
                               sizeof(uint64_t) };
        if (SUCCEEDED(gpu_readback_->Map(0, &range, &mapped)) && mapped) {
            const uint64_t* p = static_cast<const uint64_t*>(mapped);
            const double freq = static_cast<double>(gpu_timestamp_freq_);
            for (int i = 0; i < done_count; ++i) {
                const uint64_t t0 = p[done * k_gpu_ts_per_slot + i * 2];
                const uint64_t t1 = p[done * k_gpu_ts_per_slot + i * 2 + 1];
                if (t1 <= t0) continue;
                const double ms = static_cast<double>(t1 - t0) / freq * 1e3;
                gpu_stats_.add(gpu_scope_names_[done][i], ms);
            }
            D3D12_RANGE empty{ 0, 0 };
            gpu_readback_->Unmap(0, &empty);
        }
        gpu_slot_fence_[done] = 0;
    }
    gpu_scope_count_[done] = 0;
    gpu_slot_ = next;
    gpu_stats_.set_frames(gpu_stats_.frames() + 1);
}

void D3D12Backend::gpu_profile_dump() {
    if (!gpu_profiling_enabled_) {
        std::fprintf(stdout, "[gpu] 未开启 GPU 计时（GRender_SetGPUProfiling(1) / --bench）\n");
        return;
    }
    // 设备可能已挂起：仍把已有的样本打出来 —— 最后一个有值的段就是 TDR 挂起点。
    if (device_lost_) {
        std::fprintf(stdout, "[gpu] (device lost; 以下为挂起前已结算的样本)\n");
        // 挂起时"正在飞"的槽还没经过轮转结算，这里就地补一次 Resolve+读回。
        gpu_profile_flush_pending();
    }
    gpu_stats_.print("D3D12");
}

// 设备挂起后把还挂在飞的所有槽立刻 Resolve + 读回。这些槽的时间戳是
// 定位 TDR 的关键证据：设备移除后 GPU 时间戳停止推进，所以"最后一个有值的
// 段"就是挂起前完成到的位置，其后的段全为 0（= GPU 卡在这里）。
void D3D12Backend::gpu_profile_flush_pending() {
    if (!gpu_profiling_ready_) return;
    ID3D12GraphicsCommandList* cmd = swapchain_.command_list();
    if (cmd) {
        for (int s = 0; s < k_gpu_slots; ++s) {
            const int n = gpu_scope_count_[s];
            if (n <= 0) continue;
            const UINT first = static_cast<UINT>(s * k_gpu_ts_per_slot);
            cmd->ResolveQueryData(gpu_query_heap_, D3D12_QUERY_TYPE_TIMESTAMP, first,
                                  static_cast<UINT>(n) * 2, gpu_readback_,
                                  static_cast<UINT64>(first) * sizeof(uint64_t));
        }
        // 提交一次让 Resolve 生效（设备已挂起时 ExecuteCommandLists 只是空转，
        // 不报错；关键是让 READBACK 缓冲拿到 CPU 可读内容）。
        ID3D12CommandList* lists[] = { cmd };
        queue_->ExecuteCommandLists(1, lists);
    }
    // 给驱动一点时间把 Resolve 结果落进 READBACK 缓冲（设备挂起路径允许阻塞）。
    if (gpu_flush_fence_ == nullptr) {
        device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, __mingw_uuidof<ID3D12Fence>(),
                             reinterpret_cast<void**>(&gpu_flush_fence_));
    }
    if (gpu_flush_fence_) {
        const UINT64 v = ++gpu_flush_fence_value_;
        if (SUCCEEDED(queue_->Signal(gpu_flush_fence_, v))) {
            for (int i = 0; i < 200; ++i) {
                if (gpu_flush_fence_->GetCompletedValue() >= v) break;
                Sleep(5);
            }
        }
    }
    const double freq = static_cast<double>(gpu_timestamp_freq_ ? gpu_timestamp_freq_ : 1);
    for (int s = 0; s < k_gpu_slots; ++s) {
        const int n = gpu_scope_count_[s];
        if (n <= 0) continue;
        void* mapped = nullptr;
        D3D12_RANGE range{ static_cast<SIZE_T>(s * k_gpu_ts_per_slot) * sizeof(uint64_t),
                           static_cast<SIZE_T>(s * k_gpu_ts_per_slot + k_gpu_ts_per_slot) *
                               sizeof(uint64_t) };
        if (SUCCEEDED(gpu_readback_->Map(0, &range, &mapped)) && mapped) {
            const uint64_t* p = static_cast<const uint64_t*>(mapped);
            // 打印原始时间戳，便于肉眼分辨"哪一段开始全 0"。
            for (int i = 0; i < n; ++i) {
                const uint64_t t0 = p[s * k_gpu_ts_per_slot + i * 2];
                const uint64_t t1 = p[s * k_gpu_ts_per_slot + i * 2 + 1];
                const double ms = (t1 > t0) ? static_cast<double>(t1 - t0) / freq * 1e3 : 0.0;
                if (t1 > t0) gpu_stats_.add(gpu_scope_names_[s][i], ms);
                // t0 有值、t1 没值 = 这段开始执行但从未结束 → TDR 挂起点。
                // （上面注释说"打印原始时间戳便于分辨哪一段开始全 0"，这里把
                //   结论直接点明：没结束的那一段才是真正卡住的 pass。）
                else if (t0 != 0) {
                    std::fprintf(stdout, "[gpu] 挂起点: slot=%d scope='%s' (t0=%llu t1=%llu)\n",
                                 s, gpu_scope_names_[s][i].c_str(),
                                 static_cast<unsigned long long>(t0),
                                 static_cast<unsigned long long>(t1));
                }
            }
            D3D12_RANGE empty{ 0, 0 };
            gpu_readback_->Unmap(0, &empty);
        }
        gpu_scope_count_[s] = 0;
    }
}


void D3D12Backend::gpu_profile_reset() {
    gpu_stats_.reset();
    for (auto& c : gpu_scope_count_) c = 0;
}

double D3D12Backend::gpu_frame_time_ms() const {
    return gpu_profiling_enabled_ ? gpu_stats_.total_ms_per_frame() : 0.0;
}

} // namespace gryce_engine::render::d3d12
