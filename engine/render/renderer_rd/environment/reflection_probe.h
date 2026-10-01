#pragma once
#include "render/rhi_handle.h"
#include "math/math.h"

#include <mutex>
#include <vector>

namespace gryce_engine::render {
class RenderContext;

class ReflectionProbeRD {
public:
    static constexpr int k_probe_resolution = 256;
    static constexpr int k_prefilter_mips = 5;

    struct ProbeData {
        math::Vector3f position;
        float intensity = 1.0f;
        math::Vector3f box_min = math::Vector3f(-10, -10, -10);
        math::Vector3f box_max = math::Vector3f(10, 10, 10);
        RHITextureHandle cubemap;       // captured cubemap (RGBA16F, 256x256)
        RHITextureHandle irradiance;    // irradiance (RGBA16F, 32x32)
        RHITextureHandle prefilter;     // prefiltered env map (RGBA16F, 256x256, mips)
        RHIFramebufferHandle fbo;
        bool valid = false;
    };

    ReflectionProbeRD() = default;
    ~ReflectionProbeRD() { destroy(); }

    bool init(RenderContext* ctx);
    void destroy();

    // Create a new probe at position
    int create_probe(const math::Vector3f& position);
    void destroy_probe(int index);

    // Capture the scene into a probe's cubemap (6 faces)
    void capture_probe(int index, const math::Vector3f& position);

    // Pre-filter the captured cubemap (irradiance + prefiltered env map)
    void prefilter_probe(int index);

    // 发布探针的立方体范围与强度。
    // 引擎原先只提供 create_probe 的默认值（±10 / 1.0），组件接线需要按
    // ReflectionProbe 组件的 box_extents / intensity 覆写，故补这两个 setter。
    void set_probe_bounds(int index, const math::Vector3f& box_min, const math::Vector3f& box_max);
    void set_probe_intensity(int index, float intensity);

    // Get probe data
    const ProbeData& get_probe(int index) const { return probes_[index]; }
    int probe_count() const;

    // IBL textures for the nearest probe
    RHITextureHandle nearest_irradiance(const math::Vector3f& position);
    RHITextureHandle nearest_prefilter(const math::Vector3f& position);

private:
    RenderContext* ctx_ = nullptr;
    std::vector<ProbeData> probes_;
    bool initialized_ = false;
    // 组件接线后，探针的创建/捕获在渲染线程执行（create_probe 内含 GPU 资源
    // 创建），而 probe_count()/nearest_*() 在前向绘制的主线程路径读取；
    // 用互斥量串行化 probes_ 的读写，避免跨线程访问同一 vector 造成竞态。
    mutable std::mutex mutex_;
};

} // namespace gryce_engine::render