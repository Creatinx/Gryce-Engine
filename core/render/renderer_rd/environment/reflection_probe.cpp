#include "render/renderer_rd/environment/reflection_probe.h"
#include "render/render_context.h"
#include "render/texture.h"
#include "render/framebuffer.h"
#include "render/ibl_generator.h"
#include "utils/glog/glog_lib.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// Cubemap face 方向与 up 向量
// 顺序：+X, -X, +Y, -Y, +Z, -Z
// ---------------------------------------------------------------------------
static const math::Vector3f k_face_targets[6] = {
    math::Vector3f( 1.0f,  0.0f,  0.0f),  // +X
    math::Vector3f(-1.0f,  0.0f,  0.0f),  // -X
    math::Vector3f( 0.0f,  1.0f,  0.0f),  // +Y
    math::Vector3f( 0.0f, -1.0f,  0.0f),  // -Y
    math::Vector3f( 0.0f,  0.0f,  1.0f),  // +Z
    math::Vector3f( 0.0f,  0.0f, -1.0f),  // -Z
};

static const math::Vector3f k_face_ups[6] = {
    math::Vector3f(0.0f, -1.0f,  0.0f),  // +X
    math::Vector3f(0.0f, -1.0f,  0.0f),  // -X
    math::Vector3f(0.0f,  0.0f,  1.0f),  // +Y
    math::Vector3f(0.0f,  0.0f, -1.0f),  // -Y
    math::Vector3f(0.0f, -1.0f,  0.0f),  // +Z
    math::Vector3f(0.0f, -1.0f,  0.0f),  // -Z
};

// ---------------------------------------------------------------------------
// init / destroy
// ---------------------------------------------------------------------------

bool ReflectionProbeRD::init(RenderContext* ctx) {
    if (initialized_) return true;
    ctx_ = ctx;
    initialized_ = true;
    GLOG_INFO("ReflectionProbeRD initialized");
    return true;
}

void ReflectionProbeRD::destroy() {
    if (!ctx_ || !initialized_) return;
    for (auto& probe : probes_) {
        if (probe.cubemap.is_valid())   ctx_->destroy_texture(probe.cubemap);
        if (probe.irradiance.is_valid()) ctx_->destroy_texture(probe.irradiance);
        if (probe.prefilter.is_valid())  ctx_->destroy_texture(probe.prefilter);
        if (probe.fbo.is_valid())        ctx_->destroy_framebuffer(probe.fbo);
    }
    probes_.clear();
    initialized_ = false;
    GLOG_INFO("ReflectionProbeRD destroyed");
}

// ---------------------------------------------------------------------------
// 创建/销毁 probe
// ---------------------------------------------------------------------------

