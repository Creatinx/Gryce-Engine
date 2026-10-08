// =============================================================================
// d3d12_imgui_compile_shim.h — 把 imgui_impl_dx12.cpp 直接调用的两个自由函数
// 重定向到运行时解析的函数指针（见 d3d12_loader.h 的说明）。
//
// 用法：只对 imgui_impl_dx12.cpp 这一个 TU 用 `-include` 强制包含。
// 之所以要重定向：本项目的 D3D12 路线是"链接期零 .lib 依赖"（d3d12.dll /
// dxgi.dll 全部 LoadLibrary + GetProcAddress），而 imgui_impl_dx12.cpp 里
// 直接写了 `::CreateDXGIFactory1(...)` 和 `D3DCompile(...)`，MinGW 会去
// 找 dxgi.lib / d3dcompiler.lib —— 链接期直接 undefined reference。
//
// 这里用宏把两个名字替换成返回 HRESULT 的转发函数；函数体在
// d3d12_imgui_backend.cpp 里（共享进程内的 loader 单例）。
// =============================================================================
#pragma once

#if defined(__cplusplus) && defined(_WIN32)

// 需要 HRESULT / LPCVOID / D3D_SHADER_MACRO / ID3DBlob / ID3DInclude 等类型。
// 本头由 -include 在 imgui_impl_dx12.cpp 的最开头强制包含，此时 <d3d12.h> /
// <d3dcompiler.h> 尚未引入，所以这里自己带上（有 include guard，重复包含无害）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#define CreateDXGIFactory1 gryce_d3d12_shim_CreateDXGIFactory1
#define D3DCompile         gryce_d3d12_shim_D3DCompile

extern "C" {

// 转发到 d3d12.dll 的 CreateDXGIFactory1。
HRESULT WINAPI gryce_d3d12_shim_CreateDXGIFactory1(REFIID riid, void** factory);

// 转发到 d3dcompiler_47.dll 的 D3DCompile。
HRESULT WINAPI gryce_d3d12_shim_D3DCompile(
    LPCVOID src_data, SIZE_T src_data_size, LPCSTR source_name,
    const D3D_SHADER_MACRO* defines, ID3DInclude* include, LPCSTR entrypoint,
    LPCSTR target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** error_msgs);

} // extern "C"

#endif // __cplusplus && _WIN32
