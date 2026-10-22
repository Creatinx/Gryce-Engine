// ui_smoke.cpp — Gryce UI 系统演示 / 冒烟程序
//
// 用法:
//   GryceUiSmoke.exe [--api opengl|vulkan|dx12] [--project <dir>] [--frames N]
//                    [--shot <png>] [--w N] [--h N] [--ui-interval N]
//                    [--theme dark|light] [--no-vsync] [--help]
//
// 与 GryceRenderSmoke 的分工：那个演示渲染管线，这个演示引擎的 UI 子系统
// （engine/ui 模型层 + api/ui_api.h 的 C 面）。跑起来能看到：
//   1. 跨帧控件状态：滑块 / 复选框 / 输入框 / 颜色全部由引擎按稳定 id 保管，
//      面板代码里没有任何本地缓冲——拖到一半切走再回来，值还在；
//   2. 确定性布局：面板矩形由布局栈算，不依赖 ImGui 自动排版与 imgui.ini；
//   3. 主题：token 表驱动，Dark / Light 一键切换；
//   4. 操作队列：UI 想改引擎状态只入队，宿主在锁外按策略受控排出；
//   5. 快捷键门控：UI 吃着键盘时不应派发快捷键，画面上有实时读数。
//
// 截图取第 N 帧（--ui-interval N 时若第 N 帧恰好是缓存重绘帧，还能顺带验证
// "节流帧与构建帧逐字节一致"）。
//
// 退出码: 0 = 正常；1 = 初始化失败；2 = 截图未生成。
#include "api/render_api.h"
#include "api/core_api.h"
#include "api/ui_api.h"
#include "platform/window.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using gryce_engine::platform::Window;
using gryce_engine::platform::WindowMode;
using gryce_engine::platform::WindowContextType;

namespace {

// ---------------------------------------------------------------------------
// 操作码（队列只搬运不认识语义的 type/key/arg/vec，语义归调用方）
// ---------------------------------------------------------------------------
enum {
    OP_THEME     = 1,   // arg = GUI_THEME_PRESET_*（离散）
    OP_SPAWN     = 2,   // key = 0，离散：点一次生成一个
    OP_SCALE     = 3,   // key = 1，连续编辑：拖动期间每帧合并成一条
};

// 快捷键命令号（注册期报冲突，不等用户按下去才发现两个功能抢一个键）
enum {
    CMD_TOGGLE_PANEL = 1,
    CMD_RESET_VIEW   = 2,
};

// 宿主键码（GLFW_KEY_*）：本层只做精确匹配，不认识语义
enum {
    KEY_1 = 49,
    KEY_2 = 50,
};

struct ImageStats {
    bool ok = false;
    int width = 0;
    int height = 0;
    double mean[3] = {0.0, 0.0, 0.0};
    double non_dominant_ratio = 0.0;
    int unique_bins = 0;
};

ImageStats analyze_image(const std::string& path) {
    ImageStats st;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 3);
    if (!pixels) return st;
    st.ok = true;
    st.width = w;
    st.height = h;

    const long long total = static_cast<long long>(w) * h;
    long long sum[3] = {0, 0, 0};
    std::vector<long long> histogram(32 * 32 * 32, 0);
    for (long long i = 0; i < total; ++i) {
        const unsigned char* px = pixels + i * 3;
        sum[0] += px[0];
        sum[1] += px[1];
        sum[2] += px[2];
        const unsigned int bin =
            (static_cast<unsigned int>(px[0] >> 3) << 10) |
            (static_cast<unsigned int>(px[1] >> 3) << 5) |
            static_cast<unsigned int>(px[2] >> 3);
        ++histogram[bin];
    }
    stbi_image_free(pixels);

    for (int c = 0; c < 3; ++c) st.mean[c] = static_cast<double>(sum[c]) / total;
    long long dominant = 0;
    int unique = 0;
    for (long long n : histogram) {
        if (n == 0) continue;
        ++unique;
        if (n > dominant) dominant = n;
    }
    st.unique_bins = unique;
    st.non_dominant_ratio = 1.0 - static_cast<double>(dominant) / total;
    return st;
}

// ---------------------------------------------------------------------------
// UI 回调：只入队 / 只画，不调任何会加锁的引擎 API
// ---------------------------------------------------------------------------

