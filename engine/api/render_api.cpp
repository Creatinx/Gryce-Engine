// render_api.cpp — Gryce C ABI 的渲染层实现：初始化/关机、帧序列
//（BeginFrame → RenderWorld / RenderGizmo / RenderGameView → EndFrame）、视口纹理
// 查询、VSync / 锁帧 / 省电模式、帧统计读数，以及调试 UI 的接管与面板入口。
// 帧级状态集中在文件内的 g_renderer，跨线程访问一律走 g_renderer.mutex。
#include "api/render_api.h"
#include "api/api_guard.h"
#include "api/viewport_api.h"
#include "api/core_api.h"
#include "api/window_api.h"
#include "runtime/engine_context.h"

#include "render/render_context.h"
#include "render/render_pipeline.h"
#include "render/render.h"
#include "render/render2d.h"
// 引擎自带的调试 UI（左上角 FPS/帧时间 + ImGui 调试窗口）。
// ImGuiRenderer 是引擎对 ImGui 的封装（GLFW 平台层 + 每后端 IImGuiBackend），
// 它自己负责 CreateContext —— 所以引擎只在"宿主还没有 ImGui 上下文"时用它，
// 绝不抢编辑器的上下文（见 debug_ui_init / GRender_DrawDebugPanel）。
#include "render/opengl/imgui_renderer.h"
#include "render/imgui_backend.h"
#ifdef GRYCE_HAS_IMGUI
#include <imgui.h>
#endif
#include "assets/asset_manager.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "ecs/world.h"
#include "scene/query.h"
#include "ecs/systems/render_system_2d.h"
#include "math/camera.h"
#include "math/math.h"
#include "scene/scene.h"
#include "utils/glog/glog_lib.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using gryce_engine::render::RenderContext;
using gryce_engine::render::RenderPipeline;
using gryce_engine::render::IRenderBackend;
using gryce_engine::render::create_render_backend;
using gryce_engine::render::RenderAPI;
using gryce_engine::ecs::World;
// 让 scene/components/math 等嵌套命名空间名在本文档中可见；
// Camera 存在 math::Camera 与 components::Camera 两个类型，调用处显式限定。
using namespace gryce_engine;

// 前置声明（**全局作用域**，与后面的定义同作用域）：调试 UI 的"渲染开关"段落
// （在本文件靠前的匿名命名空间里）要用到它。若把声明写进那个匿名命名空间，
// 它会成为与全局定义并列的**另一个重载**，调用处二义性
// —— 实测 GCC 报 "call of overloaded 'apply_screen_space_settings(...)' is ambiguous"。
static void apply_screen_space_settings(RenderPipeline* pipeline);

namespace {

constexpr float kRadToDeg = 180.0f / 3.14159265358979323846f;

struct RendererState {
    bool initialized = false;
    bool sync_mode = false;

    std::unique_ptr<RenderContext> ctx;
    std::unique_ptr<RenderPipeline> pipeline;
    std::unique_ptr<render::IRenderer2D> renderer2d;

    int viewport_w = 1280;
    int viewport_h = 720;
    int gameview_w = 1280;
    int gameview_h = 720;
    GEntityHandle viewport_camera = 0;
    GEntityHandle gameview_camera = 0;
    // 编辑器相机：非 Game 模式直接使用这些参数，不依赖场景实体
    math::Camera editor_camera;
    bool editor_camera_valid = false;
    std::string display_mode = "Shaded";
    bool scene_2d_only = false;

    // Project Settings（渲染质量）—— 持久化值，GRender_Init 时应用到管线
    bool  hdr_enabled = true;
    int   tone_map_mode = 1;   // 0=None, 1=Reinhard, 2=ACES
    float exposure = 1.0f;
    bool  shadow_enabled = true;
    int   shadow_map_size = 2048;
    bool  shadow_map_size_dirty = false;
    // 垂直同步是"重启生效"的持久化配置：GRender_SetVSync 可能在 GRender_Init
    // 之前被调用（此时 ctx 尚不存在），必须先把期望值存下来，init 时再应用。
    // 默认关闭垂直同步：FIFO 呈现会把 present 锁在显示器刷新率上，导致 GPU 在
    // 两次 vblank 之间空转（典型表现就是 "GPU 跑不满 / 帧率被显示器封顶"）。
    // 目标高帧率（如 240fps）或竞技/基准场景应默认 off；需要防撕裂时再显式开启。
    bool  vsync_enabled = false;
    bool  backend_pending = false;
    bool  pipeline_reload_pending = false;
    GRenderAPI pending_backend_api = GRYCE_RENDER_API_OPENGL;
    GRenderAPI current_backend = GRYCE_RENDER_API_OPENGL;
    math::Vector3f ambient = math::Vector3f(0.15f, 0.15f, 0.15f);
    float ibl_intensity = 1.0f;
    bool  default_environment = false;

    // 屏幕空间效果（默认关闭；SSR/SSIL 需要管线自动跑 depth+normal 预通道）
    bool  ssao_enabled = false;
    bool  ssr_enabled = false;
    bool  show_fps = false;      // 左上角白色 FPS 叠加
    float fps = 0.0f;            // 由 GRender_EndFrame 平滑更新
    // ---- 帧统计（调试 UI 读数 + 自动化验证）----
    float frame_ms = 0.0f;          // 帧间隔 EMA（"帧时间"）
    float last_frame_ms = 0.0f;     // 最近一帧的帧间隔
    std::chrono::steady_clock::time_point last_frame_end_time{};
    // ---- 锁帧（帧率上限）----
    int   frame_rate_limit = 0;     // <=0 表示不限制
    // ---- 调试 UI ----
    bool  debug_ui_enabled = true;  // 叠加 + 调试窗口总开关（GRender_SetDebugUI）
    bool  debug_window_open = true; // 调试窗口显示中（窗口内勾选 / F1 切换）
    bool  debug_imgui_ready = false;   // 引擎已接管 ImGui（叠加与窗口都可用）
    std::unique_ptr<render::ImGuiRenderer> debug_imgui;   // 引擎接管的 ImGui
    // 引擎自己那个 ImGui 上下文的裸指针：绘制前后显式切进/切出。
    // （ImGui 的"当前上下文"是最后创建/设置的那个；宿主若在引擎之后又
    //   CreateContext（编辑器就可能这样），NewFrame/Render 会打到宿主的上下文上，
    //   两边 UI 都会错乱 —— 所以引擎每次都显式 SetCurrentContext 自己的那个。）
    ImGuiContext* debug_imgui_ctx = nullptr;
    // 给"UI 绘制命令"用的存活标志（shared_ptr 让命令持有它）：
    // 关闭时先 pause 渲染线程、再置 false —— 之后即使有一条命令被主线程 drain
    // 出来执行，也不会去碰已经销毁的 ImGui 后端。
    std::shared_ptr<bool> debug_ui_alive;
    // 帧时间历史（调试 UI 里的曲线；环形缓冲）
    static constexpr int k_msHistory = 180;
    float ms_history[k_msHistory] = {};
    int   ms_history_count = 0;
    int   ms_history_head = 0;
    bool  gpu_profiling = false;    // 记录 GRender_SetGPUProfiling 的状态
    bool  gpu_culling_enabled = true;   // 调试 UI 勾选框状态（与管线保持同步）
    // MSAA 采样数（前向 HDR pass；1 = 关闭）。init 前下发即生效。
    int   msaa_samples = 4;
    bool  ssil_enabled = false;
    // Scene View 网格线：编辑器辅助显示，默认关闭，由编辑器显式开启。
    bool  grid_enabled = false;
    float ssr_max_steps = 64.0f;
    float ssr_max_roughness = 0.6f;
    float ssr_thickness = 0.01f;
    float ssr_bilateral = 0.5f;

    std::mutex mutex;
};

static RendererState g_renderer;

#ifdef _WIN32
// 锁帧时把系统定时器粒度提到 1ms（见 GRender_EndFrame 的说明），shutdown 时配平。
static bool g_timer_resolution_raised = false;
#endif

static RenderAPI to_internal_api(GRenderAPI api) {
    switch (api) {
        case GRYCE_RENDER_API_VULKAN: return RenderAPI::Vulkan;
        case GRYCE_RENDER_API_DX12:   return RenderAPI::DX12;
        // WebGL（Emscripten 构建）：与 OpenGL 共用 GLBackend，必须显式映射。
        case GRYCE_RENDER_API_WEBGL:  return RenderAPI::WebGL;
        // DX11 未实现：显式映射让 create_render_backend 走"创建失败"分支报错。
        // 绝不能 fall-through 到 OpenGL —— 那样请求 DX 会静默跑成 OpenGL，
        // 用户毫无察觉。
        case GRYCE_RENDER_API_DX11:   return RenderAPI::DX11;
        default: return RenderAPI::OpenGL;
    }
}

static World* get_world() {
    void* ptr = GCore_GetInternalWorldPtr();
    return static_cast<World*>(ptr);
}

// 2D 覆盖层 pass：begin_frame -> RenderSystem2D -> end_frame
static void render_2d_overlay() {
    if (!g_renderer.renderer2d || !g_renderer.ctx) return;
    g_renderer.renderer2d->begin_frame(g_renderer.viewport_w, g_renderer.viewport_h);
    World* world = get_world();
    if (world && world->scene()) {
        ecs::RenderSystem2D sys(g_renderer.renderer2d.get());
        sys.on_render(*world->scene(), *g_renderer.ctx);
    }
    // ---- 左上角白色 FPS / 帧时间 ----
    // 直接画在 2D overlay 层（tonemap 之后、默认帧缓冲之上），因此不受 HDR/后处理影响。
    // ★ 引擎接管 ImGui 时（debug_imgui 非空）改由 ImGui 叠加来画：既避免两处
    //   重复显示，也因为 ImGui 叠加带帧时间曲线与限速状态，信息更全。
    //   这里的 2D 文本路径留给"没有 ImGui / 宿主自持 ImGui"的宿主，行为与原先一致。
    if (g_renderer.show_fps && !g_renderer.debug_imgui) {
        // 2D 相机默认以屏幕中心为原点、y 向上；FPS 要贴在左上角，这里临时切到
        // "左上角为原点、y 向下"的相机。放在场景 2D 之后画，因此不需要恢复。
        g_renderer.renderer2d->set_camera(math::Vector2f(0.0f, 0.0f), 1.0f, true);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "FPS %.1f   frame %.2f ms",
                      static_cast<double>(g_renderer.fps),
                      static_cast<double>(g_renderer.frame_ms));
        // y 取 30：字体图集路径以基线定位，y 太小会把字顶切掉。
        g_renderer.renderer2d->draw_text(12.0f, 30.0f, buf, 22.0f,
                                         render::Color(1.0f, 1.0f, 1.0f, 1.0f));
    }
    g_renderer.renderer2d->end_frame();
}

