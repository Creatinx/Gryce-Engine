// =============================================================================
// d3d12_framebuffer — IFramebuffer 的 D3D12 实现（M3.5）
//
// 与 GL/Vulkan 最大的认知差异：
//   * GL 的 FBO 是"附件集合"对象；
//   * Vulkan 的 framebuffer 也绑定了具体 image view；
//   * **D3D12 没有 framebuffer 对象**。OMSetRenderTargets 接受的是 RTV/DSV
//     **描述符句柄数组**，仅此而已。
//   所以 IFramebuffer 在 D3D12 上的实现 = "一组 RTV/DSV 描述符 + 尺寸信息"。
//
// 因此 attach_color_texture / attach_depth_texture 的实际动作是"取纹理的
// ID3D12Resource，为它创建 RTV/DSV 描述符"。这些描述符放在本类自有的
// NON_SHADER_VISIBLE 堆里（RTV/DSV 不需要 SHADER_VISIBLE）。
//
// 状态管理（重要）：纹理被当作 RT 绑定期间必须是 RENDER_TARGET 状态，被采样
// 时必须是 PIXEL_SHADER_RESOURCE。本类在 bind() 时把附件切到 RT 态，unbind()
// 时切回 shader-read 态 —— 这是"离屏渲染后再采样"这条最容易出错的路径。
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

#include <d3d12.h>

#include "render/framebuffer.h"

namespace gryce_engine::render::d3d12 {

class D3D12Texture;

class D3D12Framebuffer final : public IFramebuffer {
public:
    D3D12Framebuffer() = default;
    ~D3D12Framebuffer() override;

    void set_context(ID3D12Device* device, ID3D12CommandQueue* queue);

    // ---- IFramebuffer ----
    bool create(int width, int height) override;
    void destroy() override;
    void bind() const override;
    void unbind() const override;
    void resize(int width, int height) override;
    void attach_color_texture(ITexture* texture) override;
    void attach_depth_texture(ITexture* texture) override;
    // 记录 resolve 目标（单样本纹理）。D3D12 没有 render pass 式的自动解析，
    // 真正动作由 D3D12Backend::resolve_framebuffer 用 ResolveSubresource 完成；
    // 这里只保存引用供其查询。
    void attach_resolve_texture(ITexture* texture) override;
    // 颜色附件 0 的采样数（PSO 的 SampleDesc.Count 必须与之一致）。
    // 实现在 .cpp（需要 D3D12Texture 完整定义）。
    int color_sample_count() const override;
    bool is_complete() const override;
    int width() const override { return width_; }
    int height() const override { return height_; }

    // ---- D3D12 侧扩展 ----
    // 供 backend 在绘制前录制 OMSetRenderTargets 与状态转换。
    uint32_t color_attachment_count() const {
        return static_cast<uint32_t>(color_rtvs_.size());
    }
    D3D12_CPU_DESCRIPTOR_HANDLE color_rtv(uint32_t i) const;
    bool has_depth() const { return depth_ != nullptr; }
    D3D12_CPU_DESCRIPTOR_HANDLE depth_dsv() const { return depth_dsv_; }
    D3D12Texture* depth_texture() const { return depth_; }
    D3D12Texture* color_texture(uint32_t i) const;
    // resolve 目标纹理（多样本颜色的解析去处；未设置时为 nullptr）。
    D3D12Texture* resolve_texture() const { return resolve_; }

    // 颜色/深度的 DXGI 格式（PSO 必须与 RT 格式匹配，否则 CreateGraphicsPipelineState
    // 返回 E_INVALIDARG）。无附件时返回 UNKNOWN。
    DXGI_FORMAT color_format() const;
    DXGI_FORMAT depth_format() const;

    // 录制进命令列表：所有附件 COMMON/PRESENT → RENDER_TARGET(DEPTH_WRITE)。
    void record_transition_to_rt(ID3D12GraphicsCommandList* cmd);
    // 录制进命令列表：RT 态 → shader 可读态（离屏结果给后续 pass 采样）。
    void record_transition_to_shader_readable(ID3D12GraphicsCommandList* cmd);

private:
    bool create_rtv_heap(uint32_t count);
    void release_attachments();

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;

    // RTV/DSV 用 NON_SHADER_VISIBLE 堆（RTV 不能进 SHADER_VISIBLE 堆）。
    ID3D12DescriptorHeap* rtv_heap_ = nullptr;   // 颜色附件，最多 kMaxColorAttachments
    ID3D12DescriptorHeap* dsv_heap_ = nullptr;   // 深度附件，1 个
    uint32_t rtv_increment_ = 0;
    uint32_t dsv_increment_ = 0;

    static constexpr uint32_t kMaxColorAttachments = 8;

    std::vector<D3D12Texture*> color_textures_;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> color_rtvs_;
    D3D12Texture* depth_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE depth_dsv_{};
    // resolve 目标（单样本）：ResolveSubresource 的目的地。不占描述符槽位。
    D3D12Texture* resolve_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    // bind()/unbind() 是 const 接口（IFramebuffer 约束），但需要记录绑定态。
    mutable bool bound_ = false;
};

} // namespace gryce_engine::render::d3d12
