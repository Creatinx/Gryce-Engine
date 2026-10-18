// =============================================================================
// d3d12_spirv_cross — SPIR-V → HLSL 翻译（M4.1）
//
// D3D12 不认 SPIR-V，只认 DXIL（通过 DXC 编译 HLSL 得到）。所以整个着色器链是：
//   GLSL --shaderc--> SPIR-V --SPIRV-Cross--> HLSL --DXC--> DXIL
//
// 这里负责中间那一跳。**不 vendor SPIRV-Cross 源码**，而是复用 Vulkan SDK
// 自带的 spirv-cross-c-shared.dll 的 C API，走 LoadLibrary + GetProcAddress：
//   * 与 shaderc（glsl_compiler）、DXC（d3d12_dxc）完全同一模式，零链接期依赖；
//   * GL-only 运行时不需要任何 SPIRV-Cross DLL（加载失败即降级）；
//   * 免去把 200+ 个 SPIRV-Cross 源文件塞进 third_party 的维护负担。
//
// 注意：为了不引入第三方头，C API 的不透明类型与枚举值在本文件内本地声明，
// 只按 ABI 使用指针（spvc_* 全部以 void* 形式交互）。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gryce_engine::render::d3d12 {

// SPIR-V 阶段（与 shaderc 的输出对应）。
enum class SpvcStage {
    Vertex,
    Fragment,
    Compute,
};

// 是否可在当前进程加载 spirv-cross（懒加载，仅首次调用时校验）。
bool spirv_cross_available();

// 顶点属性 location → HLSL 语义名（如 0 → "POSITION"）。
// ★ 必须提供：SPIRV-Cross 默认按 SPIR-V 变量名生成语义（aPos→TEXCOORD0），
// 而 D3D12 输入布局按语义名匹配，不重映射会让顶点属性全部落空。
struct VertexSemantic {
    uint32_t location = 0;
    std::string semantic;   // "POSITION" / "NORMAL" / "TANGENT0" / "TEXCOORD0" / "COLOR0" ...
};

// 把 SPIR-V 翻成 HLSL。
//   spirv             —— SPIR-V word 序列（compile_glsl_to_spirv 的产物）
//   stage             —— 着色阶段
//   entry             —— SPIR-V 里的入口名（引擎统一是 "main"）
//   vertex_semantics  —— 仅 Vertex 阶段使用，顶点属性语义重映射
//   out_hlsl / error  —— 输出 / 错误
bool spirv_to_hlsl(const std::vector<uint32_t>& spirv, SpvcStage stage, const std::string& entry,
                   const std::vector<VertexSemantic>& vertex_semantics,
                   std::string& out_hlsl, std::string& error);

} // namespace gryce_engine::render::d3d12