// ===========================================================================
// 调试 UI：左上角 FPS/帧时间叠加 + ImGui 调试窗口
// ===========================================================================
// 四条必须遵守的约束（都是架构决定的，不是风格问题）：
//
//  1) **谁拥有 ImGui 上下文**：引擎只在宿主还没有上下文
//     （`ImGui::GetCurrentContext() == nullptr`）时接管。编辑器自己会 CreateContext，
//     那种情况下引擎只提供 GRender_DrawDebugPanel()，由宿主在自己的
//     NewFrame()/Render() 之间调用 —— 绝不抢上下文。否则会出现两个 context
//     （ImGui 的"当前上下文"是最后创建的那个），编辑器的 UI 会整块画不出来。
//
//  2) **绘制顺序**：ImGui 的 draw data 以命令形式推入**本帧**的命令缓冲
//     （异步模式由渲染线程执行、同步模式由 execute_pending_sync 直接执行），
//     因此天然排在场景/后处理命令之后 —— UI 盖在最终画面上，截图也包含它。
//
//  3) **面板里的开关不能回调 GRender_* 公开 API**：这段代码运行在
//     GRender_EndFrame 内部，g_renderer.mutex 已被持有，再进公开 API 就是自锁。
//     这里直接改字段 / 调 pipeline、backend（与公开 API 的实现等价）。
//
//  4) **字体**：ImGui 内置字体只有 ASCII 字形，所以面板文案用英文；
//     中文标签会显示成方块（要中文得另外加载带 CJK 的字体 atlas）。
// ===========================================================================

#ifdef GRYCE_HAS_IMGUI

namespace {

// GPU 帧时间读数（**不加锁**：本文件的调试 UI 运行在 GRender_EndFrame 内部，
// g_renderer.mutex 已持有，再去碰公开 API 就是自锁）。
// 后端内部的统计表自带互斥，这里的竞态只影响一个调试数字，可接受。
static double debug_ui_gpu_ms_locked() {
    if (!g_renderer.ctx) return 0.0;
    IRenderBackend* backend = g_renderer.ctx->backend();
    return backend ? backend->gpu_frame_time_ms() : 0.0;
}

// 帧时间历史（环形）。累计的 ms 直接进曲线，不做任何平滑 —— 想看的正是尖峰。
static void debug_ui_push_ms(float ms) {
    RendererState& st = g_renderer;
    st.ms_history[st.ms_history_head] = ms;
    st.ms_history_head = (st.ms_history_head + 1) % RendererState::k_msHistory;
    if (st.ms_history_count < RendererState::k_msHistory) ++st.ms_history_count;
}

// 把环形历史"顺直"成 ImGui 需要的连续数组（最旧 → 最新）。
static int debug_ui_sorted_ms(float* out, int cap) {
    const RendererState& st = g_renderer;
    const int n = st.ms_history_count < cap ? st.ms_history_count : cap;
    for (int i = 0; i < n; ++i) {
        const int idx = (st.ms_history_head - n + i + RendererState::k_msHistory) %
                        RendererState::k_msHistory;
        out[i] = st.ms_history[idx];
    }
    return n;
}

// 左上角读数：FPS / 帧时间 / GPU 时间 + 当前限速状态。
// 用无标题栏、无输入、自动尺寸的窗口，视觉上就是一层叠加（不挡鼠标）。
static void debug_ui_overlay() {
    const RendererState& st = g_renderer;
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("##gryce_fps", nullptr, flags)) {
        const double gpu_ms = debug_ui_gpu_ms_locked();
        ImGui::Text("FPS %.1f   frame %.2f ms", static_cast<double>(st.fps),
                    static_cast<double>(st.frame_ms));
        if (gpu_ms > 0.0) {
            ImGui::SameLine();
            ImGui::Text("  GPU %.2f ms", gpu_ms);
        }
        // 限速那一行：把"谁在限制帧率"讲清楚（VSync / 锁帧 / 省电）
        char limit[96];
        std::snprintf(limit, sizeof(limit), "limit %s   vsync %s   power saver %s",
                      st.frame_rate_limit > 0 ? "on" : "off",
                      st.vsync_enabled ? "on" : "off",
                      (st.pipeline && st.pipeline->power_saver_active()) ? "on" : "off");
        ImGui::TextUnformatted(limit);
        if (st.frame_rate_limit > 0) {
            ImGui::Text("target %d FPS", st.frame_rate_limit);
        }
        // 帧时间曲线：一眼看出尖峰/周期性卡顿（平均值的骗局）
        float sorted[RendererState::k_msHistory];
        const int n = debug_ui_sorted_ms(sorted, RendererState::k_msHistory);
        if (n > 1) {
            ImGui::PlotLines("##frame_ms", sorted, n, 0, "frame ms", 0.0f, 33.3f,
                             ImVec2(200.0f, 36.0f));
        }
    }
    ImGui::End();
}

// 调试窗口：热开关 + 读数。
static void debug_ui_window() {
    RendererState& st = g_renderer;
    ImGui::SetNextWindowPos(ImVec2(8.0f, 150.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 400.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Gryce Debug (F1)", &st.debug_window_open)) {
        ImGui::End();
        return;
    }

    // ---- 段落：锁帧 / 呈现 ----
    if (ImGui::CollapsingHeader("Frame pacing", ImGuiTreeNodeFlags_DefaultOpen)) {
        // VSync：交换链呈现间隔（GL/VK/DX12 都由 set_swap_interval 统一表达）。
        bool vsync = st.vsync_enabled;
        if (ImGui::Checkbox("Vertical sync (VSync)", &vsync)) {
            st.vsync_enabled = vsync;
            if (st.ctx) st.ctx->set_swap_interval(vsync ? 1 : 0);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Locks presentation to the display refresh rate.\n"
                              "Implies tearing-free but caps FPS at the monitor rate.");
        }

        // 锁帧（软件节流）：0 = 不限制。
        bool limited = st.frame_rate_limit > 0;
        if (ImGui::Checkbox("Frame rate limit", &limited)) {
            st.frame_rate_limit = limited ? (st.frame_rate_limit > 0 ? st.frame_rate_limit : 120) : 0;
        }
        if (limited) {
            int fps = st.frame_rate_limit;
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderInt("##limit_fps", &fps, 10, 360, "%d FPS")) {
                st.frame_rate_limit = fps;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("60")) st.frame_rate_limit = 60;
            ImGui::SameLine();
            if (ImGui::SmallButton("120")) st.frame_rate_limit = 120;
            ImGui::SameLine();
            if (ImGui::SmallButton("144")) st.frame_rate_limit = 144;
        } else {
            ImGui::TextDisabled("unlimited (vsync still applies)");
        }

        // 省电模式（会节流阴影重渲 / 动态分辨率，是第三种"锁帧"手段）。
        if (st.pipeline) {
            bool forced = st.pipeline->power_saver_forced_enabled();
            if (ImGui::Checkbox("Power saver (forced)", &forced)) {
                st.pipeline->set_power_saver_forced(forced);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("auto %s / active %s",
                                st.pipeline->power_saver_auto_enabled() ? "on" : "off",
                                st.pipeline->power_saver_active() ? "on" : "off");
        }
    }

    // ---- 段落：GPU 计时 ----
    if (ImGui::CollapsingHeader("GPU timing", ImGuiTreeNodeFlags_DefaultOpen)) {
        const bool supported = st.ctx && st.ctx->gpu_profiling_supported();
        bool prof = st.gpu_profiling;
        ImGui::BeginDisabled(!supported);
        if (ImGui::Checkbox("GPU profiling (timestamps)", &prof)) {
            st.gpu_profiling = prof;
            if (st.ctx) st.ctx->set_gpu_profiling(prof);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Log to console") && st.ctx && st.ctx->backend()) {
            st.ctx->backend()->gpu_profile_dump();
        }
        if (!supported) ImGui::TextDisabled("backend does not support GPU timestamps");
    }

    // ---- 段落：渲染开关（调试画面问题时最常用的几个）----
    if (ImGui::CollapsingHeader("Render")) {
        if (st.pipeline) {
            bool shadows = st.shadow_enabled;
            if (ImGui::Checkbox("Shadows", &shadows)) {
                st.shadow_enabled = shadows;
                st.pipeline->set_shadow_enabled(shadows);
            }
            bool cull = st.gpu_culling_enabled;
            if (ImGui::Checkbox("GPU culling", &cull)) {
                st.gpu_culling_enabled = cull;
                st.pipeline->set_gpu_culling_enabled(cull);
            }
            bool ssr = st.ssr_enabled;
            if (ImGui::Checkbox("SSR", &ssr)) {
                st.ssr_enabled = ssr;
                apply_screen_space_settings(st.pipeline.get());
            }
            float ibl = st.ibl_intensity;
            ImGui::SetNextItemWidth(200.0f);
            if (ImGui::SliderFloat("IBL intensity", &ibl, 0.0f, 2.0f, "%.2f")) {
                st.ibl_intensity = ibl;
                st.pipeline->set_ibl_intensity(ibl);
            }
        }
    }

    // ---- 段落：读数 ----
    if (ImGui::CollapsingHeader("Readout", ImGuiTreeNodeFlags_DefaultOpen)) {
        const double gpu_ms = debug_ui_gpu_ms_locked();
        ImGui::Text("FPS           %.2f  (ImGui %.1f)", static_cast<double>(st.fps),
                    static_cast<double>(ImGui::GetIO().Framerate));
        ImGui::Text("frame time    %.2f ms  (last %.2f ms)",
                    static_cast<double>(st.frame_ms), static_cast<double>(st.last_frame_ms));
        ImGui::Text("GPU time      %s", gpu_ms > 0.0 ? "see below" : "n/a (enable profiling)");
        if (gpu_ms > 0.0) ImGui::Text("              %.3f ms/frame avg", gpu_ms);
        if (st.ctx && st.ctx->backend()) {
            ImGui::Text("backend       %s", st.ctx->backend()->api_name());
            ImGui::Text("swap interval %d", st.vsync_enabled ? 1 : 0);
        }
        ImGui::Text("viewport      %dx%d", st.viewport_w, st.viewport_h);
    }

    ImGui::Separator();
    ImGui::TextDisabled("F1: show/hide this window.");
    ImGui::End();
}

} // namespace

