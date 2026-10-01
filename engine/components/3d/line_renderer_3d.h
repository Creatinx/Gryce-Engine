#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "export.h"
#include "components/component.h"
#include "math/math.h"
#include "render/render2d.h"
#include "render/render_context.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// LineRenderer3D — 3D 线段/折线（技能指示线、绳子、调试绘制）
//
// 几何在渲染线程按"每段一条实例"展开：
//   - 顶点流是 6 个顶点的角标（corner.x 选起点/终点，corner.y 选两侧），只上传一次
//   - 实例流是每段的起终点/宽度/颜色，每帧按 points 重建
// 面向相机的横向偏移在顶点着色器里用 view 矩阵的基向量算，因此上传数据与相机
// 无关，CPU 侧不需要取相机朝向（与 ParticleSystem3D 同一套做法）。
// ---------------------------------------------------------------------------
class GRYCE_API LineRenderer3D : public Component {
public:
    std::vector<math::Vector3f> points;
    bool loop = false;
    float width = 0.05f;
    render::Color color = render::Color::white();

    LineRenderer3D() = default;
    ~LineRenderer3D() override;

    const char* type() const override { return "LineRenderer3D"; }

    // 段实例：start(3) + end(3) + width(1) + color(4) = 44 字节/段。
    // 四边形几何由 6 顶点的静态角标提供（只上传一次），所以每段只需传一份数据。
    struct SegmentInstance {
        float sx = 0.0f;
        float sy = 0.0f;
        float sz = 0.0f;
        float ex = 0.0f;
        float ey = 0.0f;
        float ez = 0.0f;
        float width = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // ---- GPU 侧（由 RenderPipeline::collect_lines3d 每帧调用一次）-----------
    // 把折线展开为段实例流，并把上传命令投递到渲染线程。必须在主线程、
    // render_scene 期间调用。
    void prepare_gpu(render::RenderContext* ctx, const math::Matrix4f& model);

    // 销毁 GPU 资源（主线程调用；实际销毁命令排队到渲染线程）
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }

    // 本帧待绘制的段数（供渲染侧判断是否有内容可画）
    uint32_t segment_count() const { return segment_count_; }

private:
    void build_segments(const math::Matrix4f& model);

    // 三槽环形实例缓冲：主线程写当前槽，渲染线程可能仍在读上一槽。
    // 槽位以 shared_ptr 捕获进命令队列，容量跨帧保留，避免每帧堆分配。
    static constexpr int k_scratch_slots = 3;
    std::shared_ptr<std::vector<SegmentInstance>> scratch_[k_scratch_slots];
    int scratch_index_ = 0;
    uint32_t segment_count_ = 0;
    // 静态角标几何是否已上传（每个 mesh 只需一次）
    std::atomic<bool> gpu_ready_{false};

    render::RHIMeshHandle gpu_mesh_handle_;
    render::RenderContext* ctx_ = nullptr;

    // 组件析构时置 false，延迟执行的命令据此放弃悬垂回调
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components