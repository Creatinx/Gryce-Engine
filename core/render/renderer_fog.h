#pragma once

#include "render/export.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// RendererFog — 雾渲染抽象接口
// 负责体积雾和全局雾效。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererFog {
public:
    virtual ~RendererFog() = default;
};

} // namespace gryce_engine::render