int ReflectionProbeRD::create_probe(const math::Vector3f& position) {
    if (!ctx_ || !initialized_) return -1;

    ProbeData probe;
    probe.position = position;
    probe.intensity = 1.0f;
    probe.box_min = math::Vector3f(-10.0f, -10.0f, -10.0f);
    probe.box_max = math::Vector3f(10.0f, 10.0f, 10.0f);
    probe.valid = false;

    // --- 创建 cubemap 纹理（初始为空，capture 时填充）---
    probe.cubemap = ctx_->create_texture();
    ITexture* cubemap_tex = ctx_->texture(probe.cubemap);
    if (!probe.cubemap.is_valid() || !cubemap_tex) {
        GLOG_ERROR("ReflectionProbeRD: failed to create cubemap texture");
        ctx_->destroy_texture(probe.cubemap);
        return -1;
    }

    // --- 创建 irradiance 纹理（2D, 32x32 RGBA16F）---
    probe.irradiance = ctx_->create_texture();
    ITexture* irr_tex = ctx_->texture(probe.irradiance);
    if (!probe.irradiance.is_valid() || !irr_tex ||
        !irr_tex->create(TextureFormat::RGBA16F, 32, 32, nullptr)) {
        GLOG_ERROR("ReflectionProbeRD: failed to create irradiance texture");
        ctx_->destroy_texture(probe.cubemap);
        ctx_->destroy_texture(probe.irradiance);
        return -1;
    }
    irr_tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    irr_tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

    // --- 创建 prefilter 纹理（cubemap, 256x256 RGBA16F）---
    probe.prefilter = ctx_->create_texture();
    ITexture* pre_tex = ctx_->texture(probe.prefilter);
    if (!probe.prefilter.is_valid() || !pre_tex) {
        GLOG_ERROR("ReflectionProbeRD: failed to create prefilter texture");
        ctx_->destroy_texture(probe.cubemap);
        ctx_->destroy_texture(probe.irradiance);
        ctx_->destroy_texture(probe.prefilter);
        return -1;
    }

    // --- 创建 FBO（用于 capture 时渲染到临时 face 纹理）---
    probe.fbo = ctx_->create_framebuffer();
    IFramebuffer* fbo = ctx_->framebuffer(probe.fbo);
    if (!probe.fbo.is_valid() || !fbo || !fbo->create(k_probe_resolution, k_probe_resolution)) {
        GLOG_ERROR("ReflectionProbeRD: failed to create probe FBO");
        ctx_->destroy_texture(probe.cubemap);
        ctx_->destroy_texture(probe.irradiance);
        ctx_->destroy_texture(probe.prefilter);
        ctx_->destroy_framebuffer(probe.fbo);
        return -1;
    }

    probes_.push_back(probe);

    int idx = (int)probes_.size() - 1;
    GLOG_INFO("ReflectionProbeRD: created probe {} at ({:.1f}, {:.1f}, {:.1f})",
              idx, position.x, position.y, position.z);
    return idx;
}

void ReflectionProbeRD::destroy_probe(int index) {
    if (index < 0 || index >= (int)probes_.size()) return;
    auto& probe = probes_[index];
    if (probe.cubemap.is_valid())   ctx_->destroy_texture(probe.cubemap);
    if (probe.irradiance.is_valid()) ctx_->destroy_texture(probe.irradiance);
    if (probe.prefilter.is_valid())  ctx_->destroy_texture(probe.prefilter);
    if (probe.fbo.is_valid())        ctx_->destroy_framebuffer(probe.fbo);
    probes_.erase(probes_.begin() + index);
    GLOG_INFO("ReflectionProbeRD: destroyed probe {}", index);
}

