#include "render/streamline/sl_interface.h"

#include "math/math.h"
#include "utils/glog/glog_lib.h"

#if defined(GRYCE_HAS_STREAMLINE)
#  if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#  endif
// 输出目标（DLSS 写入的 RGBA16F 纹理 + 帧缓冲）需要完整的 RenderContext /
// ITexture / IFramebuffer 定义，这些只在 STREAMLINE 路径下用到。
#  include "render/render_context.h"
#  include "render/framebuffer.h"
#endif

namespace gryce_engine::render {

StreamlineInterface& StreamlineInterface::instance() {
    static StreamlineInterface inst;
    return inst;
}

#if defined(GRYCE_HAS_STREAMLINE)
D3D12ResourceResolver StreamlineInterface::resolver_ = nullptr;
#endif

void StreamlineInterface::set_d3d12_resource_resolver(D3D12ResourceResolver fn) {
#if defined(GRYCE_HAS_STREAMLINE)
    resolver_ = fn;
#else
    (void)fn;
#endif
}

bool StreamlineInterface::init_d3d12(void* device, int output_width, int output_height) {
#if defined(GRYCE_HAS_STREAMLINE)
    if (initialized_) return available_;
    output_width_  = output_width;
    output_height_ = output_height;

    // 动态加载 sl.interposer.dll 的核心 API（绕过 MSVC 导入库，见头文件说明）。
    if (!load_core_api()) {
        GLOG_ERROR("Streamline: failed to load sl.interposer.dll core API");
        return false;
    }

    sl::Preferences prefs{};
    prefs.engine = sl::EngineType::eCustom;
    prefs.renderAPI = sl::RenderAPI::eD3D12;
    const sl::Feature features[] = { sl::kFeatureDLSS, sl::kFeatureReflex };
    prefs.featuresToLoad = features;
    prefs.numFeaturesToLoad = static_cast<uint32_t>(sizeof(features) / sizeof(features[0]));
    prefs.flags = sl::PreferenceFlags::eDisableCLStateTracking
                | sl::PreferenceFlags::eAllowOTA
                | sl::PreferenceFlags::eLoadDownloadedPlugins;

    sl::Result res = slInit_fn(prefs, sl::kSDKVersion);
    if (res != sl::Result::eOk) {
        GLOG_ERROR("Streamline: slInit failed (0x%x)", static_cast<uint32_t>(res));
        return false;
    }

    res = slSetD3DDevice_fn(device);
    if (res != sl::Result::eOk) {
        GLOG_ERROR("Streamline: slSetD3DDevice failed (0x%x)", static_cast<uint32_t>(res));
        slShutdown_fn();
        return false;
    }

    // 加载 DLSS / Reflex 特性插件。
    res = slSetFeatureLoaded_fn(sl::kFeatureDLSS, true);
    if (res != sl::Result::eOk) {
        GLOG_WARN("Streamline: DLSS feature load reported 0x%x", static_cast<uint32_t>(res));
    }
    res = slSetFeatureLoaded_fn(sl::kFeatureReflex, true);
    if (res != sl::Result::eOk) {
        GLOG_WARN("Streamline: Reflex feature load reported 0x%x", static_cast<uint32_t>(res));
    }

    // 可选：取得 DLSS 特性函数（设置质量模式用）。
    if (slGetFeatureFunction_fn) {
        slGetFeatureFunction_fn(sl::kFeatureDLSS, "slDLSSSetOptions",
                                (void*&)slDLSSSetOptions_fn);
    }

    available_   = true;
    initialized_ = true;
    GLOG_INFO("Streamline: initialized (DLSS + Reflex)");
    return true;
#else
    (void)device; (void)output_width; (void)output_height;
    return false;
#endif
}

void StreamlineInterface::shutdown() {
#if defined(GRYCE_HAS_STREAMLINE)
    destroy_output_target(nullptr);
    if (initialized_ && slShutdown_fn) slShutdown_fn();
    initialized_ = false;
    available_   = false;
#else
#endif
}

bool StreamlineInterface::load_core_api() {
#if defined(GRYCE_HAS_STREAMLINE) && defined(_WIN32)
    HMODULE mod = LoadLibraryA("sl.interposer.dll");
    if (!mod) {
        GLOG_ERROR("Streamline: sl.interposer.dll not found next to executable");
        return false;
    }
    auto get = [&](const char* name) -> void* {
        void* p = reinterpret_cast<void*>(GetProcAddress(mod, name));
        if (!p) GLOG_WARN("Streamline: missing export %s", name);
        return p;
    };
    // 核心 API 的 PFun_ 类型定义在全局命名空间（sl_core_api.h），特性函数在
    // namespace sl（sl_dlss.h / sl_reflex.h）。
    slInit_fn               = reinterpret_cast<PFun_slInit*>(get("slInit"));
    slShutdown_fn           = reinterpret_cast<PFun_slShutdown*>(get("slShutdown"));
    slSetD3DDevice_fn       = reinterpret_cast<PFun_slSetD3DDevice*>(get("slSetD3DDevice"));
    slGetFeatureFunction_fn  = reinterpret_cast<PFun_slGetFeatureFunction*>(get("slGetFeatureFunction"));
    slSetConstants_fn       = reinterpret_cast<PFun_slSetConstants*>(get("slSetConstants"));
    slGetNewFrameToken_fn   = reinterpret_cast<PFun_slGetNewFrameToken*>(get("slGetNewFrameToken"));
    slSetTagForFrame_fn     = reinterpret_cast<PFun_slSetTagForFrame*>(get("slSetTagForFrame"));
    slEvaluateFeature_fn    = reinterpret_cast<PFun_slEvaluateFeature*>(get("slEvaluateFeature"));
    slIsFeatureSupported_fn = reinterpret_cast<PFun_slIsFeatureSupported*>(get("slIsFeatureSupported"));
    slSetFeatureLoaded_fn   = reinterpret_cast<PFun_slSetFeatureLoaded*>(get("slSetFeatureLoaded"));
    slIsFeatureLoaded_fn    = reinterpret_cast<PFun_slIsFeatureLoaded*>(get("slIsFeatureLoaded"));
    return slInit_fn && slSetD3DDevice_fn && slEvaluateFeature_fn;
#else
    return false;
#endif
}

void StreamlineInterface::write_row_major(sl::float4x4& dst, const math::Matrix4f& src) {
    // math::Matrix4f 是列主序 m[col*4+row]；sl::float4x4 是行主序，row[r] 为第 r 行、
    // row[r].x/.y/.z/.w 依次是列 0..3（float4 没有 operator[]，用 setRow + float4 构造）。
    // 元素 (row r, col c) = src.m[c*4 + r]。
    for (int r = 0; r < 4; ++r) {
        dst.setRow(r, sl::float4(src.m[r], src.m[4 + r], src.m[8 + r], src.m[12 + r]));
    }
}

bool StreamlineInterface::ensure_output_target(RenderContext* ctx, int out_w, int out_h) {
#if defined(GRYCE_HAS_STREAMLINE)
    if (output_tex_.is_valid() && out_w == output_width_ && out_h == output_height_) {
        return true;
    }
    destroy_output_target(ctx);
    if (!ctx) return false;
    output_tex_ = ctx->create_texture();
    ITexture* tex = ctx->texture(output_tex_);
    if (!output_tex_.is_valid() || !tex ||
        !tex->create(TextureFormat::RGBA16F, out_w, out_h, nullptr)) {
        return false;
    }
    tex->set_filter(TextureFilter::Linear, TextureFilter::Linear);
    tex->set_wrap(TextureWrap::ClampToEdge, TextureWrap::ClampToEdge);
    output_fbo_ = ctx->create_framebuffer();
    IFramebuffer* fbo = ctx->framebuffer(output_fbo_);
    if (!output_fbo_.is_valid() || !fbo || !fbo->create(out_w, out_h)) return false;
    fbo->attach_color_texture(tex);
    if (!fbo->is_complete()) return false;
    output_width_  = out_w;
    output_height_ = out_h;
    return true;
#else
    (void)ctx; (void)out_w; (void)out_h;
    return false;
#endif
}

void StreamlineInterface::destroy_output_target(RenderContext* ctx) {
#if defined(GRYCE_HAS_STREAMLINE)
    if (ctx) {
        if (output_tex_.is_valid()) { ctx->destroy_texture(output_tex_); output_tex_ = {}; }
        if (output_fbo_.is_valid()) { ctx->destroy_framebuffer(output_fbo_); output_fbo_ = {}; }
    }
    output_tex_ = {};
    output_fbo_ = {};
#else
    (void)ctx;
#endif
}

bool StreamlineInterface::dlss_sr_ready() const {
#if defined(GRYCE_HAS_STREAMLINE)
    return available_ && resolver_ != nullptr;
#else
    return false;
#endif
}

RHITextureHandle StreamlineInterface::dlss_sr_render(RenderContext* ctx,
                                                    RHITextureHandle color_tex,
                                                    RHITextureHandle depth_tex,
                                                    RHITextureHandle motion_vectors_tex,
                                                    RHITextureHandle exposure_tex,
                                                    const PostProcessParams& params,
                                                    const DLSSFrameInputs& f,
                                                    int render_w, int render_h) {
#if defined(GRYCE_HAS_STREAMLINE)
    (void)params;
    if (!dlss_sr_ready()) return color_tex;
    if (!ensure_output_target(ctx, output_width_, output_height_)) return color_tex;

    // 解析输入/输出资源（RHI 句柄 → 原生 D3D12 资源）。任一关键资源解析失败则跳过。
    auto resolve = [&](RHITextureHandle h, D3D12ResolvedResource& out) -> bool {
        if (!h.is_valid() || !resolver_) return false;
        // 把 RHI 句柄的地址透传给后端解析器（后端按 RHITextureHandle* 还原原生资源）。
        out = resolver_(static_cast<void*>(&h));
        return out.native != nullptr;
    };
    D3D12ResolvedResource r_color, r_depth, r_mvec, r_exp, r_out;
    if (!resolve(color_tex, r_color)) return color_tex;
    if (!resolve(depth_tex, r_depth)) return color_tex;
    if (!resolve(motion_vectors_tex, r_mvec)) return color_tex;
    if (!resolve(output_tex_, r_out)) return color_tex;
    // exposure 可选
    bool have_exp = resolve(exposure_tex, r_exp);

    sl::Resource res_color(sl::ResourceType::eTex2d, r_color.native, r_color.state);
    sl::Resource res_depth(sl::ResourceType::eTex2d, r_depth.native, r_depth.state);
    sl::Resource res_mvec (sl::ResourceType::eTex2d, r_mvec.native,  r_mvec.state);
    sl::Resource res_out  (sl::ResourceType::eTex2d, r_out.native,  r_out.state);
    sl::Resource res_exp  (sl::ResourceType::eTex2d, r_exp.native,  r_exp.state);

    sl::ResourceTag tag_color (&res_color, sl::kBufferTypeScalingInputColor, sl::eValidUntilEvaluate);
    sl::ResourceTag tag_depth (&res_depth, sl::kBufferTypeDepth,            sl::eValidUntilEvaluate);
    sl::ResourceTag tag_mvec  (&res_mvec,  sl::kBufferTypeMotionVectors,   sl::eValidUntilEvaluate);
    sl::ResourceTag tag_out   (&res_out,   sl::kBufferTypeScalingOutputColor, sl::eValidUntilEvaluate);
    sl::ResourceTag tag_exp   (&res_exp,   sl::kBufferTypeExposure,         sl::eValidUntilEvaluate);

    // slSetTagForFrame 接受 const sl::ResourceTag*（首元素地址）。构造值数组，
    // 让 tags 自然衰变为 sl::ResourceTag*；num_tags 控制实际使用的标签数。
    sl::ResourceTag tags[] = { tag_color, tag_depth, tag_mvec, tag_out, tag_exp };
    uint32_t num_tags = have_exp ? 5u : 4u;

    // 帧 token + 视口。token 由 SL 内部管理，每帧取一次即可。
    sl::FrameToken* token = nullptr;
    if (slGetNewFrameToken_fn(token, nullptr) != sl::Result::eOk || !token) {
        return color_tex;
    }
    sl::ViewportHandle viewport(0);

    // SL 常量（矩阵行主序、不含 jitter；jitter 单独给出）。
    sl::Constants consts{};
    write_row_major(consts.cameraViewToClip, f.current_view_proj);
    // clipToPrevClip = prevVP * inverse(currentVP)，行列主序转写。
    const math::Matrix4f clip_to_prev = f.prev_view_proj * f.current_view_proj.inverse();
    write_row_major(consts.clipToPrevClip, clip_to_prev);
    consts.jitterOffset.x = f.jitter_x_px;
    consts.jitterOffset.y = f.jitter_y_px;
    consts.mvecScale.x = 1.0f / static_cast<float>(render_w);   // 像素空间 MV
    consts.mvecScale.y = 1.0f / static_cast<float>(render_h);
    consts.cameraPos.x = f.camera_pos.x;
    consts.cameraPos.y = f.camera_pos.y;
    consts.cameraPos.z = f.camera_pos.z;
    consts.reset = f.camera_reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    // 可选：DLSS 质量模式（若特性函数可取）。默认 Balanced。
    if (slDLSSSetOptions_fn) {
        sl::DLSSOptions opts{};
        opts.mode = sl::DLSSMode::eBalanced;
        opts.outputWidth  = static_cast<uint32_t>(output_width_);
        opts.outputHeight = static_cast<uint32_t>(output_height_);
        slDLSSSetOptions_fn(viewport, opts);
    }

    if (slSetConstants_fn(consts, *token, viewport) != sl::Result::eOk) return color_tex;
    if (slSetTagForFrame_fn(*token, viewport, tags, num_tags, nullptr) != sl::Result::eOk) {
        return color_tex;
    }
    if (slEvaluateFeature_fn(sl::kFeatureDLSS, *token, nullptr, 0, nullptr) != sl::Result::eOk) {
        return color_tex;
    }
    return output_tex_;
#else
    (void)ctx; (void)color_tex; (void)depth_tex; (void)motion_vectors_tex;
    (void)exposure_tex; (void)params; (void)f; (void)render_w; (void)render_h;
    return color_tex;
#endif
}

bool StreamlineInterface::reflex_ready() const {
#if defined(GRYCE_HAS_STREAMLINE)
    bool loaded = false;
    if (slIsFeatureLoaded_fn) slIsFeatureLoaded_fn(sl::kFeatureReflex, loaded);
    return available_ && loaded;
#else
    return false;
#endif
}

void StreamlineInterface::reflex_mark_gpu_busy_start() {
#if defined(GRYCE_HAS_STREAMLINE)
    // Reflex 标记：SL 通过 slEvaluateFeature(SL_FEATURE_Reflex, ...) 注入，
    // 此处预留 GPU busy 起点（具体调用在 D3D12 present 路径接入，见执行计划 P1.2）。
#else
#endif
}

void StreamlineInterface::reflex_mark_gpu_busy_end() {
#if defined(GRYCE_HAS_STREAMLINE)
#else
#endif
}

void StreamlineInterface::reflex_set_latency_sleep(bool /*enabled*/) {
#if defined(GRYCE_HAS_STREAMLINE)
#else
#endif
}

} // namespace gryce_engine::render