// 画面板（调用方必须已经 NewFrame）。宿主自持 ImGui 上下文时由
// GRender_DrawDebugPanel() 调进来；引擎接管路径则在 debug_ui_submit 里调用。
static void debug_ui_build_panels_locked() {
    if (!g_renderer.debug_ui_enabled) return;
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) {
        g_renderer.debug_window_open = !g_renderer.debug_window_open;
    }
    if (g_renderer.show_fps) debug_ui_overlay();
    if (g_renderer.debug_window_open) debug_ui_window();
}

#endif  // GRYCE_HAS_IMGUI

// 引擎接管 ImGui 时的每帧收尾：NewFrame → 画面板 → Render → 把 draw data
// 以命令形式推入本帧命令缓冲。必须在 GRender_EndFrame 内、且 **g_renderer.mutex
// 已持有**时调用（它读 g_renderer 状态，并会 push_command）。
static void debug_ui_submit_locked() {
#ifdef GRYCE_HAS_IMGUI
    if (!g_renderer.debug_imgui || !g_renderer.ctx) return;
    // ★ 显式切到引擎自己的 ImGui 上下文再 NewFrame/Render：宿主（编辑器）可能在
    //   引擎之后又 CreateContext，那时"当前上下文"是宿主的，不切的话会把宿主的
    //   帧数据再渲染一遍、两边 UI 都会错乱。用完切回宿主，宿主完全不受影响。
    struct ContextSwitch {
        ImGuiContext* engine = nullptr;
        ImGuiContext* prev = nullptr;
        bool armed = false;
        ~ContextSwitch() { if (armed) ImGui::SetCurrentContext(prev); }
    } ctx_switch;
    ctx_switch.engine = g_renderer.debug_imgui_ctx;
    ctx_switch.prev = ImGui::GetCurrentContext();
    if (ctx_switch.engine && ctx_switch.prev != ctx_switch.engine) {
        ImGui::SetCurrentContext(ctx_switch.engine);
        ctx_switch.armed = true;
    }

    if (!g_renderer.debug_ui_enabled) {
        // 关闭时仍然走一遍 NewFrame/Render（空帧），避免上一帧的同步 future 永远不结束。
        g_renderer.debug_imgui->begin_frame();
        g_renderer.debug_imgui->end_frame([](ImDrawData*, std::shared_ptr<std::promise<void>> done) {
            if (done) done->set_value();
        });
        return;
    }

    g_renderer.debug_imgui->begin_frame();
    debug_ui_build_panels_locked();

    render::IImGuiBackend* ui_backend = g_renderer.debug_imgui->backend();
    render::ImGuiRenderer* renderer = g_renderer.debug_imgui.get();
    render::RenderContext* ctx = g_renderer.ctx.get();
    std::shared_ptr<bool> alive = g_renderer.debug_ui_alive;
    renderer->end_frame([ui_backend, renderer, ctx, alive](
                            ImDrawData* draw_data, std::shared_ptr<std::promise<void>> done) {
        // draw_data 只在回调期间有效；异步渲染线程要用就必须先深拷贝
        // （ImGuiRenderer::clone_draw_data 会连同 CmdList / 纹理列表一起复制）。
        std::shared_ptr<ImDrawData> clone = renderer->clone_draw_data(draw_data);
        ctx->push_command([clone, ui_backend, done, alive](IRenderBackend*) {
            // 这里运行在渲染线程：用后端的 ImGui 后端把 draw data 画进**本帧**
            // 已经绑定的渲染目标（因此排在场景之后，截图里也有 UI）。
            // alive 为 false 表示引擎正在关闭（后端可能已销毁）→ 直接跳过。
            if (alive && *alive && ui_backend && clone) {
                ui_backend->render_draw_data(clone.get());
            }
            if (done) done->set_value();
        });
    });
#endif
}

// 在场景中查找主摄像机：优先 is_main，其次名字为 MainCamera，最后任意启用的摄像机。
static scene::Entity* find_main_camera_entity(scene::Scene& scn) {
    scene::Entity* result = nullptr;
    scene::Entity* fallback_by_name = nullptr;
    scene::Entity* any_enabled = nullptr;
    scn.foreach([&](scene::Entity* entity) {
        if (!entity) return;
        auto* cam = entity->get_component<components::Camera>();
        if (!cam || !cam->enabled) return;
        if (!any_enabled) any_enabled = entity;
        if (!fallback_by_name && entity->name() == "MainCamera") fallback_by_name = entity;
        if (cam->is_main && !result) result = entity;
    });
    if (result) return result;
    if (fallback_by_name) return fallback_by_name;
    return any_enabled;
}

// 用实体 Transform + Camera 组件构造渲染管线所需的 math::Camera。
static bool build_scene_camera(scene::Entity* entity, int viewport_w, int viewport_h,
                               math::Camera& out) {
    if (!entity) return false;
    auto* cam = entity->get_component<components::Camera>();
    if (!cam || !cam->enabled) return false;
    auto* t = entity->transform();
    if (!t) return false;

    // 摄像机默认看向 -Z；用实体旋转把该方向变换到世界空间，再反解 yaw/pitch。
    math::Vector3f fwd = t->rotation.rotate_vector(math::Vector3f(0.0f, 0.0f, -1.0f));
    if (fwd.length_sq() < 1e-6f) fwd = math::Vector3f(0.0f, 0.0f, -1.0f);
    fwd = fwd.normalized();

    const float pitch = std::asin(math::clamp(fwd.y, -1.0f, 1.0f));
    const float yaw = std::atan2(fwd.z, fwd.x);

    out.set_position(t->position);
    out.set_yaw(yaw * kRadToDeg);
    out.set_pitch(pitch * kRadToDeg);
    out.set_fov(cam->fov);
    out.set_near_far(cam->near_plane, cam->far_plane);
    out.set_aspect(viewport_h > 0
                       ? static_cast<float>(viewport_w) / static_cast<float>(viewport_h)
                       : 16.0f / 9.0f);
    return true;
}

// 收集场景中的全部光源（最多 8 盏），供 PBR 多光源渲染使用。
static void collect_scene_lights(scene::Scene& scn,
                                 std::vector<RenderPipeline::Light>& out) {
    out.clear();
    scn.foreach([&](scene::Entity* entity) {
        if (!entity || out.size() >= RenderPipeline::k_max_lights) return;
        auto* light = entity->get_component<components::Light>();
        if (!light || !light->enabled) return;
        RenderPipeline::Light l;
        l.type = static_cast<RenderPipeline::LightType>(light->light_type);
        l.direction = light->direction;
        l.color = light->color;
        l.intensity = light->intensity;
        l.range = light->range;
        l.spot_angle = light->spot_angle;
        l.spot_softness = light->spot_softness;
        auto* t = entity->transform();
        l.position = t ? t->position : math::Vector3f::zero();
        out.push_back(l);
    });
    // 兜底：场景没有任何灯时保证至少一个方向光，避免 uLightCount=0 只剩环境光
    // （每个面渲染成纯色）。与 3dtest 的 collect_lights 行为保持一致。
    if (out.empty()) {
        RenderPipeline::Light fallback;
        fallback.direction = math::Vector3f(0.0f, -1.0f, 0.0f);
        out.push_back(fallback);
    }
}

// 同步模式下渲染线程未运行，RenderSystem3D 的上传路径不会执行；
// 这里在绘制前把尚未上传 GPU 的 MeshRenderer / SkinnedMeshRenderer 补传上去。
static void upload_pending_meshes(scene::Scene& scn, RenderContext& ctx) {
    // Per-frame budget: uploading dozens of meshes/materials at once (e.g.
    // after an import) would stall the render frame; cap it and let the rest
    // stay pending so they upload over the next frames.
    constexpr int k_max_uploads_per_frame = 30;
    int uploaded = 0;

    ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
        scn, [&](scene::Entity*, components::MeshRenderer* mr, components::Transform*) {
            if (uploaded >= k_max_uploads_per_frame) return;
            if (!mr || !mr->enabled || mr->mesh_path.empty() || mr->gpu_mesh()) return;
            auto data = assets::AssetManager::instance().load_mesh(mr->mesh_path, mr->submesh_index);
            if (data && !data->empty()) {
                mr->upload_to_gpu(&ctx, data.get(), /*allow_while_running=*/false);
                ++uploaded;
            }
        });

    ecs::foreach_with_components<components::SkinnedMeshRenderer, components::Transform>(
        scn, [&](scene::Entity*, components::SkinnedMeshRenderer* mr, components::Transform*) {
            if (uploaded >= k_max_uploads_per_frame) return;
            if (!mr || !mr->enabled || mr->model_path.empty() || mr->gpu_mesh()) return;
            mr->upload_to_gpu(&ctx, /*allow_while_running=*/false);
            ++uploaded;
        });

    // 编辑器修改材质贴图路径/use 标志后，material 被标记 textures_dirty；
    // 在这里统一重新 upload，让改动下一帧生效。
    auto refresh_dirty_material = [&](render::Material* mat) {
        if (mat && mat->textures_dirty) {
            mat->upload_to_gpu(&ctx);
            mat->textures_dirty = false;
        }
    };
    ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
        scn, [&](scene::Entity*, components::MeshRenderer* mr, components::Transform*) {
            if (mr) refresh_dirty_material(mr->material.get());
        });
    ecs::foreach_with_components<components::SkinnedMeshRenderer, components::Transform>(
        scn, [&](scene::Entity*, components::SkinnedMeshRenderer* mr, components::Transform*) {
            if (mr) refresh_dirty_material(mr->material.get());
        });
}

