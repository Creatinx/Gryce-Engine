// =============================================================================
// Gryce Engine —— D3D12 后端 M0 探针（test/tools/d3d12_m0_clear.cpp）
//
// 目的：用一条最小但完整的链路证明「CLion MinGW(GCC) 编译 + 运行时动态加载
// d3d12.dll/dxgi.dll」可以编出并运行真实的 D3D12 程序。这是 D3D12 后端
// （docs/zh/plans/d3d12-backend-plan.md 的 M0）的最大风险点，此探针通过后剩余工作是工程量。
//
// 实测结论（2026-10-06，修正了此前"MinGW 无 DX 头"的误判）：
//   * CLion MinGW 的 **target 头目录**（x86_64-w64-mingw32/include，不是顶层
//     include/）自带完整 DirectX 头（d3d12.h、dxgi1_2..1_6.h、d3dcommon.h 等，
//     mingw-w64 项目出品，可随仓库分发使用），且已用 __CRT_UUID_DECL 声明好
//     __mingw_uuidof<T> —— **编译期甚至不需要 Windows SDK**；
//   * 唯一缺口：D3D12CreateDevice 没有现成 PFN typedef（本文件自声明）；
//   * 链接期照旧零 .lib 依赖：D3D12CreateDevice / CreateDXGIFactory2 走
//     LoadLibrary + GetProcAddress（照 engine/render/vulkan/vk_loader.cpp 先例）。
//
// 验证内容：设备 / 命令队列 / 交换链(FLIP_DISCARD) / fence / 清屏 present 全链路。
// 通过标准：打印 "[m0] OK ..." 且 exit 0。
//
// 编译（Git Bash）：
//   "D:/JetBrains/CLion 2026.2.3.1/bin/mingw/bin/c++.exe" -std=c++20 -O2
//     -DNOMINMAX -D_WIN32_WINNT=0x0A00
//     test/tools/d3d12_m0_clear.cpp -o /tmp/d3d12_m0_clear.exe
// =============================================================================

#include <windows.h>
#include <cstdio>
#include <dxgi1_4.h>
#include <d3d12.h>

// ---------------------------------------------------------------------------
// mingw-w64 的 d3d12.h 不提供 D3D12CreateDevice 的 PFN typedef（它是唯一缺口），
// CreateDXGIFactory2 的 typedef 在 MSVC 头里而 mingw 侧未导出 —— 自声明，
// 顺带保持与引擎一致的"运行时 LoadLibrary + GetProcAddress"取函数方式。
// ---------------------------------------------------------------------------
using PfnD3D12CreateDevice  = HRESULT(WINAPI*)(IUnknown* adapter, D3D_FEATURE_LEVEL fl,
                                               REFIID riid, void** device);
using PfnCreateDXGIFactory2 = HRESULT(WINAPI*)(UINT flags, REFIID riid, void** factory);

// MinGW 侧取 IID 的统一入口：__CRT_UUID_DECL 已为各接口特化 __mingw_uuidof<T>，
// GCC 的 __uuidof 关键字不可用（它依赖 -fms-extensions 的 MS 扩展路径）。
#define M0_IID_PPV(T, pp) __mingw_uuidof<T>(), reinterpret_cast<void**>(pp)

#define M0_CHECK(expr)                                                            \
    do {                                                                          \
        HRESULT _hr = (expr);                                                     \
        if (FAILED(_hr)) {                                                        \
            std::printf("[m0] FAIL: %s -> hr=0x%08lX (line %d)\n", #expr,         \
                        static_cast<unsigned long>(_hr), __LINE__);               \
            return 1;                                                             \
        }                                                                         \
    } while (0)

namespace {

bool pump_messages() {
    MSG m{};
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) return false;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return true;
}

} // namespace

