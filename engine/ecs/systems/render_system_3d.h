#pragma once

#include "export.h"
#include "ecs/system.h"
#include <unordered_map>

namespace gryce_engine::render {
    class RenderPipeline;
    class RenderingServer;
} // namespace gryce_engine::render

namespace gryce_engine::ecs {

// ---------------------------------------------------------------------------
// RenderSystem3D — 3D 网格渲染系统
// 通过 RenderingServer API 提交渲染数据到渲染管线。
// 同时保留 GPU 上传逻辑。
// ---------------------------------------------------------------------------
class GRYCE_API RenderSystem3D : public ISystem {
public:
    explicit RenderSystem3D(render::RenderPipeline* pipeline = nullptr,
                            render::RenderingServer* rs = nullptr)
        : pipeline_(pipeline), rs_(rs) {}

    const char* name() const override { return "RenderSystem3D"; }
    Phase phase() const override { return Phase::Render; }

    void on_render(scene::Scene& scene, render::RenderContext& ctx) override;

    void set_rendering_server(render::RenderingServer* rs) { rs_ = rs; }

private:
    render::RenderPipeline* pipeline_ = nullptr;
    render::RenderingServer* rs_ = nullptr;
    // 实体ID → 光源RID 映射（避免每帧重复创建）
    std::unordered_map<uint64_t, uint32_t> entity_light_map_;
};

} // namespace gryce_engine::ecs
