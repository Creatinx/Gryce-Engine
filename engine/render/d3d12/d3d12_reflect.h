// =============================================================================
// d3d12_reflect — 从 SPIRV-Cross 产出的 HLSL 里反射 SSBO 绑定
//
// 为什么需要它：引擎按 **GLSL 块名** 绑定 SSBO（`bind_storage_buffer(shader,
// "SrcModels", buf)`；GL 侧 glGetProgramResourceIndex 查的也是块名）。而 D3D12
// 是按 **HLSL 寄存器号** 绑定的。两者之间必须有一张映射表：
//
//     GLSL 块名  →  HLSL register(tN/uN) 里的 N
//
// ★ 关键（实测确认）：SPIRV-Cross 拿到的 SPIR-V **不含 OpName**，所以
// HLSL 里的变量名全是 `_28` / `_329` 这种 ID 回退名，**按名字配对完全不可行**。
// 能可靠配对的是 **binding 号**：SPIRV-Cross 把 GLSL 的 `binding = N` 原样映射为
// `register(tN)`（只读 → ByteAddressBuffer）或 `register(uN)`（可写 →
// RWByteAddressBuffer）。实测映射：
//   SrcModels b0→t0, SrcSpheres b1→t1, DstModels b2→u2, DrawArgsBuffer b3→u3,
//   CullParams b4→t4, HiZLevels b5→t5；instanced.vert 的 DstModels b2→t2。
//
// 另一件事：区分 **UAV（可写）** 与 **SRV（只读）**。HLSL 里 `RW...` 前缀 →
// u 寄存器 → 要 CreateUnorderedAccessView；否则 → t 寄存器 → CreateShaderResourceView。
// 这决定了描述符种类与 root signature 的范围类型，猜错会 E_INVALIDARG。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gryce_engine::render::d3d12 {

struct SsboBinding {
    std::string block;     // GLSL 块名（引擎绑定用的键）
    std::string member;    // 块内第一个成员名（仅随反射结果输出，供人工核对）
    std::string hlsl_var;  // HLSL 里出现的变量名（仅随反射结果输出，供人工核对）
    std::string hlsl_type; // StructuredBuffer<T> 里的 T（仅随反射结果输出，供人工核对）
    uint32_t reg = 0;      // register(tN/uN) 里的 N
    bool is_uav = false;   // true = RW...Buffer → u 寄存器
    // ★ raw buffer（HLSL `ByteAddressBuffer`，无 `<T>`）。
    //   决定 SRV/UAV 的**视图类型**：raw 必须用
    //     Format = DXGI_FORMAT_R32_TYPELESS, StructureByteStride = 0,
    //     Flags = D3D12_BUFFER_*_FLAG_RAW, NumElements = size/4
    //   而 structured 必须用 `StructureByteStride = stride` + Flags = NONE。
    //   两者**不可混用**：把 raw 缓冲建成 structured 视图（反之亦然）时，着色器
    //   声明的资源布局与描述符不符，D3D12 会直接 **移除设备**（0x887A0005），
    //   且是否触发取决于当帧数据 —— 表现为"时好时坏的 TDR"。
    bool is_raw = false;
    uint32_t stride = 0;   // 元素字节数（structured 用；raw 恒为 4）
};

struct SsboReflection {
    std::vector<SsboBinding> bindings;
    uint32_t srv_count = 0;   // t0..t{srv_count-1} 需要覆盖
    uint32_t uav_count = 0;   // u0..u{uav_count-1} 需要覆盖
};

// hlsl           —— SPIRV-Cross 生成的 HLSL 源码
// vulkan_glsl    —— vulkanize 之后的 GLSL（用于取块名与成员名）
SsboReflection reflect_ssbo_bindings(const std::string& hlsl, const std::string& vulkan_glsl);

// 在 GLSL 里用 **(不区分大小写)** 的名字查块名。返回 nullptr 表示没找到。
// 大小写不敏感是为了容忍 SPIRV-Cross 的大小写改写（实测它会保留原样，但
// 不把这一条当前提 —— 查不到就退回大小写敏感匹配）。
const SsboBinding* find_ssbo(const std::vector<SsboBinding>& list, const std::string& name);

} // namespace gryce_engine::render::d3d12
