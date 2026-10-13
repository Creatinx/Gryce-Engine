#include "d3d12_imgui_backend.h"

#include <d3d12.h>

#include "render/render.h"
#include "render/texture.h"
#include "d3d12_backend.h"
#include "d3d12_texture.h"
#include "d3d12_swapchain.h"
#include "d3d12_descriptors.h"
#include "d3d12_loader.h"
#include "utils/glog/glog_lib.h"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>

// ---------------------------------------------------------------------------
// 两个自由函数的转发实现（声明见 d3d12_imgui_compile_shim.h）。
// imgui_impl_dx12.cpp 单独一个 TU，用 -include 强制包含 shim 头把
// ::CreateDXGIFactory1 / ::D3DCompile 宏替换成本函数；这里提供函数体，
// 走 loader 里的运行时函数指针，保持"零 .lib 依赖"。
// ---------------------------------------------------------------------------
extern "C" HRESULT WINAPI gryce_d3d12_shim_CreateDXGIFactory1(REFIID riid, void** factory) {
    const gryce_engine::render::d3d12::Loader& l =
        gryce_engine::render::d3d12::loader_ref();
    if (!l.create_factory1) return E_NOINTERFACE;
    return l.create_factory1(riid, factory);
}

extern "C" HRESULT WINAPI gryce_d3d12_shim_D3DCompile(
    LPCVOID src_data, SIZE_T src_data_size, LPCSTR source_name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* include, LPCSTR entrypoint,
    LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** error_msgs) {
    const gryce_engine::render::d3d12::Loader& l =
        gryce_engine::render::d3d12::loader_ref();
    if (!l.d3d_compile) return E_NOINTERFACE;
    return l.d3d_compile(src_data, src_data_size, source_name, defines, include,
                         entrypoint, target, flags1, flags2, code, error_msgs);
}

namespace gryce_engine::render {

namespace {

// ---------------------------------------------------------------------------
// ImGui 专用 SRV 堆
//
// D3D12 规定同一时刻只能有一个 shader-visible CBV/SRV/UAV 堆被
// SetDescriptorHeaps 绑定。ImGui 自己需要一小块堆来放字体纹理与用户纹理的
// SRV，而引擎主堆（DescriptorHeapSet::srv）会被引擎的 PBR / 后处理占用，
// 两者的分配策略完全不同（引擎整帧不回收，ImGui 每帧线性重来）。
//
// 因此本后端**自己建一块独立的小堆**给 ImGui 用，绘制 ImGui 前把这块堆绑上去、
// 绘制后再把引擎主堆绑回去（D3D12Backend::rebind_descriptor_heaps）。
// 这两个动作必须成对，否则后续 draw 的 SRV 会指向 ImGui 堆 → 花屏/黑屏。
// ---------------------------------------------------------------------------
constexpr uint32_t kImGuiSrvHeapCapacity = 256;

ID3D12DescriptorHeap* g_imgui_srv_heap = nullptr;
ID3D12Device* g_imgui_device = nullptr;

// 线性分配（ImGui 一帧内分配的槽位在下一帧 NewFrame 时整体回收，
// 这里用简单的环形游标即可，不需要自由链表）。
uint32_t g_imgui_srv_next = 0;
uint32_t g_imgui_srv_increment = 0;

void imgui_heap_alloc(ImGui_ImplDX12_InitInfo* /*info*/,
                     D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                     D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    if (!g_imgui_srv_heap) {
        out_cpu->ptr = 0;
        out_gpu->ptr = 0;
        return;
    }
    const uint32_t index = g_imgui_srv_next++ % kImGuiSrvHeapCapacity;
    out_cpu->ptr = g_imgui_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr +
                   static_cast<SIZE_T>(index) * g_imgui_srv_increment;
    out_gpu->ptr = g_imgui_srv_heap->GetGPUDescriptorHandleForHeapStart().ptr +
                   static_cast<UINT64>(index) * g_imgui_srv_increment;
}

void imgui_heap_free(ImGui_ImplDX12_InitInfo* /*info*/,
                    D3D12_CPU_DESCRIPTOR_HANDLE /*cpu*/,
                    D3D12_GPU_DESCRIPTOR_HANDLE /*gpu*/) {
    // 环形分配，无需真正归还；容量 256 足够（字体纹理 + 少量 Viewport 纹理）。
}

bool create_imgui_srv_heap(ID3D12Device* device) {
    if (g_imgui_srv_heap) return true;
    if (!device) return false;

    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = kImGuiSrvHeapCapacity;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&desc, __mingw_uuidof<ID3D12DescriptorHeap>(),
                                            reinterpret_cast<void**>(&g_imgui_srv_heap)))) {
        GLOG_ERROR("D3D12ImGuiBackend: failed to create ImGui SRV heap");
        g_imgui_srv_heap = nullptr;
        return false;
    }
    g_imgui_srv_increment = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_imgui_device = device;
    g_imgui_srv_next = 0;
    return true;
}

void destroy_imgui_srv_heap() {
    if (g_imgui_srv_heap) {
        g_imgui_srv_heap->Release();
        g_imgui_srv_heap = nullptr;
    }
    g_imgui_device = nullptr;
    g_imgui_srv_next = 0;
}

} // namespace

