// ui_smoke.cpp — Gryce UI 系统演示 / 冒烟程序
//
// 用法:
//   GryceUiSmoke.exe [--api opengl|vulkan|dx12] [--project <dir>] [--frames N]
//                    [--shot <png>] [--w N] [--h N] [--ui-interval N] [--ui-scale f]
//                    [--theme dark|light|midnight|nord|highcontrast]
//                    [--page controls|queue|layout|anim|struct|about] [--no-vsync]
//                    [--ui-backend imgui|builtin] [--help]
//
// 与 GryceRenderSmoke 的分工：那个演示渲染管线，这个演示引擎的 UI 子系统
// （engine/ui 模型层 + api/ui_api.h 的 C 面）。跑起来能看到：
//   1. 跨帧控件状态：滑块 / 复选框 / 输入框 / 颜色全部由引擎按稳定 id 保管，
//      面板代码里没有任何本地缓冲——拖到一半切走再回来，值还在；
//   2. 确定性布局：面板矩形由布局栈算，不依赖 ImGui 自动排版与 imgui.ini；
//   3. 主题：token 表驱动，5 套预设（Dark / Light / Midnight / Nord /
//      HighContrast）一键切换，圆角/滚动条等美化度量随预设落地；
//   4. 操作队列：UI 想改引擎状态只入队，宿主在锁外按策略受控排出；
//   5. 快捷键门控：UI 吃着键盘时不应派发快捷键，画面上有实时读数；
//   6. 动画系统两块：块一纯函数（补间/缓动/颜色插值，改属性直接用）在
//      Anim 页面板里演示；块二运行时（片段/轨道/通道/播放器）驱动右侧
//      舞台上的四个组件（滑移/脉冲/弹跳/错峰入场）。
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
#include <cmath>
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
    CMD_SHOW_ANIM    = 4,
};

// 宿主键码（GLFW_KEY_*）：本层只做精确匹配，不认识语义
enum {
    KEY_1 = 49,
    KEY_2 = 50,
    KEY_3 = 51,
    KEY_4 = 52,
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
    int  page = 0;          // 0 = 控件  1 = 队列  2 = 布局  3 = 动画  4 = 关于
    int  last_page = -1;    // 上一帧的页：翻页时重建各自的树/舞台
    int  spawn_count = 0;   // 累计排出的离散操作数（排出后才计数）
    int  theme_preset = GUI_THEME_PRESET_DARK;

    // 布局页（组件树）：树只建一次（视口尺寸变化或翻页回来才重建），之后
    // 滑杆只改树里组件的参数并重解析
    uint64_t lay_col = 0;
    uint64_t lay_grid = 0;
    uint64_t lay_anchor = 0;
    int  lay_built_h = 0;
    bool lay_built = false;

    // 动画页：块一的演示时钟（宿主自持，补间是纯函数、时间归调用方），
    // 块二的舞台组件（父级全是 0 = 无父级，动画直写 pos，布局不碰）
    float demo_t = 0.0f;
    int   anim_loop = GUI_ANIM_LOOP_LOOP;
    int   anim_ease = GUI_ANIM_EASE_IN_OUT_CUBIC;
    float anim_speed = 1.0f;
    float anim_seek = 0.0f;
    uint64_t anim_slide = 0;
    uint64_t anim_pulse = 0;
    uint64_t anim_hop = 0;
    uint64_t anim_stag[4] = {0, 0, 0, 0};
    int  anim_built_h = 0;
    bool anim_built = false;
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

// 预设名表：启动时用 GUi_ThemePresetName 填一次，之后只读。
const char* const* theme_name_table() {
    static char names[GUI_THEME_PRESET_COUNT][24];
    static const char* ptrs[GUI_THEME_PRESET_COUNT];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < GUI_THEME_PRESET_COUNT; ++i) {
            GUi_ThemePresetName(i, names[i], sizeof(names[i]));
            ptrs[i] = names[i];
        }
        init = true;
    }
    return ptrs;
}

