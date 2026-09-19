#pragma once
#include "render/rhi_handle.h"
#include "math/math.h"

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

    // Get probe data
    const ProbeData& get_probe(int index) const { return probes_[index]; }
    int probe_count() const { return (int)probes_.size(); }

    // IBL textures for the nearest probe
    RHITextureHandle nearest_irradiance(const math::Vector3f& position);
    RHITextureHandle nearest_prefilter(const math::Vector3f& position);

private:
    RenderContext* ctx_ = nullptr;
    std::vector<ProbeData> probes_;
    bool initialized_ = false;
};

} // namespace gryce_engine::render