// ===========================================================================
// engine/app/app_host.cpp
// 通用应用宿主实现。设计口径见 app_host.h。
//
// 启动顺序（与 renderer_smoke 的差异都集中在"窗口由脚本决定"这一条）：
//   GCore_Init（不自动载场景） -> Play 模式 -> main.js -> 循环
//   main.js 里 engine.window.create() 才建 GLFW 窗口并 GRender_Init，
//   所以纯逻辑脚本可以完全无窗口运行（CI 里跑逻辑验证很方便）。
// ===========================================================================

#include "app/app_host.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "api/core_api.h"
#include "api/entity_api.h"
#include "api/input_api.h"
#include "api/render_api.h"
#include "api/scene_api.h"
#include "api/types.h"
#include "api/window_api.h"
#include "script/extension_host.h"
#include "script/js_app_bindings.h"
#include "script/script_app.h"
#include "resources/project.h"
#include "resources/project_data.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::app {

namespace {

using script::AppHostServices;
using script::ScriptApplication;
using script::WindowOptions;

// ---------------------------------------------------------------------------
// project.data（与 renderer_smoke 同口径；只保留运行时真正用到的字段）
// ---------------------------------------------------------------------------
struct ProjectConfig {
    std::string title = "Gryce";
    int width = 1280;
    int height = 720;
    std::string render_api = "opengl";
    bool hdr = true;
    int tone_map_mode = 1;
    float exposure = 1.0f;
    bool shadow_enabled = true;
    int shadow_map_size = 2048;
    int point_shadow_size = 512;
    float ambient[3] = {0.2f, 0.22f, 0.26f};
    float ibl_intensity = 1.0f;
    bool default_environment = false;
    bool ssao_enabled = false;
    bool ssr_enabled = false;
    bool ssil_enabled = false;
    bool rt_reflection_enabled = false;
    float rt_reflection_resolution_scale = 1.0f;
    bool rt_reflection_denoise = true;
    // 画面重建模式：off / temporal / dlss（见 docs/zh/rt-reflection.md）
    std::string super_resolution = "temporal";
    float ssr_max_steps = 64.0f;
    float ssr_max_roughness = 0.6f;
    float ssr_thickness = 0.1f;
    float ssr_bilateral = 0.5f;
    float ssr_env_fallback = 1.0f;
    float ssr_resolution_scale = 1.0f;
    bool pcss_enabled = false;
    float pcss_light_size = 0.06f;
    float pcss_max_radius = 12.0f;
    // 抗锯齿：MSAA 抗几何轮廓（默认 4x，1 = 关）；TAA 抗 MSAA 管不到的
    // 反射/后处理/高光细节（默认关，开启后细节略软是时域累积的代价）。
    int msaa_samples = 4;
    bool taa_enabled = false;
    float taa_weight = 0.85f;
    // 启动态：true = 进 Play（运行时语义，组件 on_update 生效）；
    // false = 停在编辑态（编辑器语义，撤销栈才记账）。
    // Play 态下 undo_capture 直接跳过，所以编辑器项目必须显式关掉它，
    // 否则所有编辑都进不了历史，撤销按钮永远是灰的。
    bool start_in_play = true;
    bool loaded = false;
};

std::string trim(std::string s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

ProjectConfig load_project_config(const std::string& project_root) {
    ProjectConfig c;
    const resources::ProjectDataResult pd = resources::read_project_data(project_root);
    if (!pd.found) return c;
    if (!pd.ok) {
        // 读不进来等于配置全丢：用 stderr 直出（此时日志系统可能还没起），
        // 再让所有字段走内置默认值。
        std::fprintf(stderr, "[app] project.data parse failed: %s\n", pd.error.c_str());
        c.loaded = false;
        return c;
    }
    if (!pd.tolerant_note.empty()) {
        std::fprintf(stderr, "[app] %s\n", pd.tolerant_note.c_str());
    }
    try {
        const nlohmann::json& j = pd.data;
        if (j.contains("name") && j["name"].is_string()) c.title = j["name"].get<std::string>();
        if (j.contains("window") && j["window"].is_object()) {
            c.width = j["window"].value("width", c.width);
            c.height = j["window"].value("height", c.height);
            if (j["window"].contains("title") && j["window"]["title"].is_string()) {
                c.title = j["window"]["title"].get<std::string>();
            }
        }
        c.render_api = trim(j.value("render_api", c.render_api));
        c.hdr = j.value("hdr", c.hdr);
        c.tone_map_mode = j.value("tone_map_mode", c.tone_map_mode);
        c.exposure = j.value("exposure", c.exposure);
        c.shadow_enabled = j.value("shadow_enabled", c.shadow_enabled);
        c.shadow_map_size = j.value("shadow_map_size", c.shadow_map_size);
        c.point_shadow_size = j.value("point_shadow_size", c.point_shadow_size);
        c.ambient[0] = j.value("ambient_r", c.ambient[0]);
        c.ambient[1] = j.value("ambient_g", c.ambient[1]);
        c.ambient[2] = j.value("ambient_b", c.ambient[2]);
        c.ibl_intensity = j.value("ibl_intensity", c.ibl_intensity);
        c.default_environment = j.value("default_environment", c.default_environment);
        c.ssao_enabled = j.value("ssao_enabled", c.ssao_enabled);
        c.ssr_enabled = j.value("ssr_enabled", c.ssr_enabled);
        c.ssil_enabled = j.value("ssil_enabled", c.ssil_enabled);
        c.rt_reflection_enabled = j.value("rt_reflection_enabled", c.rt_reflection_enabled);
        c.rt_reflection_resolution_scale =
            j.value("rt_reflection_resolution_scale", c.rt_reflection_resolution_scale);
        c.rt_reflection_denoise = j.value("rt_reflection_denoise", c.rt_reflection_denoise);
        c.super_resolution = j.value("super_resolution", c.super_resolution);
        c.ssr_max_steps = j.value("ssr_max_steps", c.ssr_max_steps);
        c.ssr_max_roughness = j.value("ssr_max_roughness", c.ssr_max_roughness);
        c.ssr_thickness = j.value("ssr_thickness", c.ssr_thickness);
        c.ssr_bilateral = j.value("ssr_bilateral_filter", c.ssr_bilateral);
        c.ssr_env_fallback = j.value("ssr_env_fallback", c.ssr_env_fallback);
        c.ssr_resolution_scale = j.value("ssr_resolution_scale", c.ssr_resolution_scale);
        c.pcss_enabled = j.value("pcss_enabled", c.pcss_enabled);
        c.pcss_light_size = j.value("pcss_light_size", c.pcss_light_size);
        c.pcss_max_radius = j.value("pcss_max_radius", c.pcss_max_radius);
        c.msaa_samples = j.value("msaa_samples", c.msaa_samples);
        c.taa_enabled = j.value("taa_enabled", c.taa_enabled);
        c.taa_weight = j.value("taa_weight", c.taa_weight);
        c.start_in_play = j.value("start_in_play", c.start_in_play);
        c.loaded = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[app] project.data parse failed: %s\n", e.what());
        c.loaded = false;
    }
    return c;
}

GRenderAPI parse_api(const std::string& name, bool* ok) {
    *ok = true;
    if (name == "vulkan") return GRYCE_RENDER_API_VULKAN;
    if (name == "dx12") return GRYCE_RENDER_API_DX12;
    if (name == "webgl") return GRYCE_RENDER_API_WEBGL;
    if (name == "opengl" || name.empty()) return GRYCE_RENDER_API_OPENGL;
    *ok = false;
    return GRYCE_RENDER_API_OPENGL;
}

const char* api_name(GRenderAPI api) {
    switch (api) {
        case GRYCE_RENDER_API_VULKAN: return "vulkan";
        case GRYCE_RENDER_API_DX12: return "dx12";
        case GRYCE_RENDER_API_WEBGL: return "webgl";
        default: return "opengl";
    }
}

// ---------------------------------------------------------------------------
// 宿主服务：绑定层通过它建窗、载场景、退出
// ---------------------------------------------------------------------------
class HostServices final : public AppHostServices {
public:
    ProjectConfig cfg;
    std::string api_override;   // 命令行 --api
    std::string main_scene_override;  // 命令行 --scene（同步给脚本的 main_scene）
    bool show_fps = false;
    bool debug_ui = false;
    bool quit_requested = false;
    int shot_flush_frames = 0;

    bool create_window(const WindowOptions& opts) override {
        if (window_valid()) return true;

        const std::string want = !opts.api.empty() ? opts.api
                               : !api_override.empty() ? api_override
                                                       : cfg.render_api;
        bool ok = true;
        const GRenderAPI api = parse_api(want, &ok);
        if (!ok) {
            GLOG_ERROR("[app] unknown render api '{}'", want);
            return false;
        }

        // 渲染器里"init 前生效"的项
        GRender_SetShadowMapSize(cfg.shadow_map_size);
        GRender_SetPointShadowSize(cfg.point_shadow_size);
        GRender_SetVSync(opts.vsync);
        GRender_SetFPSOverlay(show_fps);
        // 运行时默认不挂 ImGui 调试面板（那是冒烟台/编辑器的东西）
        GRender_SetDebugUI(debug_ui);

        if (GWindow_CreateEx(opts.title.c_str(), opts.width, opts.height,
                             GWINDOW_MODE_WINDOWED, api) != 0) {
            GLOG_ERROR("[app] GWindow_CreateEx failed");
            return false;
        }

        GRenderInitDesc desc{};
        desc.version = sizeof(GRenderInitDesc);
        desc.native_window = GWindow_GetRenderHandle();
        desc.api = api;
        desc.viewport_w = opts.width;
        desc.viewport_h = opts.height;
        desc.sync_mode = false;
        if (GRender_Init(&desc) != 0) {
            GLOG_ERROR("[app] GRender_Init failed ({})", api_name(api));
            GWindow_Destroy();
            return false;
        }

        renderer_ = true;
        apply_render_settings();

        GLOG_INFO("[app] window ready: '{}' {}x{} backend={}", opts.title, opts.width, opts.height,
                  api_name(api));
        return true;
    }

    void destroy_window() override {
        if (renderer_) {
            GRender_Shutdown();
            renderer_ = false;
        }
        if (window_valid()) GWindow_Destroy();
    }

    bool window_valid() const override { return GWindow_IsValid(); }

    void window_size(int* out_w, int* out_h) override {
        int w = 0;
        int h = 0;
        GWindow_GetFramebufferSize(&w, &h);
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
    }

    void set_cursor_locked(bool locked) override { GInput_SetMouseLocked(locked); }

    bool load_scene(const std::string& path) override {
        // 幂等：路径与当前场景相同时直接成功。宿主启动时会按 project.data
        // 的 main_scene 先载一次，脚本惯例上还会再 load 一次同一个场景，没有
        // 这道判断就会白解析一遍（大场景是肉眼可见的启动延迟）。
        // 注意 GScene_GetCurrentPath 返回的是写入长度（仿 GetName 的约定），
        // 不是 0/-1 的成功码。
        char current[512] = {};
        if (GScene_GetCurrentPath(current, static_cast<int>(sizeof(current))) > 0 &&
            path == current) {
            return true;
        }
        return GScene_Load(path.c_str()) == 0;
    }

    std::string current_scene() override {
        char buf[512] = {};
        // 同样按"返回长度"解释：<=0 表示没有当前场景。
        if (GScene_GetCurrentPath(buf, static_cast<int>(sizeof(buf))) <= 0) return {};
        return buf;
    }

    std::string main_scene() override {
        // --scene 覆盖时一律返回覆盖值：脚本无参 load() 拿到的是同一个路径，
        // 于是"命令行指定场景"不会被脚本按配置再载一遍覆盖掉。
        if (!main_scene_override.empty()) return main_scene_override;
        return resources::Project::instance().main_scene();
    }

    void request_quit() override { quit_requested = true; }

    bool has_renderer() const override { return renderer_; }

    float fps() override { return renderer_ ? GRender_GetFPS() : 0.0f; }

    bool screenshot(const std::string& path) override {
        if (!renderer_) return false;
        if (GRender_SaveScreenshot(path.c_str()) != 0) return false;
        shot_flush_frames = 4;   // 渲染线程要到后续帧才真正写盘
        return true;
    }

    void set_rt_reflection(bool enabled) override {
        if (renderer_) GRender_SetRTReflection(enabled);
    }
    bool rt_reflection_enabled() const override {
        return renderer_ ? GRender_IsRTReflectionEnabled() : false;
    }
    void set_ssr(bool enabled) override {
        if (renderer_) GRender_SetSSR(enabled);
    }
    bool ssr_enabled() const override {
        return renderer_ ? GRender_IsSSREnabled() : false;
    }

private:
    void apply_render_settings() {
        GRender_SetHDR(cfg.hdr);
        GRender_SetToneMapMode(cfg.tone_map_mode);
        GRender_SetExposure(cfg.exposure);
        // 抗锯齿在其它渲染开关之前下发：MSAA 会重建 HDR 目标，越早越省一次重建。
        GRender_SetMSAASamples(cfg.msaa_samples);
        GRender_SetTAAEnabled(cfg.taa_enabled);
        GRender_SetTAAWeight(cfg.taa_weight);
        GRender_SetShadowEnabled(cfg.shadow_enabled);
        GRender_SetAmbient(cfg.ambient[0], cfg.ambient[1], cfg.ambient[2]);
        GRender_SetIBLIntensity(cfg.ibl_intensity);
        GRender_SetDefaultEnvironment(cfg.default_environment);
        GRender_SetSSRParams(cfg.ssr_max_steps, cfg.ssr_max_roughness, cfg.ssr_thickness,
                             cfg.ssr_bilateral);
        GRender_SetSSREnvFallback(cfg.ssr_env_fallback);
        GRender_SetSSRResolutionScale(cfg.ssr_resolution_scale);
        GRender_SetPCSSEnabled(cfg.pcss_enabled);
        GRender_SetPCSSParams(cfg.pcss_light_size, cfg.pcss_max_radius, 1.0f);
        GRender_SetSSAO(cfg.ssao_enabled);
        GRender_SetSSR(cfg.ssr_enabled);
        GRender_SetSSIL(cfg.ssil_enabled);
        GRender_SetRTReflection(cfg.rt_reflection_enabled);
        GRender_SetRTReflectionResolutionScale(cfg.rt_reflection_resolution_scale);
        GRender_SetRTReflectionDenoise(cfg.rt_reflection_denoise);
        // 超分模式：DLSS 缺失时引擎侧自动降级为内置时域重建并打日志。
        // 这里按字符串直接映射（超分模块在渲染 DLL 内不导出，宿主只走 C ABI）。
        if (cfg.super_resolution == "off" || cfg.super_resolution == "native") {
            GRender_SetSuperResolutionMode(GRENDER_SR_OFF);
        } else if (cfg.super_resolution == "dlss") {
            GRender_SetSuperResolutionMode(GRENDER_SR_DLSS);
        } else {
            GRender_SetSuperResolutionMode(GRENDER_SR_TEMPORAL);
        }
    }

    bool renderer_ = false;
};

struct AppOptions {
    std::string project_root;
    std::string entry = "res:/main.js";
    std::string scene;        // --scene：显式启动场景（优先于 project.data）
    bool entry_explicit = false;  // --entry 是否由用户显式给出
    std::string api;          // 空 = project.data
    std::string extension;    // 空 = <project>/extension.dll（存在才加载）
    std::string shot;         // --shot：跑满帧数后截图
    int width = 0;            // 0 = project.data
    int height = 0;
    int max_frames = 0;       // 0 = 一直跑
    bool no_extension = false;
    bool show_fps = false;
    bool debug_ui = false;
    bool edit_mode = false;   // --edit-mode：强制停在编辑态（覆盖 project.data）
};

// 用法文本里的程序名取自 argv[0]：现在有两个宿主（GryceApp 通用入口、
// GryceEditor 编辑器专用入口），写死一个名字会让另一个的 --help 显示
// 别人的名字。
void print_usage(const char* argv0) {
    const std::string exe = argv0
        ? std::filesystem::path(argv0).filename().string()
        : std::string("GryceApp");
    std::printf(
        "%s — Gryce 运行时（项目入口是 main.js）\n"
        // 不写死"默认当前目录"：编辑器宿主是把项目根作为参数前置进来的，
        // 这里的缺省值随宿主而变。
        "  --project <dir>      项目根（缺省用宿主内置值或当前目录；res:/ 由此解析）\n"
        "  --entry <path>       入口脚本（默认 res:/main.js）\n"
        "  --scene <path>       显式启动场景，覆盖 project.data 的 main_scene\n"
        "  --api <name>         渲染后端：opengl|vulkan|dx12（默认取 project.data）\n"
        "  --w / --h <px>       窗口尺寸覆盖（默认取 project.data）\n"
        "  --frames <N>         跑满 N 帧后退出（无窗口时用于逻辑验证；0=不限）\n"
        "  --shot <png>         跑满帧数后截图再退出（隐含 --frames 60）\n"
        "  --extension <path>   指定原生扩展动态库（默认 <project>/extension.dll）\n"
        "  --no-extension       不加载任何扩展\n"
        "  --fps                左上角 FPS 叠加（默认关）\n"
        "  --debug-ui           启用渲染器 ImGui 调试面板（默认关）\n"
        "  --edit-mode          停在编辑态（不进 Play、不跑组件 update、撤销栈记账），\n"
        "                       覆盖 project.data 的 start_in_play\n"
        "  --help               显示本帮助\n",
        exe.c_str());
}

} // namespace

int run(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    AppOptions opts;
    opts.project_root = std::filesystem::current_path().string();

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[app] %s 缺少参数\n", flag);
                std::exit(1);
            }
            return argv[++i];
        };
        if (std::strcmp(a, "--project") == 0) opts.project_root = need_value(a);
        else if (std::strcmp(a, "--entry") == 0) { opts.entry = need_value(a); opts.entry_explicit = true; }
        else if (std::strcmp(a, "--scene") == 0) opts.scene = need_value(a);
        else if (std::strcmp(a, "--api") == 0) opts.api = need_value(a);
        else if (std::strcmp(a, "--w") == 0) opts.width = std::atoi(need_value(a));
        else if (std::strcmp(a, "--h") == 0) opts.height = std::atoi(need_value(a));
        else if (std::strcmp(a, "--frames") == 0) opts.max_frames = std::atoi(need_value(a));
        else if (std::strcmp(a, "--shot") == 0) opts.shot = need_value(a);
        else if (std::strcmp(a, "--extension") == 0) opts.extension = need_value(a);
        else if (std::strcmp(a, "--no-extension") == 0) opts.no_extension = true;
        else if (std::strcmp(a, "--fps") == 0) opts.show_fps = true;
        else if (std::strcmp(a, "--debug-ui") == 0) opts.debug_ui = true;
        else if (std::strcmp(a, "--edit-mode") == 0) opts.edit_mode = true;
        else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "[app] 未知参数: %s\n", a);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!opts.shot.empty() && opts.max_frames <= 0) opts.max_frames = 60;

    HostServices host;
    host.cfg = load_project_config(opts.project_root);
    host.show_fps = opts.show_fps;
    host.debug_ui = opts.debug_ui;
    if (!opts.api.empty()) host.api_override = opts.api;
    host.main_scene_override = opts.scene;

    std::printf("[app] project='%s' entry='%s' api=%s\n", opts.project_root.c_str(),
                opts.entry.c_str(),
                !opts.api.empty() ? opts.api.c_str()
                                  : host.cfg.render_api.empty() ? "opengl"
                                                                : host.cfg.render_api.c_str());

    GCoreInitDesc core_desc{};
    core_desc.version = sizeof(GCoreInitDesc);
    core_desc.project_root = opts.project_root.c_str();
    core_desc.enable_reflection = true;

    // 启动场景的优先级（从高到低）：
    //   1) --scene <path>     显式命令行覆盖，载入后忽略 project.data；
    //   2) --entry <script>   显式指定脚本 -> 场景归脚本管（脚本自己 scene.load）；
    //   3) project.data "main_scene"  默认启动场景，宿主自动载入。
    // 编辑态（--edit-mode 或 start_in_play:false）不自动载：文档层自己管场景，
    // 否则会与编辑器打开文档的动作重复解析。
    const bool play_mode = !opts.edit_mode && host.cfg.start_in_play;
    const bool auto_load_scene =
        opts.scene.empty() && !opts.entry_explicit && play_mode;
    if (!opts.scene.empty()) {
        std::printf("[app] scene override: %s\n", opts.scene.c_str());
    } else if (opts.entry_explicit) {
        std::printf("[app] entry script given: scene comes from the script\n");
    } else if (!play_mode) {
        std::printf("[app] edit mode: scene managed by the document layer\n");
    }
    GCore_SetAutoLoadMainScene(auto_load_scene);
    if (GCore_Init(&core_desc) != 0) {
        std::fprintf(stderr, "[app] GCore_Init failed\n");
        return 1;
    }

    // --scene 在 core 初始化之后载入（此时项目根/res: 已挂载）。
    if (!opts.scene.empty()) {
        if (GScene_Load(opts.scene.c_str()) != 0) {
            std::fprintf(stderr, "[app] failed to load scene '%s'\n", opts.scene.c_str());
            return 1;
        }
        std::printf("[app] scene loaded '%s'\n", opts.scene.c_str());
    }

    // 启动态：默认进 Play（运行时语义，组件 on_update 生效）。
    // 编辑器项目在 project.data 里写 "start_in_play": false，或用 --edit-mode：
    // Play 态下引擎的 undo_capture 会跳过（播放期间的改动本来就不该进历史），
    // 所以编辑器必须停在编辑态，否则撤销/脏标记全部失效。
    {
        const bool start_in_play = !opts.edit_mode && host.cfg.start_in_play;
        GCommand cmd{};
        cmd.type = start_in_play ? ECMD_PLAY_MODE : ECMD_STOP_MODE;
        GCore_PushCommand(&cmd);
        std::printf("[app] start mode: %s\n", start_in_play ? "play" : "edit");
    }

    // 扩展路径：显式指定 > <project>/extension.dll；不存在则跳过。
    std::string ext_path = opts.extension;
    if (ext_path.empty() && !opts.no_extension) {
#if defined(_WIN32)
        const char* kExtName = "extension.dll";
#else
        const char* kExtName = "extension.so";
#endif
        const std::filesystem::path candidate =
            std::filesystem::path(opts.project_root) / kExtName;
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) ext_path = candidate.string();
    }

    ScriptApplication app_script;
    app_script.on_context_created = [&](JSContext* ctx) {
        script::install_app_bindings(ctx, &app_script, &host);
        if (!ext_path.empty()) {
            std::string err;
            if (!script::load_extension_library(ctx, ext_path, &err)) {
                GLOG_ERROR("[app] extension load failed: {}", err);
            }
        }
    };

    std::string err;
    if (!app_script.load(opts.entry, &err)) {
        std::fprintf(stderr, "[app] entry load failed: %s\n", err.c_str());
        GCore_Shutdown();
        return 1;
    }
    app_script.start();

    const auto start_time = std::chrono::steady_clock::now();
    auto last_time = start_time;
    int frames = 0;
    bool shot_taken = false;

    while (true) {
        if (GWindow_IsValid()) {
            GWindow_PollEvents();
            // 平台输入 -> 命令队列；没有这一步 engine.input.* 永远是空状态。
            GInput_SyncToCore();
        }

        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last_time).count();
        last_time = now;
        dt = std::max(0.0005f, std::min(dt, 0.1f));

        GCore_BeginFrame(dt);
        app_script.tick(dt);

        if (host.has_renderer()) {
            GRender_BeginFrame();
            GRender_RenderWorld();
            GRender_RenderGizmo();
            GRender_EndFrame();
        }
        GCore_EndFrame();
        ++frames;

        if (host.shot_flush_frames > 0) --host.shot_flush_frames;

        if (GWindow_IsValid() && GWindow_ShouldClose()) host.request_quit();

        // --shot：跑满帧数后请求截图，等渲染线程写盘后再退出。
        if (!opts.shot.empty() && !shot_taken && opts.max_frames > 0 &&
            frames >= opts.max_frames) {
            shot_taken = true;
            if (!host.screenshot(opts.shot)) {
                std::fprintf(stderr, "[app] screenshot request failed: %s\n", opts.shot.c_str());
            }
        }

        const bool frames_done = opts.max_frames > 0 && frames >= opts.max_frames;
        if (host.quit_requested && host.shot_flush_frames == 0) break;
        if (frames_done && (opts.shot.empty() || shot_taken) &&
            host.shot_flush_frames == 0) {
            break;
        }
    }

    const auto elapsed = std::chrono::duration<float>(
        std::chrono::steady_clock::now() - start_time).count();
    app_script.shutdown();
    GInput_SetMouseLocked(false);

    // 顺序不能反：先拆场景（World 持有 GPU 资源句柄），再拆渲染器。
    GCore_Shutdown();
    if (host.has_renderer()) host.destroy_window();

    std::printf("[app] exit: frames=%d elapsed=%.2fs avg_fps=%.1f\n", frames,
                static_cast<double>(elapsed),
                elapsed > 0.0f ? static_cast<double>(frames) / elapsed : 0.0);
    return 0;
}

} // namespace gryce_engine::app