int main() {
    // ---- 1. Win32 窗口（探针不走 GLFW；引擎里由 GLFW 的 GLFW_NO_API 提供 HWND）----
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GryceD3D12M0";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Gryce D3D12 M0 Probe",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        std::printf("[m0] FAIL: CreateWindowExW err=%lu\n", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);

    // ---- 2. 运行时动态加载（链接期零 .lib 依赖）----
    HMODULE mod_d3d12 = LoadLibraryW(L"d3d12.dll");
    HMODULE mod_dxgi  = LoadLibraryW(L"dxgi.dll");
    if (!mod_d3d12 || !mod_dxgi) {
        std::printf("[m0] FAIL: LoadLibrary d3d12.dll=%p dxgi.dll=%p\n",
                    static_cast<void*>(mod_d3d12), static_cast<void*>(mod_dxgi));
        return 1;
    }
    auto p_create_device =
        reinterpret_cast<PfnD3D12CreateDevice>(GetProcAddress(mod_d3d12, "D3D12CreateDevice"));
    auto p_create_factory =
        reinterpret_cast<PfnCreateDXGIFactory2>(GetProcAddress(mod_dxgi, "CreateDXGIFactory2"));
    if (!p_create_device || !p_create_factory) {
        std::printf("[m0] FAIL: GetProcAddress (create_device=%p create_factory=%p)\n",
                    reinterpret_cast<void*>(p_create_device),
                    reinterpret_cast<void*>(p_create_factory));
        return 1;
    }

    // ---- 3. 工厂 / 适配器 / 设备 ----
    IDXGIFactory4* factory = nullptr;
    M0_CHECK(p_create_factory(0, M0_IID_PPV(IDXGIFactory4, &factory)));

    IDXGIAdapter1* adapter = nullptr;
    M0_CHECK(factory->EnumAdapters1(0, &adapter));
    DXGI_ADAPTER_DESC1 adesc{};
    M0_CHECK(adapter->GetDesc1(&adesc));
    std::printf("[m0] adapter: %ls (vendor=0x%04X, dedicated VRAM=%lu MB)\n", adesc.Description,
                adesc.VendorId, static_cast<unsigned long>(adesc.DedicatedVideoMemory >> 20));

    ID3D12Device* device = nullptr;
    M0_CHECK(p_create_device(adapter, D3D_FEATURE_LEVEL_12_0,
                             M0_IID_PPV(ID3D12Device, &device)));
    std::printf("[m0] device created (feature level 12.0)\n");

    // ---- 4. 命令队列 / 分配器 / 命令列表 / fence ----
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    M0_CHECK(device->CreateCommandQueue(&qdesc, M0_IID_PPV(ID3D12CommandQueue, &queue)));

    ID3D12CommandAllocator* allocator = nullptr;
    M0_CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            M0_IID_PPV(ID3D12CommandAllocator, &allocator)));

    ID3D12GraphicsCommandList* list = nullptr;
    M0_CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                       M0_IID_PPV(ID3D12GraphicsCommandList, &list)));
    // CreateCommandList 返回的列表处于 recording 态；不先 Close 的话，
    // allocator->Reset() 会返回 E_FAIL（存在未关闭的命令列表）。
    M0_CHECK(list->Close());

    ID3D12Fence* fence = nullptr;
    M0_CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, M0_IID_PPV(ID3D12Fence, &fence)));
    HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fence_value = 0;
    auto flush_gpu = [&]() -> bool {
        ++fence_value;
        if (FAILED(queue->Signal(fence, fence_value))) return false;
        if (fence->GetCompletedValue() < fence_value) {
            if (FAILED(fence->SetEventOnCompletion(fence_value, fence_event))) return false;
            if (WaitForSingleObject(fence_event, 10000) != WAIT_OBJECT_0) return false;
        }
        return true;
    };

    // ---- 5. 交换链（FLIP_DISCARD，2 缓冲）----
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width       = 800;
    scd.Height      = 600;
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferCount = 2;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;

    IDXGISwapChain1* swap1 = nullptr;
    // 注意：mingw-w64 头的 CreateSwapChainForHwnd 是无 riid 的旧式签名
    //（MSVC SDK 头是 riid 版）—— 写后端时此处按 MinGW 头的签名走即可。
    M0_CHECK(factory->CreateSwapChainForHwnd(queue, hwnd, &scd, nullptr, nullptr, &swap1));
    IDXGISwapChain3* swap = nullptr;
    M0_CHECK(swap1->QueryInterface(M0_IID_PPV(IDXGISwapChain3, &swap)));
    swap1->Release();
    M0_CHECK(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));

    // ---- 6. RTV 堆 + 每个后备缓冲一个 RTV ----
    constexpr UINT kBuffers = 2;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC rhd{};
    rhd.NumDescriptors = kBuffers;
    rhd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    M0_CHECK(device->CreateDescriptorHeap(&rhd, M0_IID_PPV(ID3D12DescriptorHeap, &rtv_heap)));
    const UINT rtv_inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    ID3D12Resource* targets[kBuffers] = {};
    for (UINT i = 0; i < kBuffers; ++i) {
        M0_CHECK(swap->GetBuffer(i, M0_IID_PPV(ID3D12Resource, &targets[i])));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * rtv_inc;
        device->CreateRenderTargetView(targets[i], nullptr, h);
    }

    // ---- 7. 渲染循环：清屏 + present，至多 90 帧或窗口被关 ----
    const float clear_color[4] = {0.10f, 0.32f, 0.62f, 1.0f}; // 引擎品牌蓝
    UINT frames = 0;
    while (pump_messages() && frames < 90) {
        M0_CHECK(allocator->Reset());
        M0_CHECK(list->Reset(allocator, nullptr));

        const UINT idx = swap->GetCurrentBackBufferIndex();
        // 手写屏障（不依赖 d3dx12.h 的 CD3DX12 辅助 —— 正好验证屏障结构在 GCC 下的可构造性）
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = targets[idx];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += SIZE_T(idx) * rtv_inc;
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->ClearRenderTargetView(rtv, clear_color, 0, nullptr);

        D3D12_RESOURCE_BARRIER to_present = barrier;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        to_present.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &to_present);

        M0_CHECK(list->Close());
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        M0_CHECK(swap->Present(1, 0));
        ++frames;
    }
    if (!flush_gpu()) {
        std::printf("[m0] FAIL: fence flush timed out or errored\n");
        return 1;
    }

    for (auto* t : targets) if (t) t->Release();
    rtv_heap->Release();
    swap->Release();
    fence->Release();
    CloseHandle(fence_event);
    list->Release();
    allocator->Release();
    queue->Release();
    device->Release();
    adapter->Release();
    factory->Release();
    DestroyWindow(hwnd);

    std::printf("[m0] OK: %u frames cleared & presented via D3D12 (MinGW headers + "
                "runtime-loaded d3d12.dll)\n", frames);
    return 0;
}
