#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "export.h"
#include "components/component.h"
#include "math/math.h"
#include "render/render2d.h"
#include "render/render_context.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Billboard — 面向相机的带贴图四边形（血条、公告、草木、名牌）
//
// 与 ParticleSystem3D / LineRenderer3D 同一套实例化做法：
//   - 顶点流是 6 个顶点的静态单位四边形角标（corner.xy ∈ [-1,1]），只上传一次
//   - 实例流是每张广告牌的中心/尺寸/不透明度/朝向标志，每帧重建
// 四边形的朝向展开在顶点着色器里完成（用 view 矩阵基向量，或 lock_x_axis 下
// 只取水平相机右向量），因此上传数据与相机无关，CPU 侧不需要取相机朝向。
// ---------------------------------------------------------------------------
class GRYCE_API Billboard : public Component {
public:
    std::string texture_path;
    bool lock_x_axis = true;
    math::Vector2f size = math::Vector2f::one();
    float opacity = 1.0f;
    bool shaded = false;

    Billboard() = default;
    ~Billboard() override;

    const char* type() const override { return "Billboard"; }

    // 实例流：center(3) + size(2) + opacity(1) + flags(2) = 32 字节/张。
    // 四边形几何由 6 顶点的静态角标提供（只上传一次），所以每张广告牌只需
    // 传一份数据；flags.x = lock_x_axis，flags.y = shaded，见顶点着色器。
    struct BillboardInstance {
        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
        float sx = 1.0f;
        float sy = 1.0f;
        float opacity = 1.0f;
        float lock_x = 1.0f;
        float shaded = 0.0f;
    };

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // ---- GPU 侧（由 RenderPipeline::collect_billboards 每帧调用一次）-------
    // 展开为实例流并把上传命令投递到渲染线程。必须在主线程、render_scene 期间调用。
    void prepare_gpu(render::RenderContext* ctx, const math::Matrix4f& model);

    // 销毁 GPU 资源（主线程调用；实际销毁命令排队到渲染线程）
    void invalidate_gpu();

    render::RHIMeshHandle gpu_mesh_handle() const { return gpu_mesh_handle_; }
    render::RHITextureHandle texture_handle() const { return texture_handle_; }

    // 本帧待绘制的实例数（供渲染侧判断是否有内容可画）
    uint32_t instance_count() const { return instance_count_; }

private:
    // 三槽环形实例缓冲：主线程写当前槽，渲染线程可能仍在读上一槽。
    // 槽位以 shared_ptr 捕获进命令队列，容量跨帧保留，避免每帧堆分配。
    static constexpr int k_scratch_slots = 3;
    std::shared_ptr<std::vector<BillboardInstance>> scratch_[k_scratch_slots];
    int scratch_index_ = 0;
    uint32_t instance_count_ = 0;

    render::RHIMeshHandle gpu_mesh_handle_;
    render::RHITextureHandle texture_handle_;
    render::RenderContext* ctx_ = nullptr;
    std::string texture_path_loaded_;

    // 组件析构时置 false，延迟执行的命令据此放弃悬垂回调
    std::shared_ptr<std::atomic<bool>> alive_token_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace gryce_engine::components