// 异步模式（渲染线程运行中）：主线程不能直接做 GL 上传，把尚未上传的网格
// 作为命令推给渲染线程执行 —— 与 RenderSystem3D::on_render 同一条路径
//（MeshRenderer::upload_to_gpu 的 allow_while_running=true 分支）。
//
// 参考工程里这一步由编辑器注册的 RenderSystem3D 完成；独立宿主（GryceGame /
// 本 C API）没有注册该系统的入口，若不在这里补齐，异步模式下场景永远不会
// 上传网格——表现为"管线初始化成功、相机与灯光都提交了，但画面只有清屏色"。
static void queue_pending_mesh_uploads(scene::Scene& scn, RenderContext& ctx) {
    struct PendingMesh {
        components::MeshRenderer* mr = nullptr;
        // 持有 shared_ptr 防止 LRU 驱逐/热重载在命令执行前释放 MeshData
        std::shared_ptr<const assets::MeshData> data;
        // 命令可能延迟 1~3 帧才在渲染线程执行，期间组件可能析构；
        // token 在入队时取好，回调据此跳过，避免悬垂指针。
        std::shared_ptr<std::atomic<bool>> token;
    };
    struct PendingSkinned {
        components::SkinnedMeshRenderer* mr = nullptr;
        std::shared_ptr<std::atomic<bool>> token;
    };

    std::vector<PendingMesh> meshes;
    ecs::foreach_with_components<components::MeshRenderer, components::Transform>(
        scn, [&](scene::Entity*, components::MeshRenderer* mr, components::Transform*) {
            if (!mr || !mr->enabled || mr->mesh_path.empty() || mr->gpu_mesh()) return;
            auto data = assets::AssetManager::instance().load_mesh(mr->mesh_path, mr->submesh_index);
            if (data && !data->empty()) {
                meshes.push_back({mr, data, mr->alive_token()});
            }
        });

    std::vector<PendingSkinned> skinned;
    ecs::foreach_with_components<components::SkinnedMeshRenderer, components::Transform>(
        scn, [&](scene::Entity*, components::SkinnedMeshRenderer* mr, components::Transform*) {
            if (!mr || !mr->enabled || mr->model_path.empty() || mr->gpu_mesh()) return;
            if (!mr->model()) return;   // AnimatorSystem 尚未加载模型，本帧跳过
            skinned.push_back({mr, mr->alive_token()});
        });

    if (meshes.empty() && skinned.empty()) return;

    RenderContext* ctx_ptr = &ctx;
    ctx.push_command([meshes, skinned, ctx_ptr](IRenderBackend*) {
        for (const auto& p : meshes) {
            if (!p.token || !p.token->load(std::memory_order_acquire)) continue;
            p.mr->upload_to_gpu(ctx_ptr, p.data.get(), /*allow_while_running=*/true);
        }
        for (const auto& p : skinned) {
            if (!p.token || !p.token->load(std::memory_order_acquire)) continue;
            p.mr->upload_to_gpu(ctx_ptr, /*allow_while_running=*/true);
        }
    });
}
} // namespace

// 把屏幕空间效果（SSAO / SSR / SSIL）的当前设置应用到管线。
// GRender_Init 与后端热切换都会重建管线，两处都必须调用，
// 否则切换后端之后效果开关就丢了。
static void apply_screen_space_settings(RenderPipeline* pipeline) {
    if (!pipeline) return;
    pipeline->set_ssao_enabled(g_renderer.ssao_enabled);
    pipeline->set_ssr_enabled(g_renderer.ssr_enabled);
    pipeline->set_ssil_enabled(g_renderer.ssil_enabled);
    pipeline->set_grid_enabled(g_renderer.grid_enabled);
    pipeline->set_ssr_quality(g_renderer.ssr_max_steps, g_renderer.ssr_max_roughness,
                              g_renderer.ssr_thickness, g_renderer.ssr_bilateral);
    // 默认程序化环境（天空 + IBL）：金属材质需要它才有反射内容
    if (g_renderer.default_environment && !pipeline->has_environment()) {
        pipeline->set_default_environment();
    }
}

// Applies a pending backend switch on the render thread: recreates the
// embedded window with the right client API (Vulkan needs GLFW_NO_API) and
// rebuilds the RenderContext/pipeline/renderer2d with the new backend.
static void apply_backend_switch() {
    const GRenderAPI api = g_renderer.pending_backend_api;
    const bool sync = g_renderer.sync_mode;
    const int w = g_renderer.viewport_w;
    const int h = g_renderer.viewport_h;

    if (GWindow_IsValid()) {
        GWindow_RecreateClientApi(api);
    }

    if (g_renderer.pipeline) {
        g_renderer.pipeline->shutdown();
        g_renderer.pipeline.reset();
    }
    if (g_renderer.renderer2d) {
        g_renderer.renderer2d->shutdown();
        g_renderer.renderer2d.reset();
    }
    if (g_renderer.ctx) {
        g_renderer.ctx->shutdown();
        g_renderer.ctx.reset();
    }
    g_renderer.initialized = false;

    auto backend = create_render_backend(to_internal_api(api));
    if (!backend) {
        GLOG_ERROR("GRender: backend switch failed to create backend");
        g_renderer.backend_pending = false;
        return;
    }
    GWindowHandle handle = GWindow_IsValid() ? GWindow_GetRenderHandle() : nullptr;
    g_renderer.ctx = std::make_unique<RenderContext>();
    if (!g_renderer.ctx->init(handle, std::move(backend))) {
        GLOG_ERROR("GRender: backend switch RenderContext::init failed");
        g_renderer.ctx.reset();
        g_renderer.backend_pending = false;
        return;
    }

    g_renderer.sync_mode = sync;
    g_renderer.viewport_w = w;
    g_renderer.viewport_h = h;
    g_renderer.gameview_w = w;
    g_renderer.gameview_h = h;

    g_renderer.pipeline = std::make_unique<RenderPipeline>();
    g_renderer.pipeline->set_viewport_output_enabled(false);
    g_renderer.pipeline->set_shadow_map_size(g_renderer.shadow_map_size);
    g_renderer.pipeline->set_msaa_samples(g_renderer.msaa_samples);
    if (!g_renderer.pipeline->init(g_renderer.ctx.get(), "res:/shaders")) {
        GLOG_WARN("GRender: backend switch pipeline init failed, clear-only");
        g_renderer.pipeline.reset();
    }
    if (g_renderer.pipeline) {
        g_renderer.pipeline->set_hdr_enabled(g_renderer.hdr_enabled);
        g_renderer.pipeline->set_tone_map_mode(g_renderer.tone_map_mode);
        g_renderer.pipeline->set_exposure(g_renderer.exposure);
        g_renderer.pipeline->set_shadow_enabled(g_renderer.shadow_enabled);
        g_renderer.pipeline->set_ambient(g_renderer.ambient);
        g_renderer.pipeline->set_ibl_intensity(g_renderer.ibl_intensity);
        apply_screen_space_settings(g_renderer.pipeline.get());
    }

    g_renderer.renderer2d = g_renderer.ctx->create_renderer2d();
    if (g_renderer.renderer2d) {
        g_renderer.renderer2d->init(g_renderer.ctx.get());
    }
    if (!sync) {
        g_renderer.ctx->start();
    }

    g_renderer.initialized = true;
    g_renderer.backend_pending = false;
    g_renderer.current_backend = api;
    GLOG_INFO("GRender: backend switched to {}",
              api == GRYCE_RENDER_API_VULKAN ? "vulkan"
              : api == GRYCE_RENDER_API_DX12 ? "dx12"
              : api == GRYCE_RENDER_API_WEBGL ? "webgl"
                                              : "opengl");
}