// 面板私有状态只有"选择哪一页"这种**不属于控件值**的东西；控件值全部走
// GUi_State*（引擎保管），所以这里没有一个 DragFloat 的本地缓冲。
struct UiDemoState {
    int  page = 0;          // 0 = 控件  1 = 队列  2 = 关于
    int  spawn_count = 0;   // 累计排出的离散操作数（排出后才计数）
    int  theme_preset = GUI_THEME_PRESET_DARK;
};

// push_op 的失败路径要能翻页提示用户，这里持一份 state 指针；正常路径
// 只用回调的 user 参数。
void* g_ui_demo_user = nullptr;

// 推一条操作。队列满且无可逐出时返回 0，此时把失败显示出来而不是静默吞掉。
void push_op(uint32_t type, uint32_t key, int64_t arg = 0, const float* vec = nullptr) {
    GUiOp op{};
    op.type = type;
    op.key = key;
    op.arg = arg;
    if (vec) {
        for (int i = 0; i < 4; ++i) op.vec[i] = vec[i];
    }
    if (!GUi_PushOp(&op)) {
        // 回调内画不了弹窗，翻到队列页让用户看见 pending 爆满
        UiDemoState* st = static_cast<UiDemoState*>(g_ui_demo_user);
        if (st) st->page = 1;
    }
}

void draw_controls_page() {
    GUi_BeginChild("controls", 0.0f, 0.0f, 1);

    GUi_TextColored(GUI_TEXT_ACCENT, "Cross-frame widget state (engine-owned, stable id)");
    GUi_Separator();

    // 滑块：本地只拿一个"本帧显示值"，写回交给状态存储。拖到一半切页再
    // 回来值还在——缓冲不属于任何栈帧，这正是与"拖一下弹回去"的区别。
    const uint64_t id_opacity = GUi_MakeId("Opacity", 1);
    float opacity = 0.65f;
    GUi_StateGetFloat(id_opacity, &opacity);
    if (GUi_SliderFloat("Opacity", &opacity, 0.0f, 1.0f, "%.2f")) {
        GUi_StateSetFloat(id_opacity, opacity);
    }

    const uint64_t id_shadows = GUi_MakeId("Shadows", 2);
    int shadows = 1;
    GUi_StateGetBool(id_shadows, &shadows);
    if (GUi_Checkbox("Shadows", &shadows)) {
        GUi_StateSetBool(id_shadows, shadows);
    }

    const uint64_t id_name = GUi_MakeId("Name", 3);
    char name[64] = {};
    if (GUi_StateGetText(id_name, name, (int)sizeof(name)) == 0) {
        std::snprintf(name, sizeof(name), "Cube");
    }
    if (GUi_InputText("Name", name, (int)sizeof(name))) {
        GUi_StateSetText(id_name, name);
    }

    const uint64_t id_color = GUi_MakeId("Tint", 4);
    float tint[4] = {0.8f, 0.6f, 0.3f, 1.0f};
    GUi_StateGetVec4(id_color, tint);
    if (GUi_ColorEdit4("Tint", tint)) {
        GUi_StateSetVec4(id_color, tint);
    }

    GUi_Spacing();
    GUi_TextColored(GUI_TEXT_DIM, "None of the four widgets above has a local buffer:");
    GUi_TextColored(GUI_TEXT_DIM, "their values live in the engine state store,");
    GUi_TextColored(GUI_TEXT_DIM, "keyed by (label, seed). Switch pages and come back.");

    GUi_EndChild();
}

