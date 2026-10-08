#include "d3d12_framebuffer.h"

#include "d3d12_texture.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render::d3d12 {

D3D12Framebuffer::~D3D12Framebuffer() {
    destroy();
}

void D3D12Framebuffer::set_context(ID3D12Device* device, ID3D12CommandQueue* queue) {
    device_ = device;
    queue_ = queue;
}

bool D3D12Framebuffer::create_rtv_heap(uint32_t count) {
    if (!device_ || count == 0) return false;
    if (count > kMaxColorAttachments) count = kMaxColorAttachments;

    // RTV 堆必须是 NON_SHADER_VISIBLE（FLAG_NONE）。传 SHADER_VISIBLE 会直接
    // 让 CreateDescriptorHeap 失败 —— 与 CBV_SRV_UAV 堆的要求正好相反。
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = count;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hd.NodeMask = 0;

    if (rtv_heap_) {
        rtv_heap_->Release();
        rtv_heap_ = nullptr;
    }
    HRESULT hr = device_->CreateDescriptorHeap(&hd, __mingw_uuidof<ID3D12DescriptorHeap>(),
                                              reinterpret_cast<void**>(&rtv_heap_));
    if (FAILED(hr) || !rtv_heap_) {
        GLOG_ERROR("D3D12Framebuffer: RTV heap creation failed (count={}, hr=0x{:08x})", count,
                   static_cast<unsigned>(hr));
        return false;
    }
    rtv_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // 深度用独立的一个 DSV 堆。
    D3D12_DESCRIPTOR_HEAP_DESC dd{};
    dd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dd.NumDescriptors = 1;
    dd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dd.NodeMask = 0;
    if (dsv_heap_) {
        dsv_heap_->Release();
        dsv_heap_ = nullptr;
    }
    hr = device_->CreateDescriptorHeap(&dd, __mingw_uuidof<ID3D12DescriptorHeap>(),
                                       reinterpret_cast<void**>(&dsv_heap_));
    if (FAILED(hr) || !dsv_heap_) {
        GLOG_ERROR("D3D12Framebuffer: DSV heap creation failed (hr=0x{:08x})",
                   static_cast<unsigned>(hr));
        return false;
    }
    dsv_increment_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return true;
}

bool D3D12Framebuffer::create(int width, int height) {
    if (!device_ || width <= 0 || height <= 0) return false;
    destroy();
    width_ = width;
    height_ = height;
    // 先建堆（最多 8 个颜色附件 + 1 个深度）。
    return create_rtv_heap(kMaxColorAttachments);
}

void D3D12Framebuffer::destroy() {
    release_attachments();
    if (rtv_heap_) { rtv_heap_->Release(); rtv_heap_ = nullptr; }
    if (dsv_heap_) { dsv_heap_->Release(); dsv_heap_ = nullptr; }
    width_ = 0;
    height_ = 0;
    bound_ = false;
    rtv_increment_ = 0;
    dsv_increment_ = 0;
}

void D3D12Framebuffer::release_attachments() {
    // 只解除引用，不销毁纹理本体（纹理由资源池拥有）。
    color_textures_.clear();
    color_rtvs_.clear();
    depth_ = nullptr;
    depth_dsv_ = {};
}

void D3D12Framebuffer::attach_color_texture(ITexture* texture) {
    if (!texture || !rtv_heap_) return;
    auto* tex = static_cast<D3D12Texture*>(texture);
    if (!tex->is_valid()) {
        GLOG_WARN("D3D12Framebuffer::attach_color_texture: invalid texture");
        return;
    }
    if (tex->is_depth()) {
        GLOG_WARN("D3D12Framebuffer: depth texture passed to attach_color_texture; "
                  "use attach_depth_texture instead");
        return;
    }

    // 追加一个颜色附件槽位。RTV 描述符写进本堆的第 N 个位置。
    const uint32_t slot = static_cast<uint32_t>(color_rtvs_.size());
    if (slot >= kMaxColorAttachments) {
        GLOG_ERROR("D3D12Framebuffer: exceeded max color attachments ({})", kMaxColorAttachments);
        return;
    }
    if (!create_rtv_heap(slot + 1)) return;   // 扩容（重建堆会清掉旧描述符，下面重写全部）

    color_textures_.push_back(tex);
    color_rtvs_.clear();
    // 扩容后旧 RTV 全失效 → 重新为所有已有附件创建描述符。
    D3D12_CPU_DESCRIPTOR_HANDLE base = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < color_textures_.size(); ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = base;
        h.ptr += static_cast<SIZE_T>(i) * rtv_increment_;
        color_textures_[i]->create_rtv(h);
        color_rtvs_.push_back(h);
    }

    if (width_ == 0 || height_ == 0) {
        width_ = tex->width();
        height_ = tex->height();
    }
}