extern "C" {

int GRender_Init(const GRenderInitDesc* desc) {
    GRYCE_API_GUARD();
    if (!desc || desc->version != sizeof(GRenderInitDesc)) return -1;

    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.initialized) return 0;

    // WebGL2：WebGL context 与创建它的线程绑定，浏览器的 rAF 主循环天然是
    // 单线程模型 —— 强制 sync_mode（渲染线程不启动，命令在主线程内联执行）。
    // 这也避免了 pthreads + SharedArrayBuffer 的 COOP/COEP 部署门槛。
#ifdef __EMSCRIPTEN__
    if (!desc->sync_mode) {
        GLOG_INFO("GRender_Init: WebGL build forces sync_mode=1 "
                  "(WebGL context is main-thread bound)");
    }
    g_renderer.sync_mode = true;
#else
    g_renderer.sync_mode = desc->sync_mode;
#endif

    auto backend = create_render_backend(to_internal_api(desc->api));
    if (!backend) {
        GLOG_ERROR("GRender_Init: failed to create render backend");
        return -1;
    }

    // 优先使用平台层提供的渲染句柄（External HWND 模式下为嵌入的 GLFW 窗口）
    GWindowHandle render_handle = desc->native_window;
    if (GWindow_IsValid()) {
        GWindowHandle platform_render = GWindow_GetRenderHandle();
        if (platform_render) render_handle = platform_render;
    }

    g_renderer.ctx = std::make_unique<RenderContext>();
    if (!g_renderer.ctx->init(render_handle, std::move(backend))) {
        GLOG_ERROR("GRender_Init: RenderContext::init failed");
        g_renderer.ctx.reset();
        return -1;
    }

    g_renderer.viewport_w = desc->viewport_w > 0 ? desc->viewport_w : 1280;
    g_renderer.viewport_h = desc->viewport_h > 0 ? desc->viewport_h : 720;
    g_renderer.gameview_w = g_renderer.viewport_w;
    g_renderer.gameview_h = g_renderer.viewport_h;

    // 垂直同步：必须在渲染线程启动前下发。异步模式下 set_swap_interval 只是把
    // 命令排进建帧队列，而首帧由渲染线程的 begin_frame 应用；此处 running_ 仍为
    // false，会直接落到后端，交换链在第一次呈现时即为期望的 present mode。
    // 旧实现里 GRender_SetVSync 在 init 之前调用会因 ctx 为空被静默丢弃，
    // 交换链始终停留在 FIFO（垂直同步），帧率被锁死在显示器刷新率上。
    g_renderer.ctx->set_swap_interval(g_renderer.vsync_enabled ? 1 : 0);

    // Init render pipeline for 3D scene rendering
    g_renderer.pipeline = std::make_unique<RenderPipeline>();
    // WPF 编辑器通过嵌入的 GLFW HWND 直接显示场景（ViewportHwndHost），
    // 因此 tonemap 必须写入默认帧缓冲（交换链），而不是离屏 viewport FBO。
    // 离屏输出留给需要采样纹理的宿主（如旧 ImGui 编辑器）使用。
    g_renderer.pipeline->set_viewport_output_enabled(false);
    // 应用 Project Settings（阴影贴图尺寸 / MSAA 必须在 init 之前 ——
    // 两者都决定 init 时创建的 GPU 目标形态）
    g_renderer.pipeline->set_shadow_map_size(g_renderer.shadow_map_size);
    g_renderer.pipeline->set_msaa_samples(g_renderer.msaa_samples);
    if (!g_renderer.pipeline->init(g_renderer.ctx.get(), "res:/shaders")) {
        GLOG_WARN("GRender_Init: RenderPipeline init failed (shaders may be missing), falling back to clear-only");
        g_renderer.pipeline.reset();
    }
    if (g_renderer.pipeline) {
        g_renderer.pipeline->set_hdr_enabled(g_renderer.hdr_enabled);
        g_renderer.pipeline->set_tone_map_mode(g_renderer.tone_map_mode);
        g_renderer.pipeline->set_exposure(g_renderer.exposure);
        g_renderer.pipeline->set_shadow_enabled(g_renderer.shadow_enabled);
        g_renderer.pipeline->set_ambient(g_renderer.ambient);
        g_renderer.pipeline->set_ibl_intensity(g_renderer.ibl_intensity);
        apply_screen_space_settings(g_renderer.pipeline.get());
    }

    // 2D 覆盖层渲染器：编辑器视口在 3D 场景之上绘制 2D（Sprite2D/UI 等）。
    // init 必须在 RenderContext::start()（渲染线程）之前调用。
    g_renderer.renderer2d = g_renderer.ctx->create_renderer2d();
    if (g_renderer.renderer2d) {
        g_renderer.renderer2d->init(g_renderer.ctx.get());
        GLOG_INFO("GRender_Init: 2D overlay renderer initialized");
    } else {
        GLOG_WARN("GRender_Init: create_renderer2d returned nullptr");
    }

    // 调试 UI：宿主没有自己的 ImGui 上下文时，引擎接管一套（左上角 FPS/帧时间
    // 叠加 + F1 调试窗口）。必须在渲染线程启动**之前**完成 —— ImGui 后端会建
    // 自己的 PSO/SRV 堆与字体纹理，放到渲染线程跑起来之后再建容易和首帧抢资源。
#ifdef GRYCE_HAS_IMGUI
    if (g_renderer.debug_ui_enabled && desc->native_window && ImGui::GetCurrentContext() == nullptr) {
        auto imgui_backend = g_renderer.ctx->backend()
                                 ? g_renderer.ctx->backend()->create_imgui_backend()
                                 : nullptr;
        if (imgui_backend) {
            auto renderer = std::make_unique<render::ImGuiRenderer>();
            auto* glfw_window = static_cast<GLFWwindow*>(desc->native_window);
            if (renderer->init(glfw_window, std::move(imgui_backend))) {
                g_renderer.debug_imgui = std::move(renderer);
                g_renderer.debug_ui_alive = std::make_shared<bool>(true);
                g_renderer.debug_imgui_ctx = ImGui::GetCurrentContext();   // 引擎自己的上下文
                // ★★ 关键：告诉 ImGui「不要改 OS 光标的显示/模式」★★
                //   ImGui 的 GLFW 后端每帧都会按自己的判断调
                //   glfwSetInputMode(GLFW_CURSOR, GLFW_CURSOR_NORMAL/HIDDEN)，
                //   直接把宿主设的 GLFW_CURSOR_DISABLED（指针锁定）冲掉。
                //   症状就是"按住 Alt 指针刚出来又立刻被抢回去 / 指针锁时有时无"
                //   —— 交互模式里属于致命干扰。NoMouseCursorChange 让 ImGui 只管
                //   自己的 ImGuiMouseCursor，OS 光标由宿主/引擎全权控制。
                ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
                GLOG_INFO("GRender_Init: 调试 UI 已接管 ImGui（F1 切换调试窗口，"
                          "GRender_SetDebugUI(false) 关闭）");
            } else {
                GLOG_WARN("GRender_Init: 调试 UI 的 ImGui 初始化失败，回退到 2D FPS 文本");
            }
        } else {
            GLOG_WARN("GRender_Init: 后端未提供 ImGui 后端，调试窗口不可用"
                      "（左上角仍有 2D FPS/帧时间）");
        }
    }
#endif

    // Async mode: start render thread
    if (!g_renderer.sync_mode) {
        g_renderer.ctx->start();
    }

    g_renderer.current_backend = desc->api;
    g_renderer.backend_pending = false;
    g_renderer.initialized = true;
    GLOG_INFO("GRender_Init: {} mode, {}x{}",
              g_renderer.sync_mode ? "sync" : "async",
              g_renderer.viewport_w, g_renderer.viewport_h);
    return 0;
}

void GRender_Shutdown(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (!g_renderer.initialized) return;

    if (g_renderer.pipeline) {
        g_renderer.pipeline->shutdown();
        g_renderer.pipeline.reset();
    }

    if (g_renderer.renderer2d) {
        g_renderer.renderer2d->shutdown();
        g_renderer.renderer2d.reset();
    }

    // 调试 UI 必须在 ctx 销毁**之前**拆掉（它的 ImGui 后端持有设备资源）。
    // 顺序要点：
    //   1) 先 pause 渲染线程（join）—— 之后不会再有新的 UI 命令并发执行；
    //   2) 把存活标志置 false —— ctx->shutdown() 里 drain 出来的残留 UI 命令
    //      因此变成空操作，不会去碰已销毁的 ImGui 后端；
    //   3) 再销毁 ImGui（内部会 wait_gpu_idle 并释放自己的 SRV 堆/字体纹理）。
    if (g_renderer.debug_imgui) {
        if (g_renderer.ctx && !g_renderer.sync_mode) {
            g_renderer.ctx->pause_render_thread();
        }
        if (g_renderer.debug_ui_alive) *g_renderer.debug_ui_alive = false;
        g_renderer.debug_imgui->shutdown();
        g_renderer.debug_imgui.reset();
        g_renderer.debug_ui_alive.reset();
        g_renderer.debug_imgui_ctx = nullptr;
    }

    if (g_renderer.ctx) {
        g_renderer.ctx->shutdown();
        g_renderer.ctx.reset();
    }

    g_renderer.initialized = false;
    g_renderer.sync_mode = false;
    g_renderer.gpu_profiling = false;
#ifdef _WIN32
    // 与锁定帧时的 timeBeginPeriod(1) 配平（否则会一直影响系统调度粒度）。
    if (g_timer_resolution_raised) {
        timeEndPeriod(1);
        g_timer_resolution_raised = false;
    }
#endif
    // fps / frame_ms 刻意不清零：宿主常在 shutdown 之后打印最后一帧的读数
    // （冒烟程序就是），清零会让"帧率验证"读到 0。只清曲线历史。
    g_renderer.ms_history_count = 0;
    g_renderer.ms_history_head = 0;
    g_renderer.last_frame_end_time = {};
    GLOG_INFO("GRender_Shutdown: renderer destroyed");
}

bool GRender_IsInitialized(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.initialized;
}

void GRender_BeginFrame(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.backend_pending) {
        apply_backend_switch();
        if (!g_renderer.initialized || !g_renderer.ctx ||
            !g_renderer.ctx->is_initialized()) {
            return;
        }
    }
    if (!g_renderer.ctx || !g_renderer.ctx->is_initialized()) return;

    if (g_renderer.sync_mode) {
        auto* backend = g_renderer.ctx->backend();
        if (backend) backend->begin_frame();
    }
    // Runtime shadow map resize: applied on the render thread (owns the GL
    // context), requested by GRender_SetShadowMapSize from the UI thread.
    if (g_renderer.shadow_map_size_dirty && g_renderer.pipeline) {
        g_renderer.pipeline->set_shadow_map_size(g_renderer.shadow_map_size);
        if (!g_renderer.pipeline->resize_shadow_map(g_renderer.ctx.get())) {
            GLOG_ERROR("GRender: failed to resize shadow map to {}",
                       g_renderer.shadow_map_size);
        }
        g_renderer.shadow_map_size_dirty = false;
    }
    // Hot reload of the render pipeline: rebuild shaders/FBOs/post-process
    // targets in place while preserving the current configuration.
    if (g_renderer.pipeline_reload_pending && g_renderer.pipeline) {
        g_renderer.pipeline_reload_pending = false;
        if (!g_renderer.pipeline->hot_reload()) {
            GLOG_ERROR("GRender: pipeline hot reload failed");
        }
    }
    // Async mode: render thread handles begin_frame
}

