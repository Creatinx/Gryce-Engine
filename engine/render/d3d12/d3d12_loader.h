// =============================================================================
// d3d12_loader — D3D12 / DXGI 运行时动态加载（M0 已验证的路线）
//
// 与 Vulkan 侧"shaderc_shared.dll 动态加载"同一套约定：
//   * 链接期零 .lib 依赖 —— MinGW 链 MSVC 导入库有符号/权限坑
//     （engine 对 vulkan-1.lib 已踩过），COM 接口方法本就走 vtable，
//     只有自由函数（D3D12CreateDevice / CreateDXGIFactory2）需要GetProcAddress；
//   * d3d12.dll / dxgi.dll 是 Win10+ 系统自带组件，分发零附加文件；
//   * 头文件用 mingw-w64 自带的 d3d12.h / dxgi1_4.h（CLion 工具链 target
//     include 目录里有，已由 M0 探针 test/tools/d3d12_m0_clear.cpp 验证）。
// =============================================================================
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

namespace gryce_engine::render::d3d12 {

// mingw-w64 头未提供这两个自由函数的 PFN typedef，本地声明。
using PfnD3D12CreateDevice  = HRESULT(WINAPI*)(IUnknown* adapter, D3D_FEATURE_LEVEL min_feature_level,
                                               REFIID riid, void** device);
using PfnCreateDXGIFactory2 = HRESULT(WINAPI*)(UINT flags, REFIID riid, void** factory);

// D3D12SerializeRootSignature 也是 d3d12.dll 的自由函数（不是 COM 方法），
// M4.4 建 root signature 需要它。**必须走 GetProcAddress**：直接调用会在链接期
// 找 d3d12.lib，违反"零 .lib 依赖"的既定路线（MinGW 链 MSVC 导入库有坑）。
using PfnD3D12SerializeRootSignature = HRESULT(WINAPI*)(
    const D3D12_ROOT_SIGNATURE_DESC* root_signature_desc, D3D_ROOT_SIGNATURE_VERSION version,
    ID3DBlob** blob, ID3DBlob** error_blob);

// D3D12GetDebugInterface 同样是 d3d12.dll 的自由函数（用于打开调试层）。
// 拿到的 ID3D12Debug 用来在创建设备前打开调试层；调试层会逐条校验 API 调用并
// 把违规记进 ID3D12InfoQueue —— 这是定位"设备被移除"最直接的手段：
// 没有它只能看到一堆 0x887A0005（那全是**后果**，不是原因）。
using PfnD3D12GetDebugInterface = HRESULT(WINAPI*)(REFIID riid, void** debug_interface);

// ---- M5.2 ImGui DX12 后端需要的两个额外入口点 ----
// imgui_impl_dx12.cpp 的 CreateDeviceObjects() 直接调用它们：
//   * ::CreateDXGIFactory1 —— 只为查询 tearing 支持（dxgi.dll 导出）；
//   * ::D3DCompile        —— 编译内嵌的 ImGui 顶点/像素着色器（d3dcompiler_47.dll）。
// 两者都在本项目的"零 .lib 依赖"路线之外（直接调用会要求链接 dxgi.lib /
// d3dcompiler.lib），所以在这里声明为函数指针并用 GetProcAddress 解析，
// 再由 d3d12_imgui_backend.cpp 通过 -D 宏把 ::CreateDXGIFactory1 / ::D3DCompile
// 重定向到这两个指针（见该文件的 shim 段）。
using PfnCreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID riid, void** factory);
using PfnD3DCompile = HRESULT(WINAPI*)(LPCVOID src_data, SIZE_T src_data_size,
                                       LPCSTR source_name, const D3D_SHADER_MACRO* defines,
                                       ID3DInclude* include, LPCSTR entrypoint,
                                       LPCSTR target, UINT flags1, UINT flags2,
                                       ID3DBlob** code, ID3DBlob** error_msgs);

// 运行时加载上下文：DLL 句柄 + 自由函数指针 + 已创建的工厂。
// 工厂放在 loader 里（而不是 backend 私有成员），因为 swapchain 也需要它 ——
// CreateSwapChainForHwnd 是 **IDXGIFactory 的方法**，不是设备的。
struct Loader {
    HMODULE d3d12_dll = nullptr;
    HMODULE dxgi_dll  = nullptr;
    HMODULE d3dcompiler_dll = nullptr;  // 仅 ImGui DX12 后端需要（可选）
    PfnD3D12CreateDevice  create_device = nullptr;
    PfnCreateDXGIFactory2 create_factory = nullptr;
    PfnD3D12SerializeRootSignature serialize_root_signature = nullptr;
    PfnD3D12GetDebugInterface get_debug_interface = nullptr;
    PfnCreateDXGIFactory1 create_factory1 = nullptr;   // ImGui DX12 后端
    PfnD3DCompile d3d_compile = nullptr;               // ImGui DX12 后端

    // 由 backend 在 init 里建好；**loader 不负责释放它** ——
    // 由 backend 按"创建顺序倒序"统一释放，避免双重释放与顺序错乱。
    IDXGIFactory4* factory = nullptr;
  };

// LoadLibrary d3d12.dll / dxgi.dll 并解析两个入口点。
// 任一步失败返回 false 并填好 log（调用方直接放弃 D3D12 后端即可）。
bool init_loader(Loader& out);

// FreeLibrary（幂等：未加载时是空操作）。不释放 factory（见上）。
void shutdown_loader(Loader& loader);

// ---------------------------------------------------------------------------
// 进程内 loader 单例的只读访问。
//
// 存在的唯一理由：imgui_impl_dx12.cpp 里直接调用的 CreateDXGIFactory1 /
// D3DCompile 需要转发到运行时解析的函数指针，而那个 TU 拿不到 backend 实例
// （见 d3d12_imgui_compile_shim.h）。backend 在 init_loader 成功后登记一次，
// 转发函数从这里取。未登记时返回一个空的 Loader（指针全 null，安全降级）。
// ---------------------------------------------------------------------------
void register_loader(const Loader* loader);
const Loader& loader_ref();

} // namespace gryce_engine::render::d3d12
