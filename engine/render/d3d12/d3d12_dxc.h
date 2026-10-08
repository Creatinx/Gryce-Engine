// =============================================================================
// d3d12_dxc — HLSL → DXIL，基于 dxcompiler.dll 运行时加载（M4.2）
//
// 为什么走运行时加载而不是链接 dxcompiler.lib：
//   * 与 d3d12_loader / shaderc / spirv-cross 完全同一模式，**零链接期依赖**；
//   * MinGW 无法链接 MSVC 的 .lib（导出的 .drectve 段不兼容，本项目已在
//     shaderc 上踩过：214MB 的 shaderc_combined.lib 无法被 g++ 链接）；
//   * dxcompiler.dll 自包含（仅依赖 VCRuntime + KERNEL32）。
//
// 接口形态（DXC 的 COM 风格，MinGW 下需注意）：
//   DxcCreateInstance(CLSID_DxcCompiler, IID_IDxcCompiler3, &compiler)
//   → IDxcCompiler3::Compile(blob, args..., &IDxcResult)
//   → IDxcResult::GetOutput(DXC_OUT_OBJECT, IID_IDxcBlob, &blob)  → DXIL 字节
//
// ★ MinGW 陷阱：`__uuidof<T>` 不可用，必须用 `(__mingw_uuidof<T>())`（括号必需，
//   否则报 "no arguments depend on template parameter"）。本文件不引 dxcapi.h
//   （它依赖 MSVC 的 objidl 系列），而是**本地声明最小 COM 接口**，只按 ABI 用。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gryce_engine::render::d3d12 {

// 编译目标 profile，按着色阶段选择。
enum class DxcProfile {
    Vs_5_0,   // 顶点 SM 5.0
    Ps_5_0,   // 像素 SM 5.0
    Cs_5_0,   // 计算 SM 5.0
    Vs_6_0,   // 顶点 SM 6.0（DXIL）
    Ps_6_0,
    Cs_6_0,
};

// 是否可在当前进程加载 dxcompiler（懒加载，仅首次调用时校验）。
bool dxc_available();

// HLSL → DXIL 字节码。
//   hlsl      —— HLSL 源码
//   entry     —— 入口函数名（本引擎统一 "main"）
//   profile   —— 目标 profile 字符串（如 "vs_6_0" / "ps_6_0"）
//   out_dxil  —— 成功时写入 DXIL 字节码
//   error     —— 失败时写入可读错误（含 DXC 诊断）
//
// 返回 true 表示 out_dxil 非空且编译成功。
bool compile_hlsl_to_dxil(const std::string& hlsl, const std::string& entry,
                          DxcProfile profile, std::vector<uint8_t>& out_dxil, std::string& error);

// 便捷：把枚举转成 DXC 接受的 profile 字符串。
const char* dxc_profile_string(DxcProfile p);

// DXC 编译器版本（用于日志/诊断；不可用时返回空串）。
std::string dxc_version();

} // namespace gryce_engine::render::d3d12