void D3D12Framebuffer::attach_depth_texture(ITexture* texture) {
    if (!texture || !dsv_heap_) return;
    auto* tex = static_cast<D3D12Texture*>(texture);
    if (!tex->is_valid() || !tex->is_depth()) {
        GLOG_WARN("D3D12Framebuffer::attach_depth_texture: texture is not a depth texture");
        return;
    }
    depth_ = tex;
    depth_dsv_ = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    tex->create_dsv(depth_dsv_);

    if (width_ == 0 || height_ == 0) {
        width_ = tex->width();
        height_ = tex->height();
    }
}

bool D3D12Framebuffer::is_complete() const {
    // D3D12 没有 GL 式的 completeness 检查；语义上"至少有一个颜色或深度附件"即可用。
    return !color_textures_.empty() || depth_ != nullptr;
}

void D3D12Framebuffer::bind() const {
    // 真正的 OM 绑定需要命令列表；由 backend 在绘制前调 record_transition_to_rt
    // + OMSetRenderTargets（用 color_rtv(0..n) / depth_dsv()）。
    bound_ = true;
}

void D3D12Framebuffer::unbind() const {
    // 解绑 = 切回交换链的 back buffer，由 backend 处理。
    bound_ = false;
}

void D3D12Framebuffer::resize(int width, int height) {
    // D3D12 的 framebuffer 只是描述符容器，尺寸信息由附件纹理决定；
    // 这里仅更新记录值（真正的纹理重建由调用方负责）。
    if (width > 0 && height > 0) {
        width_ = width;
        height_ = height;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Framebuffer::color_rtv(uint32_t i) const {
    if (i < color_rtvs_.size()) return color_rtvs_[i];
    return D3D12_CPU_DESCRIPTOR_HANDLE{};
}

D3D12Texture* D3D12Framebuffer::color_texture(uint32_t i) const {
    if (i < color_textures_.size()) return color_textures_[i];
    return nullptr;
}

DXGI_FORMAT D3D12Framebuffer::color_format() const {
    if (!color_textures_.empty() && color_textures_[0]) {
        return color_textures_[0]->dxgi_format();
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT D3D12Framebuffer::depth_format() const {
    if (depth_) return depth_->dxgi_format();
    return DXGI_FORMAT_UNKNOWN;
}

void D3D12Framebuffer::record_transition_to_rt(ID3D12GraphicsCommandList* cmd) {
    if (!cmd) return;
    // 颜色附件：COMMON 或 PIXEL_SHADER_RESOURCE → RENDER_TARGET。
    for (auto* tex : color_textures_) {
        if (!tex || !tex->resource()) continue;
        const D3D12_RESOURCE_STATES before = tex->current_state();
        if (before == D3D12_RESOURCE_STATE_RENDER_TARGET) continue;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex->resource();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cmd->ResourceBarrier(1, &b);
        tex->set_current_state(D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    // 深度附件：→ DEPTH_WRITE。
    if (depth_ && depth_->resource()) {
        const D3D12_RESOURCE_STATES before = depth_->current_state();
        if (before != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = depth_->resource();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            cmd->ResourceBarrier(1, &b);
            depth_->set_current_state(D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }
}

void D3D12Framebuffer::record_transition_to_shader_readable(ID3D12GraphicsCommandList* cmd) {
    if (!cmd) return;
    const D3D12_RESOURCE_STATES target =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    for (auto* tex : color_textures_) {
        if (!tex || !tex->resource()) continue;
        const D3D12_RESOURCE_STATES before = tex->current_state();
        if (before == target) continue;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex->resource();
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = target;
        cmd->ResourceBarrier(1, &b);
        tex->set_current_state(target);
    }
    // 深度附件转成只读（可被 shader 采样，用于阴影图 / 深度重建）。
    if (depth_ && depth_->resource()) {
        const D3D12_RESOURCE_STATES before = depth_->current_state();
        const D3D12_RESOURCE_STATES depth_read =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (before != depth_read) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = depth_->resource();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = before;
            b.Transition.StateAfter = depth_read;
            cmd->ResourceBarrier(1, &b);
            depth_->set_current_state(depth_read);
        }
    }
}

} // namespace gryce_engine::render::d3d12