// Renders the current world through the pipeline (shared by SceneView / GameView).
static void render_world_internal(GEntityHandle camera_override = 0) {
    if (!g_renderer.ctx || !g_renderer.ctx->is_initialized()) return;

    auto* world = get_world();
    if (!world || !world->scene()) {
        // No world yet — just clear to dark gray
        if (g_renderer.sync_mode) {
            auto* backend = g_renderer.ctx->backend();
            if (backend) backend->clear(0.15f, 0.15f, 0.15f, 1.0f);
        } else {
            g_renderer.ctx->clear(0.15f, 0.15f, 0.15f, 1.0f);
        }
        return;
    }

    // Set viewport
    if (g_renderer.sync_mode) {
        auto* backend = g_renderer.ctx->backend();
        if (backend) backend->set_viewport(0, 0, g_renderer.viewport_w, g_renderer.viewport_h);
    } else {
        g_renderer.ctx->set_viewport(0, 0, g_renderer.viewport_w, g_renderer.viewport_h);
    }

    // 2D 场景编辑器：只渲染 2D 画布（不混 3D）
    if (g_renderer.scene_2d_only) {
        if (g_renderer.sync_mode) {
            auto* backend = g_renderer.ctx->backend();
            if (backend) backend->clear(0.12f, 0.14f, 0.18f, 1.0f);
        } else {
            g_renderer.ctx->clear(0.12f, 0.14f, 0.18f, 1.0f);
        }
        render_2d_overlay();
        return;
    }

    // Render via pipeline if available
    if (g_renderer.pipeline && g_renderer.pipeline->is_valid()) {
        // 先补传网格，再解析摄像机/光源，最后渲染。
        // 同步模式：主线程直接上传；异步模式：作为命令推给渲染线程上传。
        if (g_renderer.sync_mode) {
            upload_pending_meshes(*world->scene(), *g_renderer.ctx);
        } else {
            queue_pending_mesh_uploads(*world->scene(), *g_renderer.ctx);
        }
        g_renderer.pipeline->set_viewport(g_renderer.viewport_w, g_renderer.viewport_h);
        // 编辑器场景（非 Game 模式）：直接使用编辑器提供的相机，不依赖场景实体。
        // Game 模式：通过 GGameView_SetCamera 指定的实体读取相机。
        math::Camera camera;
        if (camera_override != 0) {
            // GameView：从场景实体读取相机
            scene::Entity* camera_entity = gryce_core::EntityResolver::resolve(camera_override);
            if (!camera_entity) {
                camera_entity = find_main_camera_entity(*world->scene());
            }
            if (!build_scene_camera(camera_entity,
                    g_renderer.viewport_w, g_renderer.viewport_h, camera)) {
                // 兜底用编辑器相机
                camera = g_renderer.editor_camera;
            }
        } else {
            // SceneView：直接使用编辑器相机
            if (g_renderer.editor_camera_valid) {
                camera = g_renderer.editor_camera;
            } else if (scene::Entity* main_cam = find_main_camera_entity(*world->scene())) {
                // 独立 exe / 无编辑器相机时回退到场景主相机（Lua 驱动的 FPS
                // 相机等），避免 GRender_RenderWorld 永远用固定机位渲染。
                if (!build_scene_camera(main_cam,
                        g_renderer.viewport_w, g_renderer.viewport_h, camera)) {
                    camera.set_position(math::Vector3f(0.0f, 2.0f, 5.0f));
                    camera.set_yaw(0.0f);
                    camera.set_pitch(-30.0f);
                    camera.set_fov(60.0f);
                    camera.set_near_far(0.1f, 1000.0f);
                }
            } else {
                camera.set_position(math::Vector3f(0.0f, 2.0f, 5.0f));
                camera.set_yaw(0.0f);
                camera.set_pitch(-30.0f);
                camera.set_fov(60.0f);
                camera.set_near_far(0.1f, 1000.0f);
            }
        }
        camera.set_aspect(g_renderer.viewport_h > 0
            ? static_cast<float>(g_renderer.viewport_w) / static_cast<float>(g_renderer.viewport_h)
            : 16.0f / 9.0f);
        g_renderer.pipeline->set_camera(camera);
        std::vector<RenderPipeline::Light> lights;
        collect_scene_lights(*world->scene(), lights);
        g_renderer.pipeline->set_lights(lights);
        g_renderer.pipeline->render_scene(*world->scene(), *g_renderer.ctx);

        // 2D 覆盖层：在 3D 场景（tonemap 到默认帧缓冲）之上绘制 2D 组件
        //（Sprite2D / Label / ColorRect / TileMap 等，按 CanvasLayer 分层）。
        render_2d_overlay();
    } else {
        // No pipeline — fallback: let world render systems push commands
        world->render(*g_renderer.ctx);
    }

}

void GRender_RenderWorld(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    render_world_internal();
}

void GRender_RenderGizmo(void) {
    GRYCE_API_GUARD();
    // Gizmo 渲染入口：当前为空实现（操作 Gizmo 与视口工具栏尚未接入），
    // 保留 ABI 占位，调用方无需判空。
}

void GRender_RenderGameView(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    // 相机已独立（gameview_camera 优先）；FBO/纹理仍与 SceneView 共用，
    // GRender_GetGameViewTexture() 返回视口纹理。独立 FBO 留待双管线改造。
    render_world_internal(g_renderer.gameview_camera);
}

void GRender_SetDisplayMode(const char* mode) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (!mode || mode[0] == '\0') return;
    g_renderer.display_mode = mode;
    GLOG_INFO("GRender_SetDisplayMode: {}", g_renderer.display_mode);
    // 线框等显示模式当前只记录状态并写日志：下推到后端/管线的真正线框
    // 渲染尚未接入，UI 侧已接线，设置可即时查询。
}

void GRender_EndFrame(void) {
    render::RenderContext* ctx = nullptr;
    bool sync_mode = false;
    // 帧统计：用真实墙钟帧间隔（= "帧时间"）同时算 FPS 与 ms。
    //   * fps 用指数平滑，避免数字剧烈跳动；
    //   * frame_ms 存最近一帧的原始值（调试 UI 的曲线要的就是尖峰，不能平滑）；
    //   * 历史环形缓冲供调试 UI 画曲线（周期性卡顿在平均值里看不出来）。
    {
        static std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (dt > 1e-5f && dt < 1.0f) {
            const float inst = 1.0f / dt;
            std::lock_guard lock(g_renderer.mutex);
            g_renderer.fps = (g_renderer.fps <= 0.0f) ? inst : (g_renderer.fps * 0.9f + inst * 0.1f);
            g_renderer.last_frame_ms = dt * 1000.0f;
            g_renderer.frame_ms = (g_renderer.frame_ms <= 0.0f)
                                      ? g_renderer.last_frame_ms
                                      : (g_renderer.frame_ms * 0.9f + g_renderer.last_frame_ms * 0.1f);
#ifdef GRYCE_HAS_IMGUI
            debug_ui_push_ms(g_renderer.last_frame_ms);
#endif
        }
    }
    {
        std::lock_guard api(gryce_core::api_mutex());
        std::lock_guard lock(g_renderer.mutex);
        if (!g_renderer.ctx || !g_renderer.ctx->is_initialized()) return;
        ctx = g_renderer.ctx.get();
        sync_mode = g_renderer.sync_mode;
        // 调试 UI（左上角读数 + 调试窗口）：必须在 present/execute_pending_sync
        // **之前**把 draw data 推进本帧命令流，这样它排在场景命令之后，
        // 与场景一起提交、一起被截图。
        debug_ui_submit_locked();
        if (g_renderer.sync_mode) {
            // Execute queued render commands under both locks, but defer the
            // swap to present_swap() below so a vsync stall inside
            // glfwSwapBuffers can never block the editor UI thread, which
            // needs the API lock for its 60Hz tick.
            ctx->execute_pending_sync();
        } else {
            g_renderer.ctx->present();
        }
    }
    // Swap outside the API/renderer locks: glfwSwapBuffers can block on vsync
    // indefinitely (e.g. window occluded / display mode change) and must not
    // starve the UI thread.
    if (ctx && sync_mode) {
        ctx->present_swap();
    }

    // ---- 锁帧（帧率上限）----
    // 放在提交/present 之后、锁全部释放之后：纯软件节流，不阻塞渲染线程，
    // 也不会在持锁时睡觉（否则会卡住编辑器的 UI 线程）。
    // 与 VSync 相互独立：VSync 锁在显示器刷新率上，这里锁在任意目标帧率上，
    // 两者同时开时谁更严格谁生效。
    //
    // ★ Windows 实战要点：默认系统定时器粒度约 **15.6ms**，直接 sleep_for(剩余)
    //   会每次多睡十几毫秒 —— 实测 30/60/144 三档都稳定多出 ~14ms（46.7/31.1/
    //   21.8ms）。所以这里两手并用：
    //     1) timeBeginPeriod(1) 把定时器粒度提到 1ms（进程级，shutdown 时配平）；
    //     2) 睡眠只睡到"还剩 ~1ms"，最后 1ms 自旋等 —— 自旋成本很小（<1ms/帧），
    //        但能把限速精度拉到 0.2ms 级。
    int limit_fps = 0;
    {
        std::lock_guard lock(g_renderer.mutex);
        limit_fps = g_renderer.frame_rate_limit;
    }
    if (limit_fps > 0) {
#ifdef _WIN32
        if (!g_timer_resolution_raised) {
            // 把系统定时器粒度提到 1ms（不调它 ::Sleep 的粒度约 15.6ms）。
            timeBeginPeriod(1);
            g_timer_resolution_raised = true;
        }
#endif
        const auto now = std::chrono::steady_clock::now();
        const auto& prev = g_renderer.last_frame_end_time;
        const double target_ms = 1000.0 / static_cast<double>(limit_fps);
        if (prev.time_since_epoch().count() != 0) {
            const double spent_ms =
                std::chrono::duration<double, std::milli>(now - prev).count();
            double remain_ms = target_ms - spent_ms;
            // 粗等：留 1ms 给下面的自旋，避免睡过头。单次只睡一次（不再分片重试），
            // 因为分片重试会把每次的粒度误差叠加起来。
            if (remain_ms > 2.0) {
                const double wait_ms = remain_ms - 2.0;
#ifdef _WIN32
                // ★ 必须用 Win32 ::Sleep：MinGW 的 std::this_thread::sleep_for
                //   走 winpthreads，实测 timeBeginPeriod(1) 之后 10ms 仍会睡成
                //   19ms（锁帧因此永远压在 ~23fps）。::Sleep 遵守系统定时器粒度。
                const DWORD ms = static_cast<DWORD>(wait_ms);
                if (ms > 0) ::Sleep(ms);
#else
                std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(wait_ms));
#endif
            }
            // 再补一次 1ms 级的细睡（::Sleep 的整数粒度会向上取整，这一档把它吃掉），
            // 剩下不到 1ms 交给自旋 —— 自旋窗口越小 CPU 越省。
            {
                const auto t = std::chrono::steady_clock::now();
                const double left = target_ms -
                    std::chrono::duration<double, std::milli>(t - now).count();
#ifdef _WIN32
                if (left > 1.2) ::Sleep(1);
#else
                if (left > 1.2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
            }
            // 细等：自旋（每轮重算剩余时间，因此期间改目标帧率/关限速都能立刻响应）。
            for (;;) {
                const auto t = std::chrono::steady_clock::now();
                remain_ms = target_ms -
                            std::chrono::duration<double, std::milli>(t - now).count();
                if (remain_ms <= 0.0) break;
                int cur = 0;
                {
                    std::lock_guard lock(g_renderer.mutex);
                    cur = g_renderer.frame_rate_limit;
                }
                if (cur <= 0 || cur != limit_fps) break;
                std::this_thread::yield();
            }
        }
    }
    {
        std::lock_guard lock(g_renderer.mutex);
        g_renderer.last_frame_end_time = std::chrono::steady_clock::now();
    }
}

