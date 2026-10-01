#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "export.h"
#include "components/component.h"
#include "math/math.h"
#include "render/render2d.h"
#include "render/render_context.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// VolumetricLight — 体积光柱 / God Rays
//
// 自包含发射网格组件（与 Terrain 同一套"程序化生成 + generation-guard 上传"做法）：
// 组件在局部空间生成一段沿 -Z 展开的锥体光柱（-Z 就是 Transform 的前向轴，
// Transform::forward() == (0,0,-1)），从实体位置沿前向延伸到 range；渲染管线在
// 前向 pass 的透明阶段用 additivelike 混合叠加绘制（volumelight 着色器），
// 得到一条柔和的放射状光柱。
//
// 几何完全在局部空间生成（与实体变换无关），因此静止/移动的光柱每帧无需重建，
// 只在 range/steps/jitter 变化时重建一次；世界朝向由着色器里的 uModel 旋转前向轴
// 得到（见 volumelight.vert 的 vAxis 计算）。
//
// 字段语义：
//   - range：光柱长度（世界单位），锥体远端的半径按固定半张角 25 度推出
//     （r_far = range*tan(25°)），光柱越远越粗。
//   - intensity：亮度，直接乘到顶点色 rgb 上（在 HDR 中参与 bloom）。
//   - color：光柱颜色（rgb）；color.a 作为整体不透明度。
//   - steps：轴向分段数（clamp 到 2..64），决定光柱沿轴向的细分与落点平滑度；
//     本实现不做光线步进（raymarching），steps 被用作"几何沿轴向的分段质量"，
//     是 falloff 质量参数而非步进次数。
//   - jitter：轴向各环半径的确定性随机扰动幅度（0..1），打散规则的环带、让
//     边缘更柔和；同样不是采样抖动，而是几何层面的柔化参数。
//     这两个字段的真实含义（raymarching 步数 / 逐步抖动）在本实现中不适用，
//     在此显式记录为"已改为落点质量参数"的妥协。
// ---------------------------------------------------------------------------
class GRYCE_API VolumetricLight : public Component {
public:
    float intensity = 1.0f;
    float range = 10.0f;
    render::Color color = render::Color::white();
    int steps = 16;
    float jitter = 0.1f;

    VolumetricLight() = default;
    ~VolumetricLight() override;

    const char* type() const override { return "VolumetricLight"; }

    // 单条顶点流：position(3) + params(4) + color(4) = 44 字节/顶点。
    //   params = (t, cos_inner, cos_outer, range)
    //     t        轴向参数 0（近端）..1（远端）
    //     cos_inner 角向落点高端（视线几乎平行光轴时最亮）
    //     cos_outer 角向落点低端
    //     range     光柱长度，用于相机距离衰减
    //   color.rgb 已乘 intensity，color.a 为颜色不透明度。
    // 与 voluelight 着色器的 attribute location 0..2 一一对应。
    struct ShaftVertex {
        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
        float t = 0.0f;
        float cos_inner = 1.0f;
        float cos_outer = 0.0f;
        float range = 1.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // ---- GPU 侧（由 RenderPipeline::collect_volumetric_lights 每帧调用一次）----
    // 参数变化时重建锥体网格并把上传命令投递到渲染线程。必须在主线程、
    // render_scene 期间调用。
    void prepare_gpu(render::RenderContext* ctx);

    // 销毁 GPU 资源（主线程调用；实际销毁命令排队到渲染线程）
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }
    uint32_t vertex_count() const { return vertex_count_; }

    // 世界空间包围球（视锥剔除）：光柱从实体位置沿前向延伸到 range，包围球取
    // 局部锥体的中段并随世界变换缩放到世界空间。
    void world_bounds(const math::Matrix4f& world, math::Vector3f& center, float& radius) const;

private:
    // 生成局部空间锥体（沿 -Z）。返回顶点数（写入 vertex_count_）。
    void build_geometry();

    render::RHIMeshHandle gpu_mesh_handle_;
    render::RenderContext* ctx_ = nullptr;
    // 待上传/已上传的 CPU 几何；按值捕获进上传命令，保证延迟执行时仍有效。
    std::shared_ptr<const std::vector<ShaftVertex>> vertices_;
    std::shared_ptr<const std::vector<uint32_t>> indices_;
    uint32_t vertex_count_ = 0;

    // 网格代次：主线程递增；queued_generation_ 记录"已投递上传命令"的代次，
    // 命令在渲染线程执行前不重复投递（初值 1 而 queued 为 0，保证首个网格上传）。
    uint64_t geometry_generation_ = 1;
    std::atomic<uint64_t> queued_generation_{0};
    // 几何是否需要在下次 prepare_gpu 重建（参数变化 / 上下文切换）
    bool gpu_dirty_ = true;
    float last_range_ = -1.0f;
    int last_steps_ = -1;
    float last_jitter_ = -1.0f;

    // 组件析构时置 false，延迟执行的命令据此放弃悬垂回调
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components