void draw_queue_page(void* user) {
    UiDemoState* st = static_cast<UiDemoState*>(user);
    GUi_BeginChild("queue", 0.0f, 0.0f, 1);

    GUi_TextColored(GUI_TEXT_ACCENT, "Op queue (push in callback, drain outside the lock)");
    GUi_Separator();

    if (GUi_Button("Spawn (discrete, key=0 never merges)")) {
        push_op(OP_SPAWN, 0, 1);
    }
    GUi_SameLine(0.0f);
    if (GUi_Button("Theme: Dark")) {
        push_op(OP_THEME, 0, GUI_THEME_PRESET_DARK);
    }
    GUi_SameLine(0.0f);
    if (GUi_Button("Theme: Light")) {
        push_op(OP_THEME, 0, GUI_THEME_PRESET_LIGHT);
    }

    // 连续编辑：key = 1，同键排队期间天然合并，一次拖拽只留最后一条
    const uint64_t id_scale = GUi_MakeId("Scale", 5);
    float scale = 1.0f;
    GUi_StateGetFloat(id_scale, &scale);
    if (GUi_SliderFloat("Scale (continuous, key=1 merges)", &scale, 0.1f, 4.0f, "%.2f")) {
        GUi_StateSetFloat(id_scale, scale);
        push_op(OP_SCALE, 1, 0, &scale);
    }

    GUi_Spacing();
    GUi_TextColored(GUI_TEXT_DIM, "Queue stats (read in callback, lock-free pure data):");
    int pending = 0;
    uint64_t pushed = 0, coalesced = 0, drained = 0, evicted = 0, rejected = 0, throttled = 0;
    GUi_GetOpQueueStats(&pending, &pushed, &coalesced, &drained, &evicted, &rejected, &throttled);
    char line[160];
    std::snprintf(line, sizeof(line), "pending=%d pushed=%llu coalesced=%llu",
                  pending, (unsigned long long)pushed, (unsigned long long)coalesced);
    GUi_LabelText("pending", line);
    std::snprintf(line, sizeof(line), "drained=%llu evicted=%llu rejected=%llu throttled=%llu",
                  (unsigned long long)drained, (unsigned long long)evicted,
                  (unsigned long long)rejected, (unsigned long long)throttled);
    GUi_LabelText("drained", line);
    std::snprintf(line, sizeof(line), "discrete ops applied: %d", st->spawn_count);
    GUi_LabelText("applied", line);
    if (rejected > 0) {
        GUi_TextColored(GUI_TEXT_WARN, "rejected > 0: consumer is not draining; the push failed loudly");
    }

    GUi_EndChild();
}

void draw_about_page(void* user) {
    UiDemoState* st = static_cast<UiDemoState*>(user);
    (void)st;
    GUi_BeginChild("about", 0.0f, 0.0f, 1);
    GUi_TextColored(GUI_TEXT_ACCENT, "Shortcut gating");
    GUi_Separator();
    const int dispatch = GUi_ShouldDispatchShortcut();
    GUi_LabelText("ShouldDispatchShortcut", dispatch ? "1 (dispatch)" : "0 (UI owns the keyboard)");
    GUi_TextColored(GUI_TEXT_DIM, "Ctrl+1 / Ctrl+2 are registered; typing in the input box flips this to 0.");
    GUi_Spacing();
    GUi_TextColored(GUI_TEXT_ACCENT, "Theme");
    GUi_Separator();
    char rgba[96];
    std::snprintf(rgba, sizeof(rgba), "0x%08X", GUi_ThemeColor(GUI_THEME_COLOR_TEXT));
    GUi_LabelText("TEXT token", rgba);
    std::snprintf(rgba, sizeof(rgba), "0x%08X", GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG));
    GUi_LabelText("WINDOW_BG", rgba);
    GUi_EndChild();
}

void ui_callback(void* user) {
    UiDemoState* st = static_cast<UiDemoState*>(user);

    // 布局栈算出的面板矩形（确定性：相同调用序列 -> 相同矩形）
    GRUiRect viewport{};
    float dw = 0.0f, dh = 0.0f;
    GUi_GetDisplaySize(&dw, &dh);
    viewport.x = 0.0f; viewport.y = 0.0f; viewport.w = dw; viewport.h = dh;
    GUi_LayoutReset(&viewport);

    GRUiRect panel{};
    panel.x = 24.0f; panel.y = 24.0f; panel.w = 420.0f; panel.h = dh - 48.0f;
    GUi_LayoutBeginPanel(&panel, 8.0f);
    GRUiRect content{};
    GUi_LayoutContentRect(&content);

    const int flags = GUI_WIN_NO_SAVED_SETTINGS | GUI_WIN_NO_COLLAPSE;
    if (GUi_BeginWindow("UI System Demo", content.x, content.y, content.w, content.h, flags)) {
        if (GUi_BeginMenuBar()) {
            if (GUi_BeginMenu("Pages")) {
                if (GUi_MenuItem("Controls", "Ctrl+1", 1)) st->page = 0;
                if (GUi_MenuItem("Queue", "Ctrl+2", 1)) st->page = 1;
                if (GUi_MenuItem("About", nullptr, 1)) st->page = 2;
                GUi_EndMenu();
            }
            GUi_EndMenuBar();
        }
        switch (st->page) {
            case 0: draw_controls_page(); break;
            case 1: draw_queue_page(user); break;
            default: draw_about_page(user); break;
        }
        GUi_EndWindow();
    }
    GUi_LayoutEndPanel();
}

