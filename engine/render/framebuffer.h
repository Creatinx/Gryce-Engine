#pragma once

#include <cstdint>

namespace gryce_engine::render {

// 前向声明（避免循环包含）
class ITexture;

// ---------------------------------------------------------------------------
// IFramebuffer — 跨 API 帧缓冲接口
// ---------------------------------------------------------------------------
class IFramebuffer {
public:
    virtual ~IFramebuffer() = default;

    virtual bool create(int width, int height) = 0;
    virtual void destroy() = 0;

    virtual void bind() const = 0;
    virtual void unbind() const = 0;

    virtual void resize(int width, int height) = 0;

    virtual void attach_color_texture(ITexture* texture) = 0;
    virtual void attach_depth_texture(ITexture* texture) = 0;

    // 附加"解析目标"（Vulkan render pass 的 pResolveAttachments；其余后端无操作）。
    // 仅多样本颜色附件需要：把本 FBO 的 MSAA 颜色解析进 dst（单样本）纹理，
    // 使后续 pass 能像往常一样采样它。dst 必须与颜色附件同格式同尺寸且为 1x。
    virtual void attach_resolve_texture(ITexture* /*texture*/) {}

    // 把本 FBO 的颜色附件解析（resolve）到 dst FBO 的颜色附件。
    // GL：glBlitFramebuffer；D3D12：ResolveSubresource；
    // Vulkan：render pass 的 resolve 附件已在 pass 内自动完成，这里为空操作。
    // 两者均非多样本时调用方不应发起（无意义）；失败返回 false。
    virtual bool resolve_color_to(IFramebuffer* /*dst*/) { return false; }

    // 颜色附件 0 的多样本数（1 = 普通帧缓冲）。供 PSO/管线推导采样数。
    virtual int color_sample_count() const { return 1; }

    virtual bool is_complete() const = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;
};

} // namespace gryce_engine::render
