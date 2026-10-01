#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "export.h"
#include "components/component.h"
#include "components/3d/line_renderer_3d.h"
#include "math/math.h"
#include "render/render2d.h"
#include "render/render_context.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// TrailRenderer — 3D 拖尾（弹道、飞船尾焰、刀光）
//
// 几何展开与 LineRenderer3D 完全一致（每段一条实例 + 6 顶点静态角标），并且
// 共用同一份 line3d 着色器与实例布局，所以这里直接复用 SegmentInstance。
// 区别只在数据来源：采样点在 on_update 里按"离上一个点超过 min_vertex_distance"
// 逐个追加、按 lifetime 淘汰，因此尾长由寿命决定，与物体速度无关。
//
// 采样点在世界空间记录（拖尾是物体运动轨迹，不随物体后续变换移动）。
// ---------------------------------------------------------------------------
class GRYCE_API TrailRenderer : public Component {
public:
    // ---- 序列化参数 --------------------------------------------------------
    float lifetime = 0.3f;               // 采样点存活时间（秒）
    float min_vertex_distance = 0.02f;   // 相邻采样点的最小间距（世界单位）
    float width = 0.1f;                  // 拖尾宽度（世界单位）
    render::Color color = render::Color::white();
    bool autodestruct = false;           // 注意：需场景实体销毁接口，当前未接线

    TrailRenderer() = default;
    ~TrailRenderer() override;

    const char* type() const override { return "TrailRenderer"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // 采样：按 min_vertex_distance 追加世界坐标，按 lifetime 淘汰过期点
    void on_update(float dt) override;
    void on_destroy() override;

    // ---- GPU 侧（由 RenderPipeline::collect_lines3d 每帧调用一次）-----------
    // 把采样历史展开为段实例流，并把上传命令投递到渲染线程。必须在主线程、
    // render_scene 期间调用。
    void prepare_gpu(render::RenderContext* ctx);

    // 销毁 GPU 资源（主线程调用；实际销毁命令排队到渲染线程）
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }

    // 本帧待绘制的段数（供渲染侧判断是否有内容可画）
    uint32_t segment_count() const { return segment_count_; }

private:
    struct TrailPoint {
        math::Vector3f position;
        float time = 0.0f;   // 组件本地时间轴（秒）
    };

    void build_segments();

    // 采样历史（世界空间）。容量上限兜底：物体高速运动 + 极小间距时，
    // 单帧就可能在 lifetime 内积累大量点，避免无界增长。
    static constexpr size_t k_max_points = 1024;
    std::vector<TrailPoint> points_;
    float elapsed_ = 0.0f;

    // 三槽环形实例缓冲：主线程写当前槽，渲染线程可能仍在读上一槽。
    // 槽位以 shared_ptr 捕获进命令队列，容量跨帧保留，避免每帧堆分配。
    static constexpr int k_scratch_slots = 3;
    std::shared_ptr<std::vector<LineRenderer3D::SegmentInstance>> scratch_[k_scratch_slots];
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