D3D12ImGuiBackend::D3D12ImGuiBackend(D3D12Backend* backend) : backend_(backend) {}

D3D12ImGuiBackend::~D3D12ImGuiBackend() {
    shutdown();
}

bool D3D12ImGuiBackend::init() {
    if (!backend_ || !backend_->device()) return false;
    if (!create_imgui_srv_heap(backend_->device())) return false;

    ImGui_ImplDX12_InitInfo info{};
    info.Device = backend_->device();
    info.CommandQueue = backend_->command_queue();
    info.NumFramesInFlight = 3;                 // 与交换链缓冲数一致（见 D3D12Swapchain）
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // 交换链 back buffer 格式
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;        // 交换链无深度附件
    info.UserData = this;
    info.SrvDescriptorHeap = g_imgui_srv_heap;
    info.SrvDescriptorAllocFn = &imgui_heap_alloc;
    info.SrvDescriptorFreeFn = &imgui_heap_free;

    if (!ImGui_ImplDX12_Init(&info)) {
        GLOG_ERROR("D3D12ImGuiBackend: ImGui_ImplDX12_Init failed");
        destroy_imgui_srv_heap();
        return false;
    }

    initialized_ = true;
    GLOG_INFO("D3D12ImGuiBackend initialized");
    return true;
}

void D3D12ImGuiBackend::shutdown() {
    if (!initialized_) {
        destroy_imgui_srv_heap();
        return;
    }
    if (backend_ && backend_->device()) {
        backend_->wait_gpu_idle();
    }
    cache_.clear();
    ImGui_ImplDX12_Shutdown();
    destroy_imgui_srv_heap();
    initialized_ = false;
}

void D3D12ImGuiBackend::new_frame() {
    ImGui_ImplDX12_NewFrame();
}

void D3D12ImGuiBackend::render_draw_data(ImDrawData* draw_data) {
    if (!draw_data || !backend_ || !initialized_) return;
    ID3D12GraphicsCommandList* cmd = backend_->swapchain().command_list();
    if (!cmd) return;

    // ★ 先把引擎本帧的画面（GL 约定，上下镜像）翻转 blit 进真实 back buffer，
    //   并把 back buffer 设为当前 RT —— ImGui 的内置着色器是 D3D 原生约定
    //   （左上原点、y 向下），必须画在"已经纠正过方向"的成品之上，否则它自己
    //   会跟着被翻一次（UI 上下颠倒）。这与 Vulkan 后端给 ImGui 投影单独做
    //   Y 翻转是同一个道理，只是这里用 blit 把引擎内容先搬正更直接。
    backend_->flush_present_blit_for_overlay();

    // ★ 把 ImGui 自己的 SRV 堆绑上去（替换引擎主堆），绘制完再换回来。
    ID3D12DescriptorHeap* heaps[] = {g_imgui_srv_heap};
    cmd->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(draw_data, cmd);
    backend_->rebind_descriptor_heaps();
}

void D3D12ImGuiBackend::rebuild_fonts() {
    if (!initialized_ || !backend_) return;
    backend_->wait_gpu_idle();
    ImGui_ImplDX12_InvalidateDeviceObjects();
    ImGui_ImplDX12_CreateDeviceObjects();
}

uint64_t D3D12ImGuiBackend::imgui_texture_id(ITexture* texture) const {
    if (!texture || !texture->is_valid() || !initialized_ || !g_imgui_srv_heap) return 0;
    auto* dx_tex = dynamic_cast<d3d12::D3D12Texture*>(texture);
    if (!dx_tex) return 0;

    auto it = cache_.find(texture);
    if (it != cache_.end()) {
        if (texture->is_valid()) {
            const uint32_t slot = it->second.slot;
            const UINT64 gpu = g_imgui_srv_heap->GetGPUDescriptorHandleForHeapStart().ptr +
                               static_cast<UINT64>(slot) * g_imgui_srv_increment;
            return static_cast<uint64_t>(gpu);
        }
        cache_.erase(it);
    }

    if (cache_.size() >= k_max_cache_size) {
        GLOG_WARN("D3D12ImGuiBackend: texture cache full ({}), evicting all", cache_.size());
        cache_.clear();
    }

    // 分配一个 ImGui 堆槽位并为纹理建 SRV。
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    imgui_heap_alloc(nullptr, &cpu, &gpu);
    if (cpu.ptr == 0) return 0;

    if (!dx_tex->create_srv(cpu)) {
        GLOG_ERROR("D3D12ImGuiBackend: failed to create SRV for texture");
        return 0;
    }

    const uint32_t index = g_imgui_srv_next - 1;
    cache_[texture] = CacheEntry{index % kImGuiSrvHeapCapacity};
    return static_cast<uint64_t>(gpu.ptr);
}

void D3D12ImGuiBackend::invalidate_texture(ITexture* texture) {
    if (!texture) return;
    cache_.erase(texture);
    // 槽位由环形分配器管理，不单独回收；缓存项单纯失效即可，
    // 下次 imgui_texture_id 会重新分配一个新槽位。
}

} // namespace gryce_engine::render
