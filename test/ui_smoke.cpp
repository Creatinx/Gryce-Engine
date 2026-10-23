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
    CMD_SHOW_LAYOUT  = 3,
};

// 宿主键码（GLFW_KEY_*）：本层只做精确匹配，不认识语义
enum {
    KEY_1 = 49,
    KEY_2 = 50,
    KEY_3 = 51,
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
    int  page = 0;          // 0 = 控件  1 = 队列  2 = 布局  3 = 关于
    int  spawn_count = 0;   // 累计排出的离散操作数（排出后才计数）
    int  theme_preset = GUI_THEME_PRESET_DARK;

    // 布局页（组件树）：树只建一次（视口尺寸变化才重建），之后滑杆只改
    // 树里组件的参数并重解析
    uint64_t lay_col = 0;
    uint64_t lay_grid = 0;
    uint64_t lay_anchor = 0;
    int  lay_built_h = 0;
    bool lay_built = false;
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

// ---------------------------------------------------------------------------
// 布局演示页：组件树（保留模式）
//
// 树在建页时（或视口尺寸变化时）建一次；滑杆只改树里组件的参数，然后重解析。
// 右半屏画解析结果：每个组件一个绝对定位的小窗口，排列规则一眼可见。
// ---------------------------------------------------------------------------

GUiCompSpec lay_spec(uint64_t parent, int kind, float x, float y, float w, float h) {
    GUiCompSpec s{};
    s.parent = parent;
    s.kind = kind;
    s.x = x; s.y = y; s.w = w; s.h = h;
    for (int i = 0; i < 4; ++i) s.padding[i] = 8.0f;
    return s;
}

// 容器组件在自己的第一行画标签，所以 padding 上边要多留一行；
// 叶子没有子节点，标签直接画在内容原点，四边统一 8。
// 另注意：组件窗口一律不小于 32px——imgui 的 WindowMinSize 默认 (32,32)，
// 显式尺寸比它小会被钳制，所以演示几何按这个下限设计。
void lay_mark_container(GUiCompSpec& s) { s.padding[0] = 24.0f; }

void build_layout_tree(UiDemoState* st, float dh) {
    GUi_CompClear();

    // 右半屏根：Column。gap 由页内滑杆改；flex 1 的子（网格）吸收剩余高度，
    // 所以拖 gap 滑杆时树不会溢出根窗口。
    GUiCompSpec root = lay_spec(0, GUI_COMP_KIND_COLUMN, 460.0f, 24.0f, 320.0f, dh - 120.0f);
    root.gap = 6.0f;
    lay_mark_container(root);
    st->lay_col = GUi_CompAdd(&root);

    // 标题行：两个叶子按 flex 1:2 分宽度，交叉轴（高）吃满
    GUiCompSpec head = lay_spec(st->lay_col, GUI_COMP_KIND_ROW, 0, 0, 0, 64.0f);
    head.align = GUI_COMP_ALIGN_STRETCH;
    head.gap = 6.0f;
    lay_mark_container(head);
    const uint64_t head_id = GUi_CompAdd(&head);
    GUiCompSpec h1 = lay_spec(head_id, GUI_COMP_KIND_LEAF, 0, 0, 0, 0);
    h1.flex = 1.0f;
    h1.align = GUI_COMP_ALIGN_STRETCH;
    GUi_CompAdd(&h1);
    GUiCompSpec h2 = lay_spec(head_id, GUI_COMP_KIND_LEAF, 0, 0, 0, 0);
    h2.flex = 2.0f;
    h2.align = GUI_COMP_ALIGN_STRETCH;
    GUi_CompAdd(&h2);

    // 中间行：交叉轴四种对齐各一个（行高固定，最后一个 stretch 吃剩余宽）
    GUiCompSpec mid = lay_spec(st->lay_col, GUI_COMP_KIND_ROW, 0, 0, 0, 140.0f);
    mid.align = GUI_COMP_ALIGN_STRETCH;
    mid.gap = 6.0f;
    lay_mark_container(mid);
    const uint64_t mid_id = GUi_CompAdd(&mid);
    const int aligns[4] = {GUI_COMP_ALIGN_START, GUI_COMP_ALIGN_CENTER,
                           GUI_COMP_ALIGN_END, GUI_COMP_ALIGN_STRETCH};
    for (int i = 0; i < 4; ++i) {
        GUiCompSpec cell = lay_spec(mid_id, GUI_COMP_KIND_LEAF, 0, 0, 64.0f, 36.0f);
        cell.align = aligns[i];
        if (aligns[i] == GUI_COMP_ALIGN_STRETCH) cell.flex = 1.0f;
        GUi_CompAdd(&cell);
    }

    // 网格：6 格，flex 吸收根里剩余高度；列数由页内滑杆改
    GUiCompSpec grid = lay_spec(st->lay_col, GUI_COMP_KIND_GRID, 0, 0, 0, 194.0f);
    grid.align = GUI_COMP_ALIGN_STRETCH;
    grid.cols = 3;
    grid.gap = 4.0f;
    grid.flex = 1.0f;
    lay_mark_container(grid);
    st->lay_grid = GUi_CompAdd(&grid);
    for (int i = 0; i < 6; ++i) {
        GUiCompSpec cell = lay_spec(st->lay_grid, GUI_COMP_KIND_LEAF, 0, 0, 0, 44.0f);
        GUi_CompAdd(&cell);
    }

    // 脚注行
    GUiCompSpec foot = lay_spec(st->lay_col, GUI_COMP_KIND_LEAF, 0, 0, 0, 32.0f);
    foot.align = GUI_COMP_ALIGN_STRETCH;
    GUi_CompAdd(&foot);

    // 对照组：无父级的叶子（直接写 pos），加上挂在叶子下的两个孩子——
    // 父级不是布局组件，它们也直接写 pos，布局不碰。
    GUiCompSpec anchor = lay_spec(0, GUI_COMP_KIND_LEAF, 460.0f, dh - 88.0f, 320.0f, 64.0f);
    st->lay_anchor = GUi_CompAdd(&anchor);
    GUiCompSpec kid_a = lay_spec(st->lay_anchor, GUI_COMP_KIND_LEAF,
                                 470.0f, dh - 60.0f, 140.0f, 32.0f);
    GUi_CompAdd(&kid_a);
    GUiCompSpec kid_b = lay_spec(st->lay_anchor, GUI_COMP_KIND_LEAF,
                                 622.0f, dh - 56.0f, 140.0f, 32.0f);
    GUi_CompAdd(&kid_b);

    st->lay_built = true;
    st->lay_built_h = static_cast<int>(dh);
}

// 递归画一个组件：自己一个绝对定位窗口，子节点在父窗口闭合之后画（互不
// 嵌套、各自独立；后画的在上层）。
//
// 层级序 = 创建序：子窗口比父窗口创建得晚，于是画在父窗口之上，父窗口
// 不透明的背景不会盖住子窗口。**不要**给组件窗口传 GUI_WIN_NO_BRING_TO_FRONT：
// 那个标志在 imgui 里会让新窗口创建时进显示序队首（画在最底层），结果父
// 窗口反而盖住全部子窗口（症状：整棵子树只有一个根窗口可见）。
void draw_component_window(uint64_t id) {
    GRUiRect r{};
    if (GUi_CompRect(id, &r) != 1) return;
    const int kind = GUi_CompKind(id);
    char name[32];
    std::snprintf(name, sizeof(name), "##c%llu", (unsigned long long)id);
    const int flags = GUI_WIN_NO_TITLE_BAR | GUI_WIN_NO_RESIZE | GUI_WIN_NO_MOVE
                    | GUI_WIN_NO_SAVED_SETTINGS | GUI_WIN_NO_SCROLLBAR
                    | GUI_WIN_NO_COLLAPSE;
    const int began = GUi_BeginWindow(name, r.x, r.y, r.w, r.h, flags);
    if (began) {
        char line[48];
        const char* k = kind == GUI_COMP_KIND_COLUMN ? "Column"
                     : kind == GUI_COMP_KIND_ROW ? "Row"
                     : kind == GUI_COMP_KIND_GRID ? "Grid" : "Leaf";
        std::snprintf(line, sizeof(line), "%s %.0fx%.0f", k, r.w, r.h);
        GUi_Text(line);
    }
    GUi_EndWindow();

    const int n = GUi_CompChildCount(id);
    for (int i = 0; i < n; ++i) {
        draw_component_window(GUi_CompChildAt(id, i));
    }
}

// 主窗口里的控件与说明；树的参数在这里改（Get -> 改 -> Set）。
void draw_layout_page(void* user) {
    UiDemoState* st = static_cast<UiDemoState*>(user);
    float dw = 0.0f, dh = 0.0f;
    GUi_GetDisplaySize(&dw, &dh);
    if (!st->lay_built || st->lay_built_h != static_cast<int>(dh)) {
        build_layout_tree(st, dh);
    }

    GUi_BeginChild("layout", 0.0f, 0.0f, 1);
    GUi_TextColored(GUI_TEXT_ACCENT, "Component tree (retained mode)");
    GUi_Separator();
    GUi_TextColored(GUI_TEXT_DIM, "Parent is a layout component -> children are arranged.");
    GUi_TextColored(GUI_TEXT_DIM, "Parent is NOT a layout component -> children keep their own pos.");
    GUi_TextColored(GUI_TEXT_DIM, "Child windows draw above their parent (creation order = z-order).");
    GUi_Spacing();

    const uint64_t id_gap = GUi_MakeId("layout.gap", 0);
    float gap = 6.0f;
    GUi_StateGetFloat(id_gap, &gap);
    if (GUi_SliderFloat("Root gap", &gap, 0.0f, 12.0f, "%.0f")) {
        GUi_StateSetFloat(id_gap, gap);
        GUiCompSpec s{};
        if (GUi_CompGet(st->lay_col, &s) == 1) {
            s.gap = gap;
            GUi_CompSet(st->lay_col, &s);
        }
    }

    const uint64_t id_cols = GUi_MakeId("layout.cols", 0);
    float cols_f = 3.0f;
    GUi_StateGetFloat(id_cols, &cols_f);
    if (GUi_SliderFloat("Grid cols", &cols_f, 2.0f, 6.0f, "%.0f")) {
        GUi_StateSetFloat(id_cols, cols_f);
        GUiCompSpec s{};
        if (GUi_CompGet(st->lay_grid, &s) == 1) {
            s.cols = static_cast<int>(cols_f);
            GUi_CompSet(st->lay_grid, &s);
        }
    }

    GUi_Spacing();
    char line[96];
    std::snprintf(line, sizeof(line), "components: %d (built once; sliders only mutate)",
                  GUi_CompCount());
    GUi_Text(line);
    GUi_EndChild();
}

// 解析并按矩形画树。主窗口闭合之后调用：每个组件是独立的绝对定位窗口。
void draw_layout_tree(void* user) {
    UiDemoState* st = static_cast<UiDemoState*>(user);
    GUi_CompResolve();
    draw_component_window(st->lay_col);
    draw_component_window(st->lay_anchor);   // 它的两个孩子由递归带出
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
    // Begin/End 必须配对：Begin 返回 0（折叠/隐藏）时也要调 End，否则
    // imgui 的窗口栈失衡，后面所有窗口都会错位。
    if (GUi_BeginWindow("UI System Demo", content.x, content.y, content.w, content.h, flags)) {
        if (GUi_BeginMenuBar()) {
            if (GUi_BeginMenu("Pages")) {
                if (GUi_MenuItem("Controls", "Ctrl+1", 1)) st->page = 0;
                if (GUi_MenuItem("Queue", "Ctrl+2", 1)) st->page = 1;
                if (GUi_MenuItem("Layout", "Ctrl+3", 1)) st->page = 2;
                if (GUi_MenuItem("About", nullptr, 1)) st->page = 3;
                GUi_EndMenu();
            }
            GUi_EndMenuBar();
        }
        switch (st->page) {
            case 0: draw_controls_page(); break;
            case 1: draw_queue_page(user); break;
            case 2: draw_layout_page(user); break;
            default: draw_about_page(user); break;
        }
    }
    GUi_EndWindow();
    GUi_LayoutEndPanel();

    // 布局页的组件树：绝对坐标窗口，画在主窗口之后（互不嵌套）
    if (st->page == 2) draw_layout_tree(user);
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
        "  --page <name>          初始页：controls / queue / layout / about（默认 controls）\n"
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
    int page = 0;

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
        else if (std::strcmp(a, "--page") == 0) {
            const char* t = value_at(i);
            if (std::strcmp(t, "controls") == 0) page = 0;
            else if (std::strcmp(t, "queue") == 0) page = 1;
            else if (std::strcmp(t, "layout") == 0) page = 2;
            else if (std::strcmp(t, "about") == 0) page = 3;
            else {
                std::fprintf(stderr, "[ui-smoke] 未知页 '%s'（controls / queue / layout / about）\n", t);
                return 1;
            }
        }
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
    state.page = page;
    g_ui_demo_user = &state;

    // 快捷键注册期就报冲突：两个功能抢一个键在这里就发现，不等用户按下去
    if (!GUi_ShortcutAdd(KEY_1, GUI_MOD_CTRL, CMD_TOGGLE_PANEL)) {
        std::printf("[ui-smoke] shortcut Ctrl+1 注册失败（冲突或表满）\n");
    }
    GUi_ShortcutAdd(KEY_2, GUI_MOD_CTRL, CMD_RESET_VIEW);
    GUi_ShortcutAdd(KEY_3, GUI_MOD_CTRL, CMD_SHOW_LAYOUT);

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

    std::printf("[ui-smoke] backend=%s frames=%d viewport=%dx%d ui_interval=%d page=%d\n",
                backend_name, frames, width, height, ui_interval, page);

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