void GRender_SetScene2D(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.scene_2d_only = enabled;
    GLOG_INFO("GRender_SetScene2D: {}", enabled);
}

// --- Project Settings ---

void GRender_SetHDR(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.hdr_enabled = enabled;
    if (g_renderer.pipeline) g_renderer.pipeline->set_hdr_enabled(enabled);
}
bool GRender_IsHDR(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.hdr_enabled;
}

// MSAA 采样数（前向 HDR pass）。init 之前调用直接生效；init 之后调用会
// 同步重建 HDR 目标（request_resize force）。1 = 关闭；实际上限由后端
// 能力协商决定，创建失败自动降级。
void GRender_SetMSAASamples(int samples) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.msaa_samples = samples;
    if (g_renderer.pipeline) {
        g_renderer.pipeline->set_msaa_samples(samples);
        // MSAA 句柄是 init 时创建的：改采样数必须重建 HDR 目标。
        // force=true 跳过"尺寸未变"早退；run_with_gpu_context_sync 保证
        // 在渲染线程上下文里改建（与窗口 resize 同一路径）。
        g_renderer.pipeline->request_resize(g_renderer.viewport_w, g_renderer.viewport_h, true);
    }
}
int GRender_GetMSAASamples(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.pipeline ? g_renderer.pipeline->msaa_samples() : g_renderer.msaa_samples;
}
void GRender_SetToneMapMode(int mode) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.tone_map_mode = mode;
    if (g_renderer.pipeline) g_renderer.pipeline->set_tone_map_mode(mode);
}
int GRender_GetToneMapMode(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.tone_map_mode;
}
void GRender_SetExposure(float exposure) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.exposure = exposure;
    if (g_renderer.pipeline) g_renderer.pipeline->set_exposure(exposure);
}
float GRender_GetExposure(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.exposure;
}
void GRender_SetShadowEnabled(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.shadow_enabled = enabled;
    if (g_renderer.pipeline) g_renderer.pipeline->set_shadow_enabled(enabled);
}
bool GRender_IsShadowEnabled(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.shadow_enabled;
}
void GRender_SetShadowMapSize(int size) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (size < 64) size = 64;
    if (size > 8192) size = 8192;
    g_renderer.shadow_map_size = size;
    // 只有管线已存在时才是"运行时改尺寸"，需要下一帧重建。
    // init 之前调用只是写入配置：GRender_Init 会按该值创建阴影贴图；若这里
    // 无条件置 dirty，init 完成后第一帧会再跑一次 resize_shadow_map，而
    // 异步模式下（渲染线程持有 GL 上下文）该重建会先销毁旧级联 FBO 再创建
    // 失败，结果是阴影彻底消失 —— 表现为"配置了阴影却看不到阴影"。
    g_renderer.shadow_map_size_dirty = (g_renderer.pipeline != nullptr);
}
int GRender_GetShadowMapSize(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.shadow_map_size;
}
int GRender_RebuildPipeline(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (!g_renderer.initialized || !g_renderer.pipeline) return -1;
    // 请求在下一个 GRender_BeginFrame 由渲染线程应用
    g_renderer.pipeline_reload_pending = true;
    return 0;
}
void GRender_SetAmbient(float r, float g, float b) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.ambient = math::Vector3f(r, g, b);
    if (g_renderer.pipeline) g_renderer.pipeline->set_ambient(g_renderer.ambient);
}
void GRender_GetAmbient(float* r, float* g, float* b) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (r) *r = g_renderer.ambient.x;
    if (g) *g = g_renderer.ambient.y;
    if (b) *b = g_renderer.ambient.z;
}
void GRender_SetIBLIntensity(float intensity) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.ibl_intensity = intensity;
    if (g_renderer.pipeline) g_renderer.pipeline->set_ibl_intensity(intensity);
}
float GRender_GetIBLIntensity(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.ibl_intensity;
}

void GRender_SetDefaultEnvironment(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.default_environment = enabled;
    if (!g_renderer.pipeline) return;
    if (enabled) {
        // 已有环境（HDR/天空盒派生）时不覆盖
        if (!g_renderer.pipeline->has_environment()) {
            g_renderer.pipeline->set_default_environment();
        }
    } else if (g_renderer.pipeline->default_environment_enabled()) {
        g_renderer.pipeline->clear_environment();
    }
}

bool GRender_IsDefaultEnvironmentEnabled(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.default_environment;
}

// --- 屏幕空间效果（SSAO / SSR / SSIL）---

void GRender_SetSSAO(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.ssao_enabled = enabled;
    if (g_renderer.pipeline) g_renderer.pipeline->set_ssao_enabled(enabled);
}
bool GRender_IsSSAOEnabled(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.ssao_enabled;
}
void GRender_SetSSR(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.ssr_enabled = enabled;
    if (g_renderer.pipeline) g_renderer.pipeline->set_ssr_enabled(enabled);
}
bool GRender_IsSSREnabled(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.ssr_enabled;
}
void GRender_SetSSIL(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.ssil_enabled = enabled;
    if (g_renderer.pipeline) g_renderer.pipeline->set_ssil_enabled(enabled);
}
bool GRender_IsSSILEnabled(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.ssil_enabled;
}

// --- Scene View 网格线（编辑器辅助显示，默认关闭）---

void GRender_SetGrid(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.grid_enabled = enabled;
    if (g_renderer.pipeline) g_renderer.pipeline->set_grid_enabled(enabled);
}
bool GRender_IsGridEnabled(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.grid_enabled;
}
void GRender_SetSSRParams(float max_steps, float max_roughness, float thickness,
                          float bilateral) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.ssr_max_steps = max_steps;
    g_renderer.ssr_max_roughness = max_roughness;
    g_renderer.ssr_thickness = thickness;
    g_renderer.ssr_bilateral = bilateral;
    if (g_renderer.pipeline) {
        g_renderer.pipeline->set_ssr_quality(max_steps, max_roughness, thickness, bilateral);
    }
}

void GRender_SetSSREnvFallback(float strength) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_ssr_env_fallback(strength);
}

void GRender_SetSSRResolutionScale(float scale) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_ssr_resolution_scale(scale);
}

void GRender_SetDebugView(int mode) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_debug_view(mode);
}

void GRender_SetPCSSEnabled(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_pcss_enabled(enabled);
}

void GRender_SetPCSSParams(float light_size, float max_radius_texels, float tap_scale) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) {
        g_renderer.pipeline->set_pcss_params(light_size, max_radius_texels, tap_scale);
    }
}

// 低/中/高配画质预设（仅切换运行时质量开关，不改动场景内容）
void GRender_SetQualityPreset(int preset) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    const int shadow_size = (preset == 2) ? 1024 : 2048;  // Low=1024，中/高=2048
    const bool pcss = (preset == 0);
    const bool ssao = (preset == 0);
    const bool ssil = (preset == 0);
    const bool ssr  = false;
    // 阴影尺寸变更走与 GRender_SetShadowMapSize 相同的延迟重建路径
    g_renderer.shadow_map_size = shadow_size;
    g_renderer.shadow_map_size_dirty = (g_renderer.pipeline != nullptr);
    g_renderer.shadow_enabled = true;
    if (g_renderer.pipeline) {
        g_renderer.pipeline->set_shadow_enabled(true);
        g_renderer.pipeline->set_pcss_enabled(pcss);
        g_renderer.pipeline->set_ssr_enabled(ssr);
        g_renderer.pipeline->set_ssao_enabled(ssao);
        g_renderer.pipeline->set_ssil_enabled(ssil);
    }
}

// 动态分辨率（DRS）控制 —— 默认关闭，开启后按实测帧时间自适应缩放内部渲染分辨率
void GRender_SetAdaptiveResolution(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_adaptive_resolution(enabled);
}
void GRender_SetResolutionScale(float scale) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_resolution_scale(scale);
}
void GRender_SetDynamicResolutionTargetFPS(float fps) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_dynamic_resolution_target_fps(fps);
}
void GRender_SetDynamicResolutionMinScale(float scale) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_dynamic_resolution_min_scale(scale);
}
void GRender_SetGPUCullingEnabled(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.gpu_culling_enabled = enabled;   // 调试 UI 勾选框状态同步
    if (g_renderer.pipeline) g_renderer.pipeline->set_gpu_culling_enabled(enabled);
}