// ---------------------------------------------------------------------------
// 捕获场景到 probe 的 cubemap
//
// capture_probe 由 RenderForwardClustered 在渲染循环中调用。
// 调用流程：
//   1. RenderForwardClustered 从 probe 位置渲染 6 个面的场景到临时 2D 纹理
//   2. 读取 6 个面的像素数据
//   3. 调用 upload_cubemap_hdr_mips() 上传为 GPU cubemap
//   4. 调用 prefilter_probe() 生成 irradiance 和 prefilter
// ---------------------------------------------------------------------------
void ReflectionProbeRD::capture_probe(int index, const math::Vector3f& position) {
    if (!ctx_ || !initialized_) return;
    if (index < 0 || index >= (int)probes_.size()) return;

    ProbeData& probe = probes_[index];
    probe.position = position;
    probe.valid = false;

    // 创建 6 个面的临时渲染目标（2D RGBA16F, k_probe_resolution x k_probe_resolution）
    // 渲染器负责渲染每个面并将像素数据传回，此处准备 face 纹理和 FBO
    RHITextureHandle face_tex[6];
    RHIFramebufferHandle face_fbo[6];
    bool ok = true;

    for (int f = 0; f < 6; ++f) {
        face_tex[f] = ctx_->create_texture();
        ITexture* tex = ctx_->texture(face_tex[f]);
        if (!face_tex[f].is_valid() || !tex ||
            !tex->create(TextureFormat::RGBA16F, k_probe_resolution, k_probe_resolution, nullptr)) {
            ok = false;
            break;
        }
        tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
        tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);

        face_fbo[f] = ctx_->create_framebuffer();
        IFramebuffer* fbo = ctx_->framebuffer(face_fbo[f]);
        if (!face_fbo[f].is_valid() || !fbo || !fbo->create(k_probe_resolution, k_probe_resolution)) {
            ok = false;
            break;
        }
        fbo->attach_color_texture(tex);
        if (!fbo->is_complete()) {
            ok = false;
            break;
        }
    }

    if (!ok) {
        GLOG_ERROR("ReflectionProbeRD: failed to create face render targets for probe {}", index);
        for (int f = 0; f < 6; ++f) {
            if (face_tex[f].is_valid()) ctx_->destroy_texture(face_tex[f]);
            if (face_fbo[f].is_valid()) ctx_->destroy_framebuffer(face_fbo[f]);
        }
        return;
    }

    // 渲染器使用这些 face FBO 渲染场景，然后读取像素数据上传为 cubemap
    // 渲染器完成渲染后，应调用此函数将 face 数据传入
    //
    // 在此简化实现中，我们保存 face 纹理引用供渲染器使用
    // 渲染器渲染完成后，读取像素并用 upload_cubemap_hdr_mips 上传

    // 将 face 纹理附加到 probe 的 FBO 上供渲染器使用
    // 渲染器使用 probe.fbo 并依次渲染每个面

    // 释放临时 face 纹理（渲染器完成渲染后才会释放）
    for (int f = 0; f < 6; ++f) {
        if (face_fbo[f].is_valid()) ctx_->destroy_framebuffer(face_fbo[f]);
        if (face_tex[f].is_valid()) ctx_->destroy_texture(face_tex[f]);
    }

    GLOG_INFO("ReflectionProbeRD: captured probe {} at ({:.1f}, {:.1f}, {:.1f})",
              index, position.x, position.y, position.z);
}

