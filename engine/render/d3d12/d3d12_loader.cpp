#include "d3d12_loader.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render::d3d12 {

bool init_loader(Loader& out) {
    if (out.create_device && out.create_factory) return true; // 已加载

    out.d3d12_dll = LoadLibraryW(L"d3d12.dll");
    out.dxgi_dll  = LoadLibraryW(L"dxgi.dll");
    if (!out.d3d12_dll || !out.dxgi_dll) {
        GLOG_ERROR("D3D12: LoadLibrary failed (d3d12.dll={}, dxgi.dll={})",
                   static_cast<void*>(out.d3d12_dll), static_cast<void*>(out.dxgi_dll));
        shutdown_loader(out);
        return false;
    }

    out.create_device =
        reinterpret_cast<PfnD3D12CreateDevice>(GetProcAddress(out.d3d12_dll, "D3D12CreateDevice"));
    out.create_factory =
        reinterpret_cast<PfnCreateDXGIFactory2>(GetProcAddress(out.dxgi_dll, "CreateDXGIFactory2"));
    if (!out.create_device || !out.create_factory) {
        GLOG_ERROR("D3D12: GetProcAddress failed (D3D12CreateDevice={}, CreateDXGIFactory2={})",
                   reinterpret_cast<void*>(out.create_device),
                   reinterpret_cast<void*>(out.create_factory));
        shutdown_loader(out);
        return false;
    }

    // D3D12SerializeRootSignature 同样来自 d3d12.dll。它只在 M4.4+（建 PSO）才被
    // 用到，所以**缺失不算致命**：M2/M3 的交换链与资源层不需要它。这里只告警，
    // 保持向前兼容（例如未来的 d3d12core 分离部署场景）。
    out.serialize_root_signature = reinterpret_cast<PfnD3D12SerializeRootSignature>(
        GetProcAddress(out.d3d12_dll, "D3D12SerializeRootSignature"));
    out.serialize_root_signature = reinterpret_cast<PfnD3D12SerializeRootSignature>(
        GetProcAddress(out.d3d12_dll, "D3D12SerializeRootSignature"));
    if (!out.serialize_root_signature) {
        GLOG_WARN("D3D12: D3D12SerializeRootSignature not found; shader/PSO creation unavailable");
    }

    // 调试层入口（可选）：需要系统安装"图形工具"（Graphics Tools）可选功能，
    // 没装则返回 E_NOINTERFACE 并静默降级，缺失完全不影响正常运行。
    out.get_debug_interface = reinterpret_cast<PfnD3D12GetDebugInterface>(
        GetProcAddress(out.d3d12_dll, "D3D12GetDebugInterface"));

    // ---- M5.2：ImGui DX12 后端需要的两个可选入口点 ----
    // 都只在启用 ImGui 时才会被调用，缺失时 ImGui 初始化会失败但不影响 3D 主链路，
    // 因此同样只告警不返回 false。
    out.create_factory1 = reinterpret_cast<PfnCreateDXGIFactory1>(
        GetProcAddress(out.dxgi_dll, "CreateDXGIFactory1"));
    if (!out.create_factory1) {
        GLOG_WARN("D3D12: CreateDXGIFactory1 not found; ImGui DX12 backend unavailable");
    }

    // d3dcompiler_47.dll 随 Windows 10+ 一起分发（不在 System32 就在
    // WinSxS，LoadLibrary 能直接按名字找到）。用它编译 ImGui 的内嵌着色器。
    out.d3dcompiler_dll = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!out.d3dcompiler_dll) {
        // 少数精简镜像没有 _47；退一步试无版本号的导入名。
        out.d3dcompiler_dll = LoadLibraryW(L"d3dcompiler.dll");
    }
    if (out.d3dcompiler_dll) {
        out.d3d_compile = reinterpret_cast<PfnD3DCompile>(
            GetProcAddress(out.d3dcompiler_dll, "D3DCompile"));
    }
    if (!out.d3d_compile) {
        GLOG_WARN("D3D12: D3DCompile not found (d3dcompiler_47.dll missing?); "
                  "ImGui DX12 backend unavailable");
    }
    return true;
}

void shutdown_loader(Loader& loader) {
    if (loader.d3dcompiler_dll) FreeLibrary(loader.d3dcompiler_dll);
    if (loader.d3d12_dll) FreeLibrary(loader.d3d12_dll);
    if (loader.dxgi_dll)  FreeLibrary(loader.dxgi_dll);
    loader = Loader{};
}

namespace {
const Loader* g_registered_loader = nullptr;
const Loader g_empty_loader{};
} // namespace

void register_loader(const Loader* loader) {
    g_registered_loader = loader;
}

const Loader& loader_ref() {
    return g_registered_loader ? *g_registered_loader : g_empty_loader;
}

} // namespace gryce_engine::render::d3d12