void GRender_SetOcclusionCullingEnabled(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_occlusion_culling_enabled(enabled);
}

bool GRender_IsOcclusionCullingActive() {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.pipeline ? g_renderer.pipeline->occlusion_culling_active() : false;
}

bool GRender_IsGPUCullingAvailable() {
    GRYCE_API_GUARD();
    if (g_renderer.pipeline) return g_renderer.pipeline->gpu_culling_available();
    return false;
}

void GRender_SetShadowCacheEnabled(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_shadow_cache_enabled(enabled);
}
void GRender_SetInstancingBatching(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_instancing_batching_enabled(enabled);
}

void GRender_SetFPSOverlay(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.show_fps = enabled;
}

void GRender_SetFrameRateLimit(int fps) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.frame_rate_limit = fps > 0 ? fps : 0;
}

int GRender_GetFrameRateLimit(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.frame_rate_limit;
}

float GRender_GetFPS(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.fps;
}

float GRender_GetFrameTimeMs(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.frame_ms;
}

float GRender_GetGPUFrameTimeMs(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    IRenderBackend* backend = g_renderer.ctx ? g_renderer.ctx->backend() : nullptr;
    return backend ? static_cast<float>(backend->gpu_frame_time_ms()) : 0.0f;
}

void GRender_SetDebugUI(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.debug_ui_enabled = enabled;
    // 打开调试 UI 至少应该看得见左上角读数。
    if (enabled) g_renderer.show_fps = true;
}

bool GRender_IsDebugUIActive(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    // "active" = 引擎确实在驱动这套 Overlay/窗口（宿主自持 ImGui 时返回 false，
    // 那种情况下请宿主自己调 GRender_DrawDebugPanel()）。
    return g_renderer.debug_imgui != nullptr;
}

void GRender_DrawDebugPanel(void) {
#ifdef GRYCE_HAS_IMGUI
    // 宿主自持 ImGui 上下文时的入口：调用方必须处于 NewFrame() 与 Render() 之间。
    if (!ImGui::GetCurrentContext()) return;
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    debug_ui_build_panels_locked();
#endif
}

void GRender_SetSSRDebugView(int mode) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_ssr_debug_view(mode);
}

// --- 材质预设枚举 ---

int GRender_GetMaterialPresetCount(void) {
    GRYCE_API_GUARD();
    return render::Material::preset_count();
}

namespace {

int copy_preset_string(const char* src, char* out_buf, int buf_size) {
    if (!out_buf || buf_size <= 0) return -1;
    if (!src) src = "";
    int i = 0;
    for (; src[i] != '\0' && i < buf_size - 1; ++i) out_buf[i] = src[i];
    out_buf[i] = '\0';
    return i;
}

} // namespace

int GRender_GetMaterialPresetName(int index, char* out_buf, int buf_size) {
    GRYCE_API_GUARD();
    if (index < 0 || index >= render::Material::preset_count()) return -1;
    return copy_preset_string(render::Material::preset_name_at(index), out_buf, buf_size);
}

int GRender_GetMaterialPresetLabel(int index, char* out_buf, int buf_size) {
    GRYCE_API_GUARD();
    if (index < 0 || index >= render::Material::preset_count()) return -1;
    return copy_preset_string(render::Material::preset_label_at(index), out_buf, buf_size);
}

GTextureHandle GRender_GetViewportTexture(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (!g_renderer.pipeline || !g_renderer.pipeline->is_valid()) return nullptr;
    auto* tex = g_renderer.pipeline->viewport_color_texture();
    return static_cast<GTextureHandle>(tex);
}

GTextureHandle GRender_GetGameViewTexture(void) {
    GRYCE_API_GUARD();
    // 运行模式（PlayMode）下 GameView 尚无独立 FBO：返回与 SceneView 共用的
    // 视口纹理，与 GRender_RenderGameView 画的目标一致。
    return GRender_GetViewportTexture();
}

int GRender_GetViewportSize(int* out_w, int* out_h) {
    GRYCE_API_GUARD();
    if (!out_w || !out_h) return -1;
    std::lock_guard lock(g_renderer.mutex);
    *out_w = g_renderer.viewport_w;
    *out_h = g_renderer.viewport_h;
    return 0;
}

int GRender_GetGameViewSize(int* out_w, int* out_h) {
    GRYCE_API_GUARD();
    if (!out_w || !out_h) return -1;
    std::lock_guard lock(g_renderer.mutex);
    *out_w = g_renderer.gameview_w;
    *out_h = g_renderer.gameview_h;
    return 0;
}

void GRender_SetVSync(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.vsync_enabled = enabled;
    // init 之前 ctx 为空：此时只记录期望值，GRender_Init 会按它创建交换链，
    // 不能直接 return 丢掉设置（那会让 --no-vsync 之类的调用静默失效）。
    if (!g_renderer.ctx) return;
    g_renderer.ctx->set_swap_interval(enabled ? 1 : 0);
}

bool GRender_IsOnBattery() {
    GRYCE_API_GUARD();
    if (g_renderer.pipeline) return g_renderer.pipeline->query_on_battery();
    return false;
}

void GRender_SetPowerSaverAuto(bool enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_power_saver_auto(enabled);
}

void GRender_SetPowerSaverForced(bool forced) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (g_renderer.pipeline) g_renderer.pipeline->set_power_saver_forced(forced);
}

bool GRender_IsPowerSaverActive() {
    GRYCE_API_GUARD();
    if (g_renderer.pipeline) return g_renderer.pipeline->power_saver_active();
    return false;
}

void GRender_SetGPUProfiling(int enabled) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (!g_renderer.ctx) return;
    g_renderer.ctx->set_gpu_profiling(enabled != 0);
}

int GRender_GPUProfilingSupported(void) {
    GRYCE_API_GUARD();
    if (!g_renderer.ctx) return 0;
    return g_renderer.ctx->gpu_profiling_supported() ? 1 : 0;
}

void GRender_DumpGPUProfiling(void) {
    GRYCE_API_GUARD();
    IRenderBackend* backend = nullptr;
    {
        std::lock_guard lock(g_renderer.mutex);
        if (!g_renderer.ctx) return;
        backend = g_renderer.ctx->gpu_profiling_backend();
    }
    // 只读取累计统计（内部有锁），不触碰 GPU 资源，可在任意线程调用。
    if (backend) backend->gpu_profile_dump();
}

int GRender_SaveScreenshot(const char* path) {
    GRYCE_API_GUARD();
    if (!path || !g_renderer.ctx) return -1;
    std::lock_guard lock(g_renderer.mutex);
    // async 模式：请求渲染线程下一帧截图并写盘；sync 模式同请求（后端在
    // end_frame 的同步截图分支处理）。
    g_renderer.ctx->request_screenshot(path);
    return 0;
}

// ========== Viewport API ==========

void GViewport_SetSize(int w, int h) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.viewport_w = w > 0 ? w : 1;
    g_renderer.viewport_h = h > 0 ? h : 1;
    if (g_renderer.pipeline && g_renderer.pipeline->is_valid()) {
        // 目标重建是 GPU 资源操作，必须在渲染线程（持有 GL context）执行；
        // 否则 GL 上 glCreateFramebuffers 返回 0，目标静默创建失败。
        RenderPipeline* pipeline = g_renderer.pipeline.get();
        const int w = g_renderer.viewport_w;
        const int h = g_renderer.viewport_h;
        g_renderer.ctx->run_on_render_thread([pipeline, w, h]() { pipeline->resize_render_targets(w, h); });
    }
}

void GViewport_GetSize(int* out_w, int* out_h) {
    GRYCE_API_GUARD();
    if (out_w) *out_w = g_renderer.viewport_w;
    if (out_h) *out_h = g_renderer.viewport_h;
}

void GViewport_SetCamera(GEntityHandle camera_entity) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.viewport_camera = camera_entity;
}

GEntityHandle GViewport_GetCamera(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    return g_renderer.viewport_camera;
}

void GViewport_SetEditorCamera(float pos_x, float pos_y, float pos_z,
                                float yaw, float pitch,
                                float fov, float near_plane, float far_plane) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.editor_camera.set_position(math::Vector3f(pos_x, pos_y, pos_z));
    g_renderer.editor_camera.set_yaw(yaw);
    g_renderer.editor_camera.set_pitch(pitch);
    g_renderer.editor_camera.set_fov(fov);
    g_renderer.editor_camera.set_near_far(near_plane, far_plane);
    g_renderer.editor_camera_valid = true;
}

void GGameView_SetSize(int w, int h) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.gameview_w = w > 0 ? w : 1;
    g_renderer.gameview_h = h > 0 ? h : 1;
}

void GGameView_GetSize(int* out_w, int* out_h) {
    GRYCE_API_GUARD();
    if (out_w) *out_w = g_renderer.gameview_w;
    if (out_h) *out_h = g_renderer.gameview_h;
}

void GGameView_SetCamera(GEntityHandle camera_entity) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    g_renderer.gameview_camera = camera_entity;
}

void GRender_RequestBackend(GRenderAPI api) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (api != GRYCE_RENDER_API_OPENGL && api != GRYCE_RENDER_API_VULKAN &&
        api != GRYCE_RENDER_API_WEBGL) return;
    if (api == g_renderer.current_backend && !g_renderer.backend_pending) return;
    g_renderer.pending_backend_api = api;
    g_renderer.backend_pending = true;
    GLOG_INFO("GRender_RequestBackend: {} requested",
              api == GRYCE_RENDER_API_VULKAN ? "vulkan"
              : api == GRYCE_RENDER_API_WEBGL ? "webgl"
                                              : "opengl");
}

void GRender_RequestSurfaceRecreate(void) {
    GRYCE_API_GUARD();
    std::lock_guard lock(g_renderer.mutex);
    if (!g_renderer.initialized) return;
    g_renderer.pending_backend_api = g_renderer.current_backend;
    g_renderer.backend_pending = true;
    GLOG_INFO("GRender_RequestSurfaceRecreate: surface recreate requested");
}

} // extern "C"
