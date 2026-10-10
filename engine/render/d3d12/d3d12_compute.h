// =============================================================================
// d3d12_compute — 计算着色器（GPU 剔除 cull_instances 的计算管线）
//
// 为什么单独一个类，而不是塞进 D3D12Shader：
//   * D3D12Shader 是**图形**管线的载体（input layout / RTV 格式 / depth-blend-cull
//     进 PSO 键 / 两个 CBV 按可见性拆分）。计算管线一条都没有；
//   * 计算的根签名是**另一套**：图形那份带 ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
//     且没有 UAV 范围，两者不能共用。
//
// 编译前端与图形完全同一套链（复用 d3d12_shader 的三个阶段函数）：
//     GLSL → vulkanize(Compute) → shaderc(SPIR-V) → SPIRV-Cross(HLSL) → DXC(DXIL)
// 多出来的一步是**反射**（d3d12_reflect.h）：SPIRV-Cross 给每个 SSBO 块分配 HLSL
// 的 t/u 寄存器号，而引擎按**块名**绑定 SSBO（GL 侧 glGetProgramResourceIndex
// 查的也是块名）—— 必须把两者对上。
//
// ★ D3D12 的 UAV 铁律：只有 DEFAULT 堆 + ALLOW_UNORDERED_ACCESS 的资源才能建
//   UAV 描述符。UPLOAD 堆（CPU 可写的那类）加这个标志会被 CreateCommittedResource
//   直接拒绝 —— 所以引擎侧必须先 set_storage() 声明，见 d3d12_buffer.h。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d12.h>

#include "d3d12_reflect.h"

namespace gryce_engine::render::d3d12 {

class D3D12Backend;

class D3D12ComputeShader {
public:
    D3D12ComputeShader() = default;
    ~D3D12ComputeShader();

    void set_context(D3D12Backend* backend, ID3D12Device* device);

    // 编译 {shader_dir}/{name}.comp 并建根签名 + 计算 PSO。
    bool load(const std::string& name, const std::string& shader_dir);

    bool valid() const { return pso_ != nullptr; }

    // 记录一个 SSBO 绑定（按 **GLSL 块名**，与 GL 后端一致；大小写敏感）。
    // 找不到同名块时记录一次告警 —— 不能静默失败：GL 侧踩过"绑定静默失败 →
    // 剔除看着零开销实则从未生效"的坑。
    void bind_storage(const char* name, ID3D12Resource* res, uint64_t size_bytes);

    // 把记录下来的绑定写成描述符并派发。返回 false 表示未派发（已打日志）。
    bool dispatch(ID3D12GraphicsCommandList* cmd, uint32_t gx, uint32_t gy, uint32_t gz);

    // 本帧清空绑定记录（dispatch_compute 入口调一次）。
    void clear_bindings() { pending_.clear(); }

    const std::string& debug_name() const { return name_; }
    const SsboReflection& reflection() const { return refl_; }

private:
    struct Binding {
        ID3D12Resource* res = nullptr;
        uint64_t size = 0;
    };

    bool build_root_signature(uint32_t srv_count, uint32_t uav_count);
    bool create_pso(const std::vector<uint8_t>& dxil);

    D3D12Backend* backend_ = nullptr;
    ID3D12Device* device_ = nullptr;

    std::string name_;
    std::string shader_dir_;
    std::vector<uint8_t> dxil_;

    SsboReflection refl_;
    // 块名 → 本帧实际资源。
    std::unordered_map<std::string, Binding> pending_;

    ID3D12RootSignature* root_signature_ = nullptr;
    ID3D12PipelineState* pso_ = nullptr;

    // 描述符表（惰性分配一次，之后每帧覆写内容）。
    // ★ 为什么不分帧归还：这几个 SSBO 的 handle 在整个运行期是稳定的，每帧写进
    //   描述符的内容完全相同 —— 即使某个在途帧读到"下一帧"写的描述符，指向的
    //   仍是同一批资源。省掉 GL/Vulkan 那套 per-frame 描述符池的复杂度。
    uint32_t srv_table_base_ = 0;
    uint32_t uav_table_base_ = 0;
    bool tables_allocated_ = false;

    bool warned_missing_block_ = false;
};

} // namespace gryce_engine::render::d3d12
