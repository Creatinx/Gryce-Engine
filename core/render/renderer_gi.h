#pragma once

#include "render/export.h"

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// RendererGI — 全局光照抽象接口
// 负责 SDFGI、VoxelGI、Lightmap 等全局光照系统。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API RendererGI {
public:
    virtual ~RendererGI() = default;
};

} // namespace gryce_engine::render