void draw_controls_page(void* user) {
    UiDemoState* st = static_cast<UiDemoState*>(user);
    GUi_BeginChild("controls", 0.0f, 0.0f, 1);

    // 主题预设：换预设走操作队列（离散操作，key=0），排出时才真正落地——
    // 与"UI 只入队、宿主锁外执行"的规矩一致，不因为在回调里就特批。
    int preset = st->theme_preset;
    if (GUi_Combo("Theme preset", &preset, theme_name_table(), GUI_THEME_PRESET_COUNT)) {
        st->theme_preset = preset;
        push_op(OP_THEME, 0, preset);
    }
    GUi_TextColored(GUI_TEXT_DIM, "5 presets; colors come from one token table.");
    GUi_Separator();

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

// 结构与弹层页：折叠头 / 树 / 可滚子区域 / 弹层 / 模态 / 提示
void draw_struct_page(void* user) {
    UiDemoState* st = static_cast<UiDemoState*>(user);
    GUi_BeginChild("struct", 0.0f, 0.0f, 1);

    GUi_TextColored(GUI_TEXT_ACCENT, "Structure (tree / header / child scroll)");
    GUi_Separator();

    if (GUi_CollapsingHeader("Collapsing header", 1)) {
        GUi_TextColored(GUI_TEXT_DIM, "展开时子项才提交；折叠态跨帧保留（控件 id 稳定）。");
    }

    if (GUi_TreeNode("Scene", 1)) {
        for (int i = 0; i < 3; ++i) {
            char lbl[32];
            std::snprintf(lbl, sizeof(lbl), "Mesh %d", i);
            if (GUi_TreeNode(lbl, 0)) {
                GUi_Indent(12.0f);
                GUi_LabelText("nested", "one level deeper");
                GUi_Unindent(12.0f);
                GUi_TreePop();
            }
        }
        GUi_TreePop();
    }

    GUi_Spacing();
    GUi_TextColored(GUI_TEXT_ACCENT, "Child region: own clip + own scroll");
    if (GUi_BeginChild("log", 0.0f, 110.0f, 1)) {
        for (int i = 0; i < 30; ++i) {
            char row[32];
            std::snprintf(row, sizeof(row), "row %02d", i);
            GUi_LabelText("row", row);
        }
        char scroll[64];
        std::snprintf(scroll, sizeof(scroll), "scroll_y = %.1f", (double)GUi_GetWindowScrollY());
        GUi_LabelText("scroll", scroll);
        GUi_EndChild();
    }

    GUi_Spacing();
    GUi_TextColored(GUI_TEXT_ACCENT, "Popups: dropdown / tooltip / modal");
    if (GUi_Button("Actions")) GUi_OpenPopup("##actions_menu");
    if (GUi_IsItemHovered()) GUi_SetTooltip("click to open a popup");
    if (GUi_BeginPopup("##actions_menu")) {
        if (GUi_MenuItem("Duplicate", "Ctrl+D", 1)) st->spawn_count += 1;
        if (GUi_MenuItem("Delete", "Del", 1)) st->spawn_count -= 1;
        GUi_EndPopup();
    }
    GUi_SameLine(0.0f);
    if (GUi_Button("Modal")) GUi_OpenModal("demo_modal", 280.0f, 110.0f);
    if (GUi_BeginModal("demo_modal", 280.0f, 110.0f)) {
        GUi_Text("Modal: outside clicks are ignored, Esc closes.");
        if (GUi_Button("Close")) GUi_CloseCurrentPopup();
        GUi_EndPopup();
    }
    GUi_SameLine(0.0f);
    GUi_LabelText("actions", "counter follows the menu items");

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
    GUi_TextColored(GUI_TEXT_DIM, "Ctrl+1 .. Ctrl+4 are registered; typing in the input box flips this to 0.");
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
// page_switched：翻页回来必须重建——组件树是进程内唯一的，动画页会把它
// 清空另建，不重建的话布局页拿着的是已失效的 id。
void draw_layout_page(void* user, bool page_switched) {
    UiDemoState* st = static_cast<UiDemoState*>(user);
    float dw = 0.0f, dh = 0.0f;
    GUi_GetDisplaySize(&dw, &dh);
    if (page_switched || !st->lay_built || st->lay_built_h != static_cast<int>(dh)) {
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

// ---------------------------------------------------------------------------
// 动画演示页：块一（纯函数）在面板里，块二（运行时）驱动右侧舞台
//
// 两块的边界在这个页面里是看得见的：
//   * 面板里的补间进度条与颜色色块用 GUi_AnimTween / GUi_AnimColorLerp
//     直接算——没有播放器、没有片段，时间用宿主自己的时钟（demo_t），
//     循环靠 fmod 自己折。这就是"改个属性直接用"的用法；
//   * 右侧舞台的四个组件由 Animator 驱动：片段写具名通道，页面每帧把
//     通道值写进组件 rect。这些组件的父级全是 0（无父级），所以"动画
//     直写 pos"不会被布局覆盖——保留模式的规则反过来被动画利用。
// ---------------------------------------------------------------------------

// 舞台片段：所有轨道合成一段 2 秒的交互动画。
//   slide.x   0 -> 200 -> 0（滑移，InOutCubic）
//   pulse.s   两次脉冲（0->1->0->1->0，InOutQuad）
//   hop.y     两段弹跳（OutQuad 关键帧，落点不等高）
//   stag.0..3 错峰入场（轨道 delay 0/0.15/0.30/0.45，OutBack 回弹）
GUiAnimClip build_stage_clip(int loop_mode) {
    GUiAnimClip clip{};
    std::snprintf(clip.name, sizeof(clip.name), "stage");
    clip.loop = loop_mode;
    clip.duration = 2.0f;   // 显式时长：短于它的轨道在尾部保持终值

    GUiAnimTrack t{};
    std::strncpy(t.channel, "slide.x", sizeof(t.channel) - 1);
    t.key_count = 3;
    t.keys[0].t = 0.0f;  t.keys[0].v = 0.0f;   t.keys[0].ease = GUI_ANIM_EASE_IN_OUT_CUBIC;
    t.keys[1].t = 1.0f;  t.keys[1].v = 200.0f; t.keys[1].ease = GUI_ANIM_EASE_IN_OUT_CUBIC;
    t.keys[2].t = 2.0f;  t.keys[2].v = 0.0f;   t.keys[2].ease = GUI_ANIM_EASE_IN_OUT_CUBIC;
    clip.tracks[clip.track_count++] = t;

    t = GUiAnimTrack{};
    std::strncpy(t.channel, "pulse.s", sizeof(t.channel) - 1);
    t.key_count = 5;
    const float pulse_t[5] = {0.0f, 0.5f, 1.0f, 1.5f, 2.0f};
    const float pulse_v[5] = {0.0f, 1.0f, 0.0f, 1.0f, 0.0f};
    for (int i = 0; i < 5; ++i) {
        t.keys[i].t = pulse_t[i];
        t.keys[i].v = pulse_v[i];
        t.keys[i].ease = GUI_ANIM_EASE_IN_OUT_QUAD;
    }
    clip.tracks[clip.track_count++] = t;

    t = GUiAnimTrack{};
    std::strncpy(t.channel, "hop.y", sizeof(t.channel) - 1);
    t.key_count = 7;
    const float hop_t[7] = {0.0f, 0.30f, 0.50f, 0.80f, 1.00f, 1.50f, 1.70f};
    const float hop_v[7] = {0.0f, 60.0f,  0.0f,  26.0f,  0.0f,  60.0f,  0.0f};
    for (int i = 0; i < 7; ++i) {
        t.keys[i].t = hop_t[i];
        t.keys[i].v = hop_v[i];
        t.keys[i].ease = GUI_ANIM_EASE_OUT_QUAD;
    }
    clip.tracks[clip.track_count++] = t;

    for (int i = 0; i < 4; ++i) {
        t = GUiAnimTrack{};
        char nm[24];
        std::snprintf(nm, sizeof(nm), "stag.%d", i);
        std::strncpy(t.channel, nm, sizeof(t.channel) - 1);
        t.delay = 0.15f * static_cast<float>(i);
        t.key_count = 2;
        t.keys[0].t = 0.0f;  t.keys[0].v = 0.0f; t.keys[0].ease = GUI_ANIM_EASE_OUT_BACK;
        t.keys[1].t = 0.45f; t.keys[1].v = 1.0f; t.keys[1].ease = GUI_ANIM_EASE_OUT_BACK;
        clip.tracks[clip.track_count++] = t;
    }
    return clip;
}

// 舞台组件：全部无父级（parent = 0）。动画每帧直写它们的 rect——父级不是
// 布局组件，这些 pos 会被保留，resolve 不会碰它们。
void build_anim_stage(UiDemoState* st, float dh) {
    GUi_CompClear();
    GUiCompSpec s{};
    s.kind = GUI_COMP_KIND_LEAF;

    s.x = 470.0f; s.y = 70.0f;  s.w = 110.0f; s.h = 40.0f;
    st->anim_slide = GUi_CompAdd(&s);
    s.x = 620.0f; s.y = 130.0f; s.w = 80.0f;  s.h = 80.0f;
    st->anim_pulse = GUi_CompAdd(&s);
    s.x = 480.0f; s.y = 250.0f; s.w = 90.0f;  s.h = 36.0f;
    st->anim_hop = GUi_CompAdd(&s);
    for (int i = 0; i < 4; ++i) {
        s.x = 480.0f + 56.0f * static_cast<float>(i);
        s.y = 330.0f;
        s.w = 40.0f; s.h = 40.0f;
        st->anim_stag[i] = GUi_CompAdd(&s);
    }
    st->anim_built = true;
    st->anim_built_h = static_cast<int>(dh);
}

// 通道值 -> 组件 rect（Get -> 改 -> Set）。不需要 resolve：没有布局父级。
void anim_set_rect(uint64_t id, float x, float y, float w, float h) {
    GUiCompSpec s{};
    if (GUi_CompGet(id, &s) != 1) return;
    s.x = x; s.y = y; s.w = w; s.h = h;
    GUi_CompSet(id, &s);
}

void apply_anim_to_stage(UiDemoState* st) {
    float v = 0.0f;
    if (GUi_AnimChannelFloat("slide.x", &v)) {
        anim_set_rect(st->anim_slide, 470.0f + v, 70.0f, 110.0f, 40.0f);
    }
    if (GUi_AnimChannelFloat("pulse.s", &v)) {
        const float w = 48.0f + 48.0f * v;
        anim_set_rect(st->anim_pulse, 660.0f - w * 0.5f, 170.0f - w * 0.5f, w, w);
    }
    if (GUi_AnimChannelFloat("hop.y", &v)) {
        anim_set_rect(st->anim_hop, 480.0f, 250.0f + v, 90.0f, 36.0f);
    }
    for (int i = 0; i < 4; ++i) {
        char nm[24];
        std::snprintf(nm, sizeof(nm), "stag.%d", i);
        if (GUi_AnimChannelFloat(nm, &v)) {
            anim_set_rect(st->anim_stag[i],
                          480.0f + 56.0f * static_cast<float>(i) + 44.0f * v,
                          330.0f, 40.0f, 40.0f);
        }
    }
}

void draw_stage_box(uint64_t id, const char* label) {
    GRUiRect r{};
    if (GUi_CompRect(id, &r) != 1) return;
    char name[40];
    std::snprintf(name, sizeof(name), "##s%llu", (unsigned long long)id);
    // 与布局页同一条铁律：不要传 GUI_WIN_NO_BRING_TO_FRONT（新窗口会进显示
    // 序队首，先画的盖住后画的）。
    const int flags = GUI_WIN_NO_TITLE_BAR | GUI_WIN_NO_RESIZE | GUI_WIN_NO_MOVE
                    | GUI_WIN_NO_SAVED_SETTINGS | GUI_WIN_NO_SCROLLBAR
                    | GUI_WIN_NO_COLLAPSE;
    const int began = GUi_BeginWindow(name, r.x, r.y, r.w, r.h, flags);
    if (began) {
        char line[48];
        std::snprintf(line, sizeof(line), "%s %.0fx%.0f", label, r.w, r.h);
        GUi_Text(line);
    }
    GUi_EndWindow();
}

void draw_anim_page(void* user, bool page_switched) {
    UiDemoState* st = static_cast<UiDemoState*>(user);
    float dw = 0.0f, dh = 0.0f;
    GUi_GetDisplaySize(&dw, &dh);
    if (page_switched || !st->anim_built || st->anim_built_h != static_cast<int>(dh)) {
        build_anim_stage(st, dh);
        // 翻页进来从头播：动画是"看到就动"的东西，停在上次退出的位置只会
        // 让人以为坏了
        GUiAnimClip first = build_stage_clip(st->anim_loop);
        GUi_AnimPlay(&first, 1);
    }

    GUi_BeginChild("anim", 0.0f, 0.0f, 1);
    GUi_TextColored(GUI_TEXT_ACCENT, "Animation: block 1 (pure) + block 2 (runtime)");
    GUi_Separator();

    // ---- 块一：纯函数，改属性直接用 ----
    static char ease_bufs[GUI_ANIM_EASE_COUNT][24];
    static const char* ease_ptrs[GUI_ANIM_EASE_COUNT];
    static bool ease_init = false;
    if (!ease_init) {
        for (int i = 0; i < GUI_ANIM_EASE_COUNT; ++i) {
            GUi_AnimEaseName(i, ease_bufs[i], sizeof(ease_bufs[i]));
            ease_ptrs[i] = ease_bufs[i];
        }
        ease_init = true;
    }
    GUi_TextColored(GUI_TEXT_DIM, "Block 1 - pure functions (no player, no state):");
    int ease = st->anim_ease;
    if (GUi_Combo("Tween ease", &ease, ease_ptrs, GUI_ANIM_EASE_COUNT)) {
        st->anim_ease = ease;
    }
    // 补间演示：时间用宿主自己的时钟，循环靠 fmod 自己折——纯函数层没有
    // 循环模式，折叠是调用方的事
    const float cycle = 1.2f;
    const float tt = std::fmod(st->demo_t, cycle);
    float v = 0.0f;
    GUi_AnimTween(0.0f, 1.0f, cycle, 0.0f, st->anim_ease, tt, &v);
    GUi_ProgressBar(v, -1.0f, 0.0f, nullptr);
    char line[96];
    std::snprintf(line, sizeof(line), "0->1 over %.1fs @ t=%.2f -> %.3f", cycle, tt, (double)v);
    GUi_LabelText("tween", line);
    const float red[4]   = {0.85f, 0.25f, 0.25f, 1.0f};
    const float blue[4]  = {0.25f, 0.45f, 0.90f, 1.0f};
    float col[4] = {};
    GUi_AnimColorLerp(red, blue, v, col);
    GUi_ColorButton("color lerp", col);
    GUi_Spacing();

    // ---- 块二：运行时 ----
    GUiAnimClip clip = build_stage_clip(st->anim_loop);
    const float dur = GUi_AnimClipDuration(&clip);
    GUi_TextColored(GUI_TEXT_DIM, "Block 2 - clip / tracks / channels / player:");
    if (GUi_Button("Play")) {
        // 播完之后 Play = 从头再来；暂停中 Play = 继续
        if (GUi_AnimIsFinished()) GUi_AnimPlay(&clip, 1);
        else GUi_AnimResume();
    }
    GUi_SameLine(0.0f);
    if (GUi_Button("Pause")) GUi_AnimPause();
    GUi_SameLine(0.0f);
    if (GUi_Button("Stop")) GUi_AnimStop();
    GUi_SameLine(0.0f);
    if (GUi_Button("Restart")) GUi_AnimPlay(&clip, 1);

    static const char* loop_names[3] = {"Once", "Loop", "PingPong"};
    int loop_mode = st->anim_loop;
    if (GUi_Combo("Loop mode", &loop_mode, loop_names, 3)) {
        st->anim_loop = loop_mode;
        // 循环模式住在片段里：换模式不重置进度（restart = 0）
        GUi_AnimPlay(&clip, 0);
    }

    if (GUi_SliderFloat("Speed", &st->anim_speed, 0.25f, 3.0f, "%.2fx")) {
        GUi_AnimSetSpeed(st->anim_speed);
    }
    // Seek 滑杆要有本地缓冲：每帧用 GUi_AnimTime() 覆盖它，拖拽会被自己
    // 弹回去。只有"没有任何控件在编辑"时才同步播放进度。
    if (!GUi_IsAnyItemActive()) st->anim_seek = GUi_AnimTime();
    float seek_t = st->anim_seek;
    if (GUi_SliderFloat("Seek", &seek_t, 0.0f, dur, "%.2fs")) {
        st->anim_seek = seek_t;
        GUi_AnimSeek(seek_t);
    }

    GUi_Spacing();
    GUi_TextColored(GUI_TEXT_DIM, "Channels (clip writes them; stage rects read them):");
    const int nch = GUi_AnimChannelCount();
    for (int i = 0; i < nch; ++i) {
        char nm[24] = {};
        GUi_AnimChannelName(i, nm, sizeof(nm));
        float fv = 0.0f;
        float cv[4] = {};
        if (GUi_AnimChannelFloat(nm, &fv)) {
            std::snprintf(line, sizeof(line), "%.2f", (double)fv);
        } else if (GUi_AnimChannelColor(nm, cv)) {
            std::snprintf(line, sizeof(line), "%.2f %.2f %.2f %.2f",
                          (double)cv[0], (double)cv[1], (double)cv[2], (double)cv[3]);
        } else {
            std::snprintf(line, sizeof(line), "?");
        }
        GUi_LabelText(nm, line);
    }
    std::snprintf(line, sizeof(line), "playing=%d finished=%d t=%.2f/%.2f",
                  GUi_AnimIsPlaying(), GUi_AnimIsFinished(),
                  (double)GUi_AnimTime(), (double)dur);
    GUi_LabelText("player", line);
    GUi_EndChild();
}

// 舞台画在主窗口之后：每个组件一个绝对定位窗口（与布局页同一套画法）
void draw_anim_stage(void* user) {
    UiDemoState* st = static_cast<UiDemoState*>(user);
    apply_anim_to_stage(st);
    draw_stage_box(st->anim_slide, "slide");
    draw_stage_box(st->anim_pulse, "pulse");
    draw_stage_box(st->anim_hop, "hop");
    for (int i = 0; i < 4; ++i) draw_stage_box(st->anim_stag[i], "stag");
}

// 外观页：圆角 / 渐变 / 描边 / 阴影一次看全（颜色全部来自主题 token，
// 换预设整页跟着变；几何来自主题的 14 个度量，与主题无关）。
void draw_style_page(void* user) {
    UiDemoState* st = static_cast<UiDemoState*>(user);
    GUi_BeginChild("style", 0.0f, 0.0f, 1);

    GUi_TextColored(GUI_TEXT_ACCENT, "Rounded fill + gradient + border");
    GUi_Separator();
    if (GUi_Button("Primary")) st->spawn_count += 1;
    GUi_SameLine(-1.0f);
    if (GUi_Button("Secondary")) st->spawn_count += 1;
    GUi_SameLine(-1.0f);
    GUi_SmallButton("Small");

    const uint64_t id_check = GUi_MakeId("Rounded check", 7);
    int check = 1;
    GUi_StateGetBool(id_check, &check);
    if (GUi_Checkbox("Rounded check", &check)) GUi_StateSetBool(id_check, check);

    const uint64_t id_slider = GUi_MakeId("Corner radius", 8);
    float slider = 0.42f;
    GUi_StateGetFloat(id_slider, &slider);
    if (GUi_SliderFloat("Corner radius", &slider, 0.0f, 1.0f, "%.2f")) {
        GUi_StateSetFloat(id_slider, slider);
    }

    const uint64_t id_drag = GUi_MakeId("Padding", 9);
    float drag = 1.25f;
    GUi_StateGetFloat(id_drag, &drag);
    if (GUi_DragFloat("Padding", &drag, 0.01f)) GUi_StateSetFloat(id_drag, drag);

    const uint64_t id_text = GUi_MakeId("Rounded field", 10);
    char text[64] = {};
    if (GUi_StateGetText(id_text, text, (int)sizeof(text)) == 0) {
        std::snprintf(text, sizeof(text), "text inside a rounded frame");
    }
    if (GUi_InputText("Rounded field", text, (int)sizeof(text))) {
        GUi_StateSetText(id_text, text);
    }

    GUi_ProgressBar(0.62f, 0.0f, 0.0f, "62%");

    GUi_Spacing();
    GUi_TextColored(GUI_TEXT_ACCENT, "Child region (own clip + rounded scrollbar)");
    if (GUi_BeginChild("style_child", 0.0f, 92.0f, 1)) {
        for (int i = 0; i < 12; ++i) {
            char row[32];
            std::snprintf(row, sizeof(row), "row %02d", i);
            GUi_LabelText("row", row);
        }
        GUi_EndChild();
    }

    GUi_Spacing();
    GUi_TextColored(GUI_TEXT_ACCENT, "Popup + tooltip (both cast a shadow)");
    if (GUi_Button("Open popup")) GUi_OpenPopup("##style_popup");
    if (GUi_IsItemHovered()) GUi_SetTooltip("rounded corners + shadow");
    if (GUi_BeginPopup("##style_popup")) {
        if (GUi_MenuItem("Duplicate", "Ctrl+D", 1)) st->spawn_count += 1;
        if (GUi_MenuItem("Delete", "Del", 1)) st->spawn_count -= 1;
        GUi_Separator();
        if (GUi_MenuItem("Disabled item", nullptr, 0)) {}
        GUi_EndPopup();
    }

    GUi_EndChild();
}

// 外观页的浮窗：只有"带标题栏的浮窗"投影（平铺面板互相贴着，影子看不见）
void draw_style_float_window(void* user) {
    (void)user;
    const int flags = GUI_WIN_TITLE_BAR | GUI_WIN_NO_SAVED_SETTINGS |
                      GUI_WIN_NO_RESIZE | GUI_WIN_NO_MOVE | GUI_WIN_NO_COLLAPSE;
    if (GUi_BeginWindow("Floating panel", 470.0f, 96.0f, 260.0f, 132.0f, flags)) {
        GUi_TextColored(GUI_TEXT_DIM, "Title bar: top corners follow the window");
        GUi_Separator();
        // 半径直接读主题度量：写死数字的话换了档位这行就会骗人
        char radii[64];
        std::snprintf(radii, sizeof(radii), "w %.0f / f %.0f / g %.0f",
                      GUi_ThemeMetric(GUI_THEME_METRIC_WINDOW_ROUNDING),
                      GUi_ThemeMetric(GUI_THEME_METRIC_FRAME_ROUNDING),
                      GUi_ThemeMetric(GUI_THEME_METRIC_GRAB_ROUNDING));
        GUi_LabelText("radius", radii);
        GUi_ProgressBar(0.35f, 0.0f, 0.0f, "35%");
    }
    GUi_EndWindow();
}

void ui_callback(void* user) {
    UiDemoState* st = static_cast<UiDemoState*>(user);
    // 翻页检测：布局页与动画页共用进程内唯一的组件树，翻回来必须重建
    const bool page_switched = st->page != st->last_page;
    st->last_page = st->page;

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
                if (GUi_MenuItem("Anim", "Ctrl+4", 1)) st->page = 3;
                if (GUi_MenuItem("Structure", nullptr, 1)) st->page = 5;
                if (GUi_MenuItem("Style", nullptr, 1)) st->page = 6;
                if (GUi_MenuItem("About", nullptr, 1)) st->page = 4;
                GUi_EndMenu();
            }
            GUi_EndMenuBar();
        }
        switch (st->page) {
            case 0: draw_controls_page(user); break;
            case 1: draw_queue_page(user); break;
            case 2: draw_layout_page(user, page_switched); break;
            case 3: draw_anim_page(user, page_switched); break;
            case 5: draw_struct_page(user); break;
            case 6: draw_style_page(user); break;
            default: draw_about_page(user); break;
        }
    }
    GUi_EndWindow();
    GUi_LayoutEndPanel();

    // 布局页的组件树 / 动画页的舞台：绝对坐标窗口，画在主窗口之后（互不嵌套）
    if (st->page == 2) draw_layout_tree(user);
    if (st->page == 3) draw_anim_stage(user);
    if (st->page == 6) draw_style_float_window(user);
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
        "  --ui-scale <f>         UI 缩放（HiDPI；模型层仍按逻辑像素算，默认 1）\n"
        "  --ops-burst <N>        先批量灌 N 条操作再跑帧循环：演示合并/预算/统计\n"
        "  --theme <name>         初始主题预设：dark / light / midnight / nord /\n"
        "                        highcontrast（默认 dark）\n"
        "  --page <name>          初始页：controls / queue / layout / anim /\n"
        "                        struct / style / about（默认 controls）\n"
        "  --ui-backend <name>    UI 后端：imgui（默认）/ builtin（引擎内置，\n"
        "                        不建 ImGui 上下文，界面由 2D 覆盖层自己画）\n"
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
    float ui_scale = 1.0f;
    int ops_burst = 0;
    bool vsync = true;
    int theme_preset = GUI_THEME_PRESET_DARK;
    int page = 0;
    int ui_backend = 0;   // 0 = ImGui（默认），1 = 引擎内置

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
        else if (std::strcmp(a, "--ui-scale") == 0) {
            ui_scale = static_cast<float>(std::atof(value_at(i)));
            if (ui_scale <= 0.0f) {
                std::fprintf(stderr, "[ui-smoke] --ui-scale 必须 > 0\n");
                return 1;
            }
        }
        else if (std::strcmp(a, "--ops-burst") == 0) ops_burst = std::atoi(value_at(i));
        else if (std::strcmp(a, "--theme") == 0) {
            const char* t = value_at(i);
            if (std::strcmp(t, "light") == 0) theme_preset = GUI_THEME_PRESET_LIGHT;
            else if (std::strcmp(t, "dark") == 0) theme_preset = GUI_THEME_PRESET_DARK;
            else if (std::strcmp(t, "midnight") == 0) theme_preset = GUI_THEME_PRESET_MIDNIGHT;
            else if (std::strcmp(t, "nord") == 0) theme_preset = GUI_THEME_PRESET_NORD;
            else if (std::strcmp(t, "highcontrast") == 0) theme_preset = GUI_THEME_PRESET_HIGH_CONTRAST;
            else {
                std::fprintf(stderr,
                             "[ui-smoke] 未知主题 '%s'（dark / light / midnight / nord / highcontrast）\n",
                             t);
                return 1;
            }
        }
        else if (std::strcmp(a, "--no-vsync") == 0) vsync = false;
        else if (std::strcmp(a, "--ui-backend") == 0) {
            const char* t = value_at(i);
            if (std::strcmp(t, "imgui") == 0) ui_backend = 0;
            else if (std::strcmp(t, "builtin") == 0) ui_backend = 1;
            else {
                std::fprintf(stderr, "[ui-smoke] 未知 UI 后端 '%s'（imgui / builtin）\n", t);
                return 1;
            }
        }
        else if (std::strcmp(a, "--page") == 0) {
            const char* t = value_at(i);
            if (std::strcmp(t, "controls") == 0) page = 0;
            else if (std::strcmp(t, "queue") == 0) page = 1;
            else if (std::strcmp(t, "layout") == 0) page = 2;
            else if (std::strcmp(t, "anim") == 0) page = 3;
            else if (std::strcmp(t, "struct") == 0 || std::strcmp(t, "structure") == 0) page = 5;
            else if (std::strcmp(t, "style") == 0) page = 6;
            else if (std::strcmp(t, "about") == 0) page = 4;
            else {
                std::fprintf(stderr,
                             "[ui-smoke] 未知页 '%s'（controls / queue / layout / anim / struct / about）\n",
                             t);
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
    // UI 后端也是 init 前生效项（决定建不建 ImGui 上下文）。
    if (ui_backend == 1) GRender_SetUIBackend(1);
    // HiDPI：模型层始终按逻辑像素算，渲染桥与输入按这个倍率换算。
    if (ui_scale != 1.0f) GRender_SetUIScale(ui_scale);

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
    GUi_ShortcutAdd(KEY_4, GUI_MOD_CTRL, CMD_SHOW_ANIM);

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

    if (GRender_GetUIBackend() == 0 && !GRender_HasImGui()) {
        std::fprintf(stderr, "[ui-smoke] ImGui 未被接管，UI 不会绘制\n");
        GCore_Shutdown();
        GRender_Shutdown();
        Window::shutdown_sdk();
        return 1;
    }

    std::printf("[ui-smoke] backend=%s ui=%s frames=%d viewport=%dx%d ui_interval=%d page=%d\n",
                backend_name, ui_backend == 1 ? "builtin" : "imgui",
                frames, width, height, ui_interval, page);

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
        // 动画时间源：块一补间的演示时钟（宿主自持）+ 块二播放器推进。
        // 两者都在锁外，与 drain_and_apply 同级。
        state.demo_t += dt;
        GUi_AnimAdvance(dt);
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
