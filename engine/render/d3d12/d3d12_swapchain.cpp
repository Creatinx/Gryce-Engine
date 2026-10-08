#include "d3d12_swapchain.h"

#include <algorithm>

#include "utils/glog/glog_lib.h"

// MinGW 的 winerror.h 没有定义 Present(DXGI_PRESENT_DO_NOT_WAIT) 的"仍在绘制"
// 状态码（DXGI_STATUS_WAS_STILL_DRAWING = 0x087A0001，见 DXGI 文档），本地补上。
#ifndef DXGI_STATUS_WAS_STILL_DRAWING
#define DXGI_STATUS_WAS_STILL_DRAWING _HRESULT_TYPEDEF_(0x087A0001L)
#endif

namespace gryce_engine::render::d3d12 {

bool D3D12Swapchain::init(Loader& loader, ID3D12Device* device, ID3D12CommandQueue* queue,
                          HWND hwnd, uint32_t width, uint32_t height, uint32_t frame_count) {
    loader_ = &loader;
    device_ = device;
    queue_ = queue;
    hwnd_ = hwnd;
    width_ = std::max(1u, width);
    height_ = std::max(1u, height);
    // FLIP_DISCARD 最少 2 个 buffer；3 个可让 CPU 与 GPU 更好重叠。
    frame_count_ = std::clamp(frame_count, 2u, 3u);

    if (!create_swapchain()) return false;
    if (!create_rtv_heap()) return false;
    if (!create_command_objects()) return false;
    if (!create_fence()) return false;
    current_index_ = swapchain_->GetCurrentBackBufferIndex();
    current_frame_ = 0;
    GLOG_INFO("D3D12Swapchain: {}x{}, {} buffers, FLIP_DISCARD",
              width_, height_, frame_count_);
    return true;
}

void D3D12Swapchain::shutdown() {
    wait_all();
    release_target_objects();

    if (command_list_) { command_list_->Release(); command_list_ = nullptr; }
    for (auto* a : allocators_) { if (a) a->Release(); }
    allocators_.clear();
    frame_fence_values_.clear();

    if (fence_) { fence_->Release(); fence_ = nullptr; }
    if (fence_event_) { CloseHandle(fence_event_); fence_event_ = nullptr; }
    if (swapchain_) { swapchain_->Release(); swapchain_ = nullptr; }

    loader_ = nullptr;
    device_ = nullptr;
    queue_ = nullptr;
    hwnd_ = nullptr;
    next_fence_value_ = 1;
}

// 创建 IDXGISwapChain3（含 back buffer 引用）。不涉及 RTV 堆。
bool D3D12Swapchain::create_swapchain() {
    if (!loader_ || !loader_->factory || !queue_ || !hwnd_) return false;

    // FLIP_DISCARD 是 D3D12 唯一正经的 flip 模型。
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // 与 GL/VK 侧后端目标格式一致
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;                 // MSAA 不在本阶段范围
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = frame_count_;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = 0;

    // mingw-w64 头里 CreateSwapChainForHwnd 是**无 riid 的旧式签名**，
    // 返回 IDXGISwapChain1*（与 MSVC SDK 的带 riid 版本不同）。
    IDXGISwapChain1* chain1 = nullptr;
    HRESULT hr = loader_->factory->CreateSwapChainForHwnd(
        queue_, hwnd_, &desc, nullptr, nullptr, &chain1);
    if (FAILED(hr) || !chain1) {
        GLOG_ERROR("D3D12Swapchain: CreateSwapChainForHwnd failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        return false;
    }

    // 升级到 IDXGISwapChain3（要用 GetCurrentBackBufferIndex）
    hr = chain1->QueryInterface(__mingw_uuidof<IDXGISwapChain3>(),
                                reinterpret_cast<void**>(&swapchain_));
    chain1->Release();
    if (FAILED(hr) || !swapchain_) {
        GLOG_ERROR("D3D12Swapchain: QueryInterface<IDXGISwapChain3> failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        return false;
    }

    // 取 back buffer 引用（FLIP 模型下 Present 后指针不变，只需拿一次）
    if (!acquire_back_buffers()) return false;
    return true;
}

// 取 back buffer 引用（init 与 recreate 共用）。调用方需保证 back_buffers_ 已清空。
bool D3D12Swapchain::acquire_back_buffers() {
    back_buffers_.assign(frame_count_, nullptr);
    for (uint32_t i = 0; i < frame_count_; ++i) {
        HRESULT hr = swapchain_->GetBuffer(i, __mingw_uuidof<ID3D12Resource>(),
                                           reinterpret_cast<void**>(&back_buffers_[i]));
        if (FAILED(hr)) {
            GLOG_ERROR("D3D12Swapchain: GetBuffer({}) failed (hr=0x{:08x})",
                       i, static_cast<unsigned>(hr));
            return false;
        }
    }
    return true;
}

bool D3D12Swapchain::create_rtv_heap() {
    if (rtv_heap_) { rtv_heap_->Release(); rtv_heap_ = nullptr; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = frame_count_;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;  // RTV 堆**不能**是 SHADER_VISIBLE
    hd.NodeMask = 0;
    HRESULT hr = device_->CreateDescriptorHeap(&hd, __mingw_uuidof<ID3D12DescriptorHeap>(),
                                               reinterpret_cast<void**>(&rtv_heap_));
    if (FAILED(hr)) {
        GLOG_ERROR("D3D12Swapchain: CreateDescriptorHeap(RTV) failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        return false;
    }
    rtv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < frame_count_; ++i) {
        device_->CreateRenderTargetView(back_buffers_[i], nullptr, rtv);
        rtv.ptr += rtv_descriptor_size_;
    }
    return true;
}

bool D3D12Swapchain::create_command_objects() {
    allocators_.assign(frame_count_, nullptr);
    frame_fence_values_.assign(frame_count_, 0);

    for (uint32_t i = 0; i < frame_count_; ++i) {
        HRESULT hr = device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            __mingw_uuidof<ID3D12CommandAllocator>(),
            reinterpret_cast<void**>(&allocators_[i]));
        if (FAILED(hr)) {
            GLOG_ERROR("D3D12Swapchain: CreateCommandAllocator({}) failed (hr=0x{:08x})",
                       i, static_cast<unsigned>(hr));
            return false;
        }
    }

    // CreateCommandList 返回时列表已处于 recording 态，必须先 Close()，
    // 否则第一次 allocator->Reset() 会失败（M0 踩过）。
    HRESULT hr = device_->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[0], nullptr,
        __mingw_uuidof<ID3D12GraphicsCommandList>(),
        reinterpret_cast<void**>(&command_list_));
    if (FAILED(hr)) {
        GLOG_ERROR("D3D12Swapchain: CreateCommandList failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        return false;
    }
    command_list_->Close();
    return true;
}

bool D3D12Swapchain::create_fence() {
    HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                      __mingw_uuidof<ID3D12Fence>(),
                                      reinterpret_cast<void**>(&fence_));
    if (FAILED(hr)) {
        GLOG_ERROR("D3D12Swapchain: CreateFence failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        return false;
    }
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        GLOG_ERROR("D3D12Swapchain: CreateEventW failed (err={})", GetLastError());
        return false;
    }
    return true;
}

bool D3D12Swapchain::recreate(uint32_t width, uint32_t height) {
    if (!swapchain_) return false;
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (width == width_ && height == height_) return true;

    // 重建前必须让所有在途帧完成：back buffer 可能仍被 GPU 引用。
    wait_all();
    release_target_objects();

    width_ = width;
    height_ = height;

    HRESULT hr = swapchain_->ResizeBuffers(frame_count_, width_, height_,
                                           DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        GLOG_ERROR("D3D12Swapchain::recreate: ResizeBuffers failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        return false;
    }

    // 重新取 back buffer 引用并重建 RTV 堆
    if (!acquire_back_buffers()) return false;
    if (!create_rtv_heap()) return false;

    current_index_ = swapchain_->GetCurrentBackBufferIndex();
    GLOG_INFO("D3D12Swapchain: recreated {}x{}", width_, height_);
    return true;
}

void D3D12Swapchain::release_target_objects() {
    for (auto*& bb : back_buffers_) {
        if (bb) { bb->Release(); bb = nullptr; }
    }
    back_buffers_.clear();
    if (rtv_heap_) { rtv_heap_->Release(); rtv_heap_ = nullptr; }
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Swapchain::back_buffer_rtv() const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(current_index_) * rtv_descriptor_size_;
    return h;
}

bool D3D12Swapchain::begin_frame() {
    if (!device_ || !command_list_ || allocators_.empty()) return false;

    // 等本槽上一轮 GPU 完成（首次 frame_fence_values_ 为 0，无需等待）
    if (frame_fence_values_[current_frame_] != 0 &&
        fence_->GetCompletedValue() < frame_fence_values_[current_frame_]) {
        HRESULT hr = fence_->SetEventOnCompletion(frame_fence_values_[current_frame_], fence_event_);
        if (FAILED(hr)) {
            GLOG_ERROR("D3D12Swapchain: SetEventOnCompletion failed (hr=0x{:08x})",
                       static_cast<unsigned>(hr));
            return false;
        }
        WaitForSingleObject(fence_event_, INFINITE);
    }

    HRESULT hr = allocators_[current_frame_]->Reset();
    if (FAILED(hr)) {
        GLOG_ERROR("D3D12Swapchain: allocator Reset failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        return false;
    }
    hr = command_list_->Reset(allocators_[current_frame_], nullptr);
    if (FAILED(hr)) {
        GLOG_ERROR("D3D12Swapchain: command_list Reset failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        return false;
    }

    current_index_ = swapchain_->GetCurrentBackBufferIndex();
    recording_ = true;
    return true;
}

bool D3D12Swapchain::end_frame() {
    if (!recording_) return false;
    recording_ = false;

    HRESULT hr = command_list_->Close();
    if (FAILED(hr)) {
        GLOG_ERROR("D3D12Swapchain: command_list Close failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        return false;
    }

    ID3D12CommandList* lists[] = { command_list_ };
    queue_->ExecuteCommandLists(1, lists);

    // ★ Present 绝不能无界阻塞调用线程（这里是**渲染线程**）。
    //
    //   为什么（实测的 DEVICE_HUNG 根因）：vsync 下 `Present(1, 0)` 会一直等到
    //   下一次 vblank。而窗口消息泵在**主线程**；主线程此刻可能正堵在
    //   RenderCommandBuffer::submit() 的 cv_main_.wait（三缓冲被占满，等渲染
    //   线程归还）。于是两边互等：渲染线程等 vblank（依赖消息泵推进），主线程
    //   等渲染线程 → 死锁超过 2 秒 → TDR(DXGI_ERROR_DEVICE_HUNG)。
    //   加了 SSR 后帧变慢，三缓冲被占满的概率骤增，所以表现为"开 SSR 就挂、
    //   关 vsync 就正常"（关 vsync 时 Present 不阻塞，死锁不成立）。
    //
    //   修法：用 DXGI_PRESENT_DO_NOT_WAIT，Present 立即返回；若返回
    //   DXGI_STATUS_WAS_STILL_DRAWING 说明本帧还没消费完，短暂等待后重试，
    //   但重试次数有界（总等待 ≪ TDR 阈值），保证渲染线程不会长时间卡死。
    const UINT present_flags =
        vsync_ ? DXGI_PRESENT_DO_NOT_WAIT : 0u;
    for (int attempt = 0; ; ++attempt) {
        hr = swapchain_->Present(vsync_ ? 1 : 0, present_flags);
        if (hr != DXGI_STATUS_WAS_STILL_DRAWING) break;
        // 本帧尚未展示：让出 CPU 一小会再试（上限 ~200ms，远小于 2s TDR）。
        if (attempt >= 20) {
            // 兜底：放弃带标志的重试，退化为阻塞式 Present（一次），避免漏帧
            // 造成的画面撕裂/停滞。只有极端卡顿时才会走到这里。
            hr = swapchain_->Present(vsync_ ? 1 : 0, 0);
            break;
        }
        Sleep(10);
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        GLOG_ERROR("D3D12Swapchain::end_frame: device removed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        return false;
    }
    // DXGI_STATUS_OCCLUDED（窗口最小化）与成功路径都只需推进帧槽；
    // 真正的 present 失败在下一帧 begin_frame 的 fence 等待里暴露。
    signal_and_advance();
    current_frame_ = (current_frame_ + 1) % frame_count_;
    return true;
}

void D3D12Swapchain::signal_and_advance() {
    const UINT64 value = next_fence_value_++;
    if (FAILED(queue_->Signal(fence_, value))) {
        GLOG_ERROR("D3D12Swapchain: queue Signal failed");
        return;
    }
    frame_fence_values_[current_frame_] = value;
}

void D3D12Swapchain::wait_all() {
    if (!queue_ || !fence_ || allocators_.empty()) return;
    const UINT64 value = next_fence_value_++;
    if (FAILED(queue_->Signal(fence_, value))) return;
    if (fence_->GetCompletedValue() < value) {
        if (SUCCEEDED(fence_->SetEventOnCompletion(value, fence_event_))) {
            WaitForSingleObject(fence_event_, INFINITE);
        }
    }
    std::fill(frame_fence_values_.begin(), frame_fence_values_.end(), 0);
}

} // namespace gryce_engine::render::d3d12
