// =============================================================================
// d3d12_retire — GPU 资源延迟释放通道（被 mesh / buffer 共用）
//
// D3D12 **严禁**销毁仍被"已提交但未完成"的命令列表引用的资源：调试层报
//   ID3D12CommandList::Close: An ID3D12Resource object ('...') referenced in the
//   command list being closed was deleted prior to closing the command list.
// 驱动侧表现为 DXGI_ERROR_DEVICE_HUNG(0x887A0006) / RemoveDevice。
//
// 而引擎有 frames_in_flight(=3) 个帧槽，渲染线程投递后 GPU 会落后 2~3 帧 ——
// 所以"替换/重建 GPU 资源"必须在 CPU 侧延迟到**引用它的命令列表执行完**之后。
//
// 这里只定义接口，真正的队列与围栏判断在 D3D12Backend::retire_gpu_resource()
// 与 drain_retired()（每次 begin_frame 排空一次）。
// =============================================================================
#pragma once

#include <d3d12.h>

namespace gryce_engine::render::d3d12 {

class ResourceRetireSink {
public:
    virtual ~ResourceRetireSink() = default;
    // 把资源挂到"最近一次已提交"的围栏值上，GPU 跑过之后才真正 Release。
    // 调用后指针所有权转移给 sink（调用方必须立刻把自己的成员置空）。
    virtual void retire_gpu_resource(ID3D12Resource* res) = 0;
};

} // namespace gryce_engine::render::d3d12