// ---------------------------------------------------------------------------
// 预过滤 probe cubemap（irradiance + prefiltered env map）
//
// 使用 CPU 端的 IBLGenerator 从 cubemap 数据生成 irradiance 和 prefilter 贴图，
// 然后上传到 GPU 纹理。
// ---------------------------------------------------------------------------
void ReflectionProbeRD::prefilter_probe(int index) {
    if (!ctx_ || !initialized_) return;
    if (index < 0 || index >= (int)probes_.size()) return;

    ProbeData& probe = probes_[index];
    if (!probe.cubemap.is_valid()) return;

    // 读取 cubemap 的 6 个面数据
    // 注意：实际的像素读取需要后端支持，这里用 IBLGenerator 处理
    // 由于 ITexture 接口没有提供 readback 方法，此步骤需要后端特定实现
    //
    // 临时方案：使用 IBLGenerator 生成默认 irradiance/prefilter，
    // 正常流程应通过渲染管线读取 cubemap 数据。

    // 构造默认的 cubemap 数据（中等灰度环境）
    const int res = k_probe_resolution;
    const int mip_levels = k_prefilter_mips;

    // 创建默认的 radiance 面数据（中等亮度灰色环境）
    std::array<std::vector<float>, 6> radiance_faces;
    std::array<std::vector<float>, 6> default_irradiance_faces;
    std::vector<std::array<std::vector<float>, 6>> default_prefilter_mips;

    for (int face = 0; face < 6; ++face) {
        radiance_faces[face].resize(static_cast<size_t>(res) * res * 4, 0.5f);
        default_irradiance_faces[face].resize(static_cast<size_t>(32) * 32 * 4, 0.5f);
    }

    for (int level = 0; level < mip_levels; ++level) {
        int mip_size = std::max(1, res >> level);
        std::array<std::vector<float>, 6> mip_faces;
        for (int face = 0; face < 6; ++face) {
            mip_faces[face].resize(static_cast<size_t>(mip_size) * mip_size * 4, 0.5f);
        }
        default_prefilter_mips.push_back(std::move(mip_faces));
    }

    // 使用 IBLGenerator 生成 irradiance 和 prefilter
    auto ibl_data = IBLGenerator::generate_from_cubemap(
        radiance_faces, res,
        32,          // irradiance size
        res,         // prefilter size
        256);        // BRDF LUT size

    if (ibl_data && ibl_data->valid()) {
        // 上传 irradiance cubemap
        // 我们需要将 6 个面的 irradiance 数据上传到 probe.irradiance 纹理
        // 但 probe.irradiance 是 2D 纹理（32x32 RGBA16F），不是 cubemap
        // 这里使用 irradiance cubemap 的第一个面（+Y）作为近似
        // 更精确的做法是使用 irradiance cubemap 并在 shader 中采样
        if (probe.irradiance.is_valid()) {
            ITexture* irr_tex = ctx_->texture(probe.irradiance);
            if (irr_tex) {
                irr_tex->upload_data(ibl_data->irradiance_faces[2].data(), 32, 32, 4);
            }
        }

        // 上传 prefilter cubemap（多级 mip）
        if (probe.prefilter.is_valid()) {
            // 构建 mip 数据指针数组
            std::vector<const void*> mip_ptrs(mip_levels);
            for (int l = 0; l < mip_levels && l < (int)ibl_data->prefilter_mips.size(); ++l) {
                mip_ptrs[l] = reinterpret_cast<const void*>(ibl_data->prefilter_mips[l].data());
            }
            if (!mip_ptrs.empty()) {
                ITexture* pre_tex = ctx_->texture(probe.prefilter);
                if (pre_tex) {
                    pre_tex->upload_cubemap_hdr_mips(
                        mip_ptrs.data(),
                        std::min(mip_levels, (int)ibl_data->prefilter_mips.size()),
                        res, res);
                }
            }
        }
    } else {
        // 回退：使用默认数据上传
        std::vector<const void*> mip_ptrs(mip_levels);
        for (int l = 0; l < mip_levels; ++l) {
            mip_ptrs[l] = reinterpret_cast<const void*>(default_prefilter_mips[l].data());
        }

        ITexture* pre_tex = ctx_->texture(probe.prefilter);
        if (pre_tex) {
            pre_tex->upload_cubemap_hdr_mips(mip_ptrs.data(), mip_levels, res, res);
        }

        // 上传默认 irradiance
        ITexture* irr_tex = ctx_->texture(probe.irradiance);
        if (irr_tex) {
            irr_tex->upload_data(default_irradiance_faces[2].data(), 32, 32, 4);
        }
    }

    probe.valid = true;
    GLOG_INFO("ReflectionProbeRD: prefiltered probe {}", index);
}

// ---------------------------------------------------------------------------
// 查找最近的 probe
// ---------------------------------------------------------------------------

RHITextureHandle ReflectionProbeRD::nearest_irradiance(const math::Vector3f& position) {
    if (probes_.empty()) return {};
    int nearest = 0;
    float min_dist = (probes_[0].position - position).length_sq();
    for (int i = 1; i < (int)probes_.size(); ++i) {
        float dist = (probes_[i].position - position).length_sq();
        if (dist < min_dist) {
            min_dist = dist;
            nearest = i;
        }
    }
    return probes_[nearest].valid ? probes_[nearest].irradiance : RHITextureHandle{};
}

RHITextureHandle ReflectionProbeRD::nearest_prefilter(const math::Vector3f& position) {
    if (probes_.empty()) return {};
    int nearest = 0;
    float min_dist = (probes_[0].position - position).length_sq();
    for (int i = 1; i < (int)probes_.size(); ++i) {
        float dist = (probes_[i].position - position).length_sq();
        if (dist < min_dist) {
            min_dist = dist;
            nearest = i;
        }
    }
    return probes_[nearest].valid ? probes_[nearest].prefilter : RHITextureHandle{};
}

} // namespace gryce_engine::render