// ---------------------------------------------------------------------------
// 锁外消费：排出的操作在这里转成真正的引擎动作
// ---------------------------------------------------------------------------

int drain_and_apply(UiDemoState* st) {
    GUiOp ops[16];
    const int n = GUi_DrainOps(ops, 16);
    for (int i = 0; i < n; ++i) {
        switch (ops[i].type) {
            case OP_THEME:
                GUi_ThemeSetPreset(static_cast<int>(ops[i].arg));
                GUi_ThemeApply();
                std::printf("[ui] theme preset -> %d\n", (int)ops[i].arg);
                break;
            case OP_SPAWN:
                ++st->spawn_count;
                std::printf("[ui] spawn #%d\n", st->spawn_count);
                break;
            case OP_SCALE:
                // 连续编辑：到这里最多一条（排队期间已合并）
                std::printf("[ui] scale -> %.3f\n", (double)ops[i].vec[0]);
                break;
            default: break;
        }
    }
    return n;
}

void print_usage() {
    std::printf(
        "GryceUiSmoke — Gryce UI 系统演示程序\n"
        "  --api <opengl|vulkan|dx12>  渲染后端（默认 opengl）\n"
        "  --project <dir>        项目根（默认编译期内置的 demo_project）\n"
        "  --frames <N>           渲染帧数后截图退出（默认 60）\n"
        "  --shot <png>           截图输出路径（默认 ui_smoke_<api>.png）\n"
        "  --w / --h <px>         窗口与视口尺寸（默认 800x600）\n"
        "  --ui-interval <N>      UI 帧节流：每 N 个渲染帧才构建一次 UI（默认 1）\n"
        "  --ops-burst <N>        先批量灌 N 条操作再跑帧循环：演示合并/预算/统计\n"
        "  --theme <dark|light>   初始主题预设（默认 dark）\n"
        "  --no-vsync             关闭垂直同步\n"
        "  --help                 显示本帮助\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string api_arg;
    std::string project_root = GRYCE_DEMO_PROJECT_ROOT;
    std::string shot_path;
    int frames = 60;
    int width = 800, height = 600;
    int width_override = 0, height_override = 0;
    int ui_interval = 1;
    int ops_burst = 0;
    bool vsync = true;
    int theme_preset = GUI_THEME_PRESET_DARK;

    // 带值的参数必须在解析后跳过值本身，否则下一轮循环会把 "60" 当成
    // 未知参数（与 renderer_smoke 同一模式：取值时 ++i）。
    auto value_at = [&](int& i) -> const char* {
        if (i + 1 >= argc) {
            std::fprintf(stderr, "[ui-smoke] %s 缺少参数\n", argv[i]);
            std::exit(1);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--api") == 0) api_arg = value_at(i);
        else if (std::strcmp(a, "--project") == 0) project_root = value_at(i);
        else if (std::strcmp(a, "--frames") == 0) frames = std::atoi(value_at(i));
        else if (std::strcmp(a, "--shot") == 0) shot_path = value_at(i);
        else if (std::strcmp(a, "--w") == 0) width_override = std::atoi(value_at(i));
        else if (std::strcmp(a, "--h") == 0) height_override = std::atoi(value_at(i));
        else if (std::strcmp(a, "--ui-interval") == 0) ui_interval = std::atoi(value_at(i));
        else if (std::strcmp(a, "--ops-burst") == 0) ops_burst = std::atoi(value_at(i));
        else if (std::strcmp(a, "--theme") == 0) {
            const char* t = value_at(i);
            if (std::strcmp(t, "light") == 0) theme_preset = GUI_THEME_PRESET_LIGHT;
            else if (std::strcmp(t, "dark") == 0) theme_preset = GUI_THEME_PRESET_DARK;
            else {
                std::fprintf(stderr, "[ui-smoke] 未知主题 '%s'（dark 或 light）\n", t);
                return 1;
            }
        }
        else if (std::strcmp(a, "--no-vsync") == 0) vsync = false;
        else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            print_usage();
            return 0;
        }
        else {
            std::fprintf(stderr, "[ui-smoke] 未知参数: %s\n", a);
            print_usage();
            return 1;
        }
    }
    if (width_override > 0) width = width_override;
    if (height_override > 0) height = height_override;

    GRenderAPI api = GRYCE_RENDER_API_OPENGL;
    if (api_arg == "vulkan") api = GRYCE_RENDER_API_VULKAN;
    else if (api_arg == "dx12") api = GRYCE_RENDER_API_DX12;
    else if (!api_arg.empty() && api_arg != "opengl") {
        std::fprintf(stderr, "[ui-smoke] 未知后端 '%s'\n", api_arg.c_str());
        return 1;
    }
    const char* backend_name = api == GRYCE_RENDER_API_VULKAN ? "vulkan"
                            : api == GRYCE_RENDER_API_DX12 ? "dx12" : "opengl";
    if (shot_path.empty()) shot_path = std::string("ui_smoke_") + backend_name + ".png";

    if (!Window::init_sdk()) {
        std::fprintf(stderr, "[ui-smoke] failed to init GLFW SDK\n");
        return 1;
    }
    const WindowContextType ctx =
        api == GRYCE_RENDER_API_OPENGL ? WindowContextType::OpenGL : WindowContextType::NoApi;
    Window window("Gryce UI Smoke", width, height, WindowMode::Windowed, ctx);
    if (!window.is_valid()) {
        std::fprintf(stderr, "[ui-smoke] failed to create window\n");
        Window::shutdown_sdk();
        return 1;
    }

    GCoreInitDesc core_desc{};
    core_desc.version = sizeof(GCoreInitDesc);
    core_desc.project_root = project_root.c_str();
    core_desc.enable_reflection = true;
    if (GCore_Init(&core_desc) != 0) {
        std::fprintf(stderr, "[ui-smoke] GCore_Init failed\n");
        Window::shutdown_sdk();
        return 1;
    }

    // 引擎自带调试面板关掉：这个宿主要演示的是"宿主角落"，两层 UI 叠在一起
    // 分不清是谁画的。调试 UI 属于 init 前生效项，必须在 GRender_Init 之前关。
    // 注意关掉它之后引擎仍会因"有 UI 回调"而接管 ImGui。
    GRender_SetDebugUI(false);
    GRender_SetFPSOverlay(false);
    GRender_SetVSync(vsync);
    if (ui_interval > 1) GRender_SetUiFrameInterval(ui_interval);

    UiDemoState state;
    state.theme_preset = theme_preset;
    g_ui_demo_user = &state;

    // 快捷键注册期就报冲突：两个功能抢一个键在这里就发现，不等用户按下去
    if (!GUi_ShortcutAdd(KEY_1, GUI_MOD_CTRL, CMD_TOGGLE_PANEL)) {
        std::printf("[ui-smoke] shortcut Ctrl+1 注册失败（冲突或表满）\n");
    }
    GUi_ShortcutAdd(KEY_2, GUI_MOD_CTRL, CMD_RESET_VIEW);

    // 主题先设好：引擎在 GRender_Init 里按当前 token 落地一次 Style
    // （Style 要在第一个 NewFrame 之前就位），所以这里只设预设、不 apply。
    GUi_ThemeSetPreset(theme_preset);

    // 排出策略：单次最多 16 条、每个推进过的帧都可排。间隔策略由
    // GUi_SetOpPolicy 的第二个参数控制，这里演示"有预算"。
    GUi_SetOpPolicy(16, 0);

    // UI 回调必须在 GRender_Init **之前**注册：引擎只有在"调试 UI 开着"或
    // "有 UI 回调"时才会接管 ImGui（建上下文），init 之后才注册的话 UI 根本
    // 不会被调用，而且不报错。
    GRender_SetUICallback(ui_callback, &state);

    GRenderInitDesc desc{};
    desc.version = sizeof(GRenderInitDesc);
    desc.native_window = window.native_handle();
    desc.api = api;
    desc.viewport_w = width;
    desc.viewport_h = height;
    desc.sync_mode = false;
    if (GRender_Init(&desc) != 0) {
        std::fprintf(stderr, "[ui-smoke] GRender_Init failed\n");
        GCore_Shutdown();
        Window::shutdown_sdk();
        return 1;
    }

    if (!GRender_HasImGui()) {
        std::fprintf(stderr, "[ui-smoke] ImGui 未被接管，UI 不会绘制\n");
        GCore_Shutdown();
        GRender_Shutdown();
        Window::shutdown_sdk();
        return 1;
    }

    std::printf("[ui-smoke] backend=%s frames=%d viewport=%dx%d ui_interval=%d\n",
                backend_name, frames, width, height, ui_interval);

    // --ops-burst：灌一批操作再进帧循环。一半离散（key=0，一条不合并）、
    // 一半连续（key=1，排队期间合并成一条）。跑完后看统计：coalesced 应该
    // 接近连续项条数，drained 受每帧预算限制分批落地。
    if (ops_burst > 0) {
        for (int i = 0; i < ops_burst; ++i) {
            GUiOp op{};
            if (i % 2 == 0) {
                op.type = OP_SPAWN;
                op.key = 0;   // 离散
                op.arg = 1;
            } else {
                op.type = OP_SCALE;
                op.key = 1;   // 同键连续：全部合并成最后一条
                op.vec[0] = 0.1f * static_cast<float>(i);
            }
            GUi_PushOp(&op);
        }
        std::printf("[ui-smoke] ops burst: pushed %d ops\n", ops_burst);
    }

    int rendered = 0;
    int frames_after_shot = -1;
    bool shot_requested = false;
    while (!window.should_close()) {
        window.poll_events();
        const float dt = 1.0f / 60.0f;
        GCore_BeginFrame(dt);
        GRender_BeginFrame();
        GRender_RenderWorld();
        // 锁外消费：回调只是入队，真正的动作在这里做
        drain_and_apply(&state);
        GRender_RenderGizmo();
        GRender_EndFrame();
        GCore_EndFrame();
        ++rendered;

        if (!shot_requested && rendered >= frames) {
            GRender_SaveScreenshot(shot_path.c_str());
            shot_requested = true;
            frames_after_shot = 3;   // 渲染线程下一帧才执行，多驱动几帧
        }
        if (frames_after_shot >= 0 && frames_after_shot-- == 0) break;
    }

    // 退出前把队列统计打出来：coalesced 明显大于 pushed 说明合并真的在发生
    int pending = 0;
    uint64_t pushed = 0, coalesced = 0, drained = 0, evicted = 0, rejected = 0, throttled = 0;
    GUi_GetOpQueueStats(&pending, &pushed, &coalesced, &drained, &evicted, &rejected, &throttled);
    std::printf("[ui-smoke] ops: pending=%d pushed=%llu coalesced=%llu drained=%llu "
                "evicted=%llu rejected=%llu throttled=%llu\n",
                pending, (unsigned long long)pushed, (unsigned long long)coalesced,
                (unsigned long long)drained, (unsigned long long)evicted,
                (unsigned long long)rejected, (unsigned long long)throttled);
    std::printf("[ui-smoke] state entries: %d\n", GUi_StateCount());

    GCore_Shutdown();
    GRender_Shutdown();
    Window::shutdown_sdk();

    if (!shot_requested) return 0;
    if (!std::filesystem::exists(shot_path)) {
        std::fprintf(stderr, "[ui-smoke] 截图未生成（渲染线程未写盘）\n");
        return 2;
    }
    const ImageStats img = analyze_image(shot_path);
    if (!img.ok) {
        std::fprintf(stderr, "[ui-smoke] 截图无法读取: %s\n", shot_path.c_str());
        return 2;
    }
    std::printf("[ui-smoke] image %dx%d mean=(%.1f,%.1f,%.1f) non-dominant=%.2f%% bins=%d\n",
                img.width, img.height, img.mean[0], img.mean[1], img.mean[2],
                img.non_dominant_ratio * 100.0, img.unique_bins);
    // 面板画出来了：画面就不是一片纯色。 bins 少说明 UI 没画上。
    if (img.unique_bins < 8) {
        std::fprintf(stderr, "[ui-smoke] 画面颜色档位过少，UI 可能未绘制\n");
        return 2;
    }
    std::printf("[ui-smoke] done, rendered=%d frames, shot=%s\n", rendered, shot_path.c_str());
    return 0;
}
