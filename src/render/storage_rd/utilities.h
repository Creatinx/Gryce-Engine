#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "render/export.h"
#include "math/math.h"

namespace gryce_engine::render {

class RenderContext;
class ITexture;
class IFramebuffer;
class IMesh;

// ---------------------------------------------------------------------------
// RendererUtilities — 渲染工具集抽象接口
// 提供全屏三角形、管线状态管理、纹理/网格转换等通用工具。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererUtilities {
public:
    virtual ~RendererUtilities() = default;

    // 全屏三角形（用于后处理 pass）
    virtual class IMesh* get_fullscreen_mesh() = 0;

    // 管线状态管理
    virtual void set_default_state() = 0;

    // 纹理格式转换
    virtual uint32_t get_texel_size(uint32_t format) const = 0;
    virtual bool is_compressed_format(uint32_t format) const = 0;
};

} // namespace gryce_engine::render