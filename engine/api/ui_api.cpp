// ===========================================================================
// engine/api/ui_api.cpp — 内置 UI（Dear ImGui）的 C 接口实现
//
// 全部函数都有两道闸：
//   1) 编译期：GRYCE_HAS_IMGUI 没定义时退化成空实现（WebGL / 关掉 ImGui 的构建）；
//   2) 运行期：不在 UI 回调帧内时直接返回默认值。
//
// 第二道闸不是防御性编程，而是必需的：宿主通常在 tick 里顺手算布局尺寸，
// 那些调用和真正的绘制在不同阶段。没有这道闸，那些调用会往处于 Idle 状态
// 的 ImGui 上下文里塞控件，ImGui 会直接断言退出。
// ===========================================================================

#include "api/ui_api.h"
#include "api/ui_api_internal.h"
#include "api/ui_builtin_ops.h"
#include "api/ui_op_queue.h"
#include "api/ui_op_queue_internal.h"
#include "ui/ui_component.h"
#include "ui/ui_anim.h"
#include "ui/ui_id.h"
#include "ui/ui_input.h"
#include "ui/ui_layout.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

#ifdef GRYCE_HAS_IMGUI
#include <imgui.h>
#endif

namespace {

bool g_in_frame = false;

// 内置 UI 轨的实现表（见 api/ui_builtin_ops.h）。空 = 只有 ImGui 轨。
const gryce_ui::BuiltinUiOps* g_builtin_ops = nullptr;

// 后端选择：0 = ImGui（默认），1 = 内置。
int g_ui_backend = 0;

// 内置轨可用 = 帧内 + 有人装了实现表。两条判据都必须在：帧外调 GUi_* 是
// 宿主的常见误用（顺手算尺寸），那时不能往控件表里塞控件。
inline bool builtin_now() { return g_in_frame && g_builtin_ops != nullptr; }
inline const gryce_ui::BuiltinUiOps& bo() { return *g_builtin_ops; }

#ifdef GRYCE_HAS_IMGUI
// 本后端的纹理是否需要翻 V 才与 ImGui 的 uv 约定一致（见 ui_api_internal.h）。
// 默认 false = D3D12 / Vulkan 约定。render_api.cpp 在建好 ImGui 后端后会覆盖。
bool g_texture_v_flip = false;
#endif

// 语义文本色取自主题 token（ui_theme.h）：换主题时这些颜色一起换，不再有
// 散落在控件实现里的硬编码色值。两条轨共用这一个映射——ImGui 轨再包成
// ImVec4，内置轨直接用 0xRRGGBBAA。
uint32_t text_token_rgba(int kind) {
    using gryce_ui::THEME_COLOR_ACCENT;
    using gryce_ui::THEME_COLOR_ERROR;
    using gryce_ui::THEME_COLOR_OK;
    using gryce_ui::THEME_COLOR_TEXT;
    using gryce_ui::THEME_COLOR_TEXT_DIM;
    using gryce_ui::THEME_COLOR_WARN;
    int token = THEME_COLOR_TEXT;
    switch (kind) {
        case GUI_TEXT_DIM:    token = THEME_COLOR_TEXT_DIM;  break;
        case GUI_TEXT_OK:     token = THEME_COLOR_OK;       break;
        case GUI_TEXT_WARN:   token = THEME_COLOR_WARN;     break;
        case GUI_TEXT_ERROR:  token = THEME_COLOR_ERROR;    break;
        case GUI_TEXT_ACCENT: token = THEME_COLOR_ACCENT;   break;
        default:              token = THEME_COLOR_TEXT;     break;
    }
    return gryce_ui::color(token);
}

#ifdef GRYCE_HAS_IMGUI
inline bool ready() { return g_in_frame && ImGui::GetCurrentContext() != nullptr; }

ImVec4 text_color(int kind) {
    const uint32_t c = text_token_rgba(kind);
    return ImVec4(static_cast<float>((c >> 24) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(c & 0xFF) / 255.0f);
}

ImGuiWindowFlags to_window_flags(int flags) {
    ImGuiWindowFlags out = ImGuiWindowFlags_None;
    if (flags & GUI_WIN_NO_TITLE_BAR)      out |= ImGuiWindowFlags_NoTitleBar;
    if (flags & GUI_WIN_NO_RESIZE)         out |= ImGuiWindowFlags_NoResize;
    if (flags & GUI_WIN_NO_MOVE)           out |= ImGuiWindowFlags_NoMove;
    if (flags & GUI_WIN_NO_COLLAPSE)       out |= ImGuiWindowFlags_NoCollapse;
    if (flags & GUI_WIN_NO_SAVED_SETTINGS) out |= ImGuiWindowFlags_NoSavedSettings;
    if (flags & GUI_WIN_MENU_BAR)          out |= ImGuiWindowFlags_MenuBar;
    if (flags & GUI_WIN_NO_SCROLLBAR)      out |= ImGuiWindowFlags_NoScrollbar;
    if (flags & GUI_WIN_NO_BRING_TO_FRONT) out |= ImGuiWindowFlags_NoBringToFrontOnFocus;
    return out;
}

// 语义文本色取自主题 token（ui_theme.h）：换主题时这些颜色一起换，
// 不再有散落在控件实现里的硬编码色值。无 ImGui 时同样可算（token 是纯数据）。
#else
inline bool ready() { return false; }
#endif

} // namespace

namespace gryce_ui {

void enter_callback() { g_in_frame = true; }
void leave_callback() { g_in_frame = false; }

// ---- 内置 UI 轨的后端选择与实现表 ----
void set_ui_backend(int backend) { g_ui_backend = (backend == 1) ? 1 : 0; }
int  ui_backend() { return g_ui_backend; }
bool builtin_ui() { return g_ui_backend == 1; }

const BuiltinUiOps* builtin_ops() { return g_builtin_ops; }
void install_builtin_ops(const BuiltinUiOps* ops) { g_builtin_ops = ops; }

void set_texture_v_flip(bool flip) {
#ifdef GRYCE_HAS_IMGUI
    g_texture_v_flip = flip;
#else
    (void)flip;
#endif
}

// 进程内唯一实例。函数局部 static：首次调用时初始化，C++11 起线程安全。
StateStore& state() {
    static StateStore s;
    return s;
}

Layout& layout() {
    static Layout l;
    return l;
}

ComponentTree& components() {
    static ComponentTree t;
    return t;
}

Animator& animator() {
    static Animator a;
    return a;
}

ShortcutTable& shortcuts() {
    static ShortcutTable t;
    return t;
}

} // namespace gryce_ui

// ---------------------------------------------------------------------------
// 操作队列的 C ABI（与 ImGui 无关：关掉 ImGui 的构建里同样要能入队/排出，
// 否则无窗口宿主连"UI 操作受控执行"这件事本身都做不到）
// ---------------------------------------------------------------------------
namespace {

gryce_ui::Op to_op(const GUiOp* op) {
    gryce_ui::Op out;
    if (!op) return out;
    out.type = op->type;
    out.key  = op->key;
    out.arg  = op->arg;
    for (int i = 0; i < 4; ++i) out.vec[i] = op->vec[i];
    return out;
}

void from_op(const gryce_ui::Op& in, GUiOp* out) {
    if (!out) return;
    out->type = in.type;
    out->key  = in.key;
    out->arg  = in.arg;
    for (int i = 0; i < 4; ++i) out->vec[i] = in.vec[i];
    out->seq  = in.seq;
}

} // namespace

extern "C" {

int GUi_PushOp(const GUiOp* op) {
    if (!op) return 0;
    return gryce_ui::op_queue().push(to_op(op)) ? 1 : 0;
}

int GUi_DrainOps(GUiOp* out, int max) {
    if (!out || max <= 0) return 0;
    // 先取上限，避免为一次排出分配超过调用方请求的空间
    const int cap = max < gryce_ui::OpQueue::kCapacity ? max : gryce_ui::OpQueue::kCapacity;
    gryce_ui::Op buffer[gryce_ui::OpQueue::kCapacity];
    const int n = gryce_ui::op_queue().drain(buffer, cap);
    for (int i = 0; i < n; ++i) from_op(buffer[i], &out[i]);
    return n;
}

void GUi_SetOpPolicy(int max_per_drain, int min_frames_between_drain) {
    gryce_ui::OpQueue::Policy p;
    p.max_per_drain = max_per_drain;
    p.min_frames_between_drain = min_frames_between_drain;
    gryce_ui::op_queue().set_policy(p);
}

void GUi_GetOpPolicy(int* out_max_per_drain, int* out_min_frames_between_drain) {
    const gryce_ui::OpQueue::Policy p = gryce_ui::op_queue().policy();
    if (out_max_per_drain) *out_max_per_drain = p.max_per_drain;
    if (out_min_frames_between_drain) *out_min_frames_between_drain = p.min_frames_between_drain;
}

void GUi_GetOpQueueStats(int* out_pending,
                         uint64_t* out_pushed,
                         uint64_t* out_coalesced,
                         uint64_t* out_drained,
                         uint64_t* out_evicted,
                         uint64_t* out_rejected,
                         uint64_t* out_throttled) {
    const gryce_ui::OpQueue::Stats s = gryce_ui::op_queue().stats();
    if (out_pending)   *out_pending   = s.pending;
    if (out_pushed)    *out_pushed    = s.pushed;
    if (out_coalesced) *out_coalesced = s.coalesced;
    if (out_drained)   *out_drained   = s.drained;
    if (out_evicted)   *out_evicted   = s.evicted;
    if (out_rejected)  *out_rejected  = s.rejected;
    if (out_throttled) *out_throttled = s.throttled;
}

void GUi_ClearOps(void) {
    gryce_ui::op_queue().clear();
}

void GUi_AdvanceOpFrame(void) {
    gryce_ui::op_queue().advance_frame();
}

} // extern "C"

extern "C" {

int GUi_InFrame(void) {
    // 内置轨没有 ImGui 上下文，ready() 恒假——帧内判据要单独认这条路。
    if (g_in_frame && builtin_now()) return 1;
    return ready() ? 1 : 0;
}

void GUi_GetDisplaySize(float* out_w, float* out_h) {
    if (out_w) *out_w = 0.0f;
    if (out_h) *out_h = 0.0f;
    if (builtin_now()) { bo().display_size(out_w, out_h); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    const ImVec2 s = ImGui::GetIO().DisplaySize;
    if (out_w) *out_w = s.x;
    if (out_h) *out_h = s.y;
#endif
}

// ---------------------------------------------------------------------------
// 窗口 / 容器
// ---------------------------------------------------------------------------

int GUi_BeginWindow(const char* name, float x, float y, float w, float h, int flags) {
    if (builtin_now()) {
        return bo().begin_window(name, x, y, w, h, static_cast<uint32_t>(flags));
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    return ImGui::Begin(name ? name : "window", nullptr, to_window_flags(flags)) ? 1 : 0;
#else
    (void)name; (void)x; (void)y; (void)w; (void)h; (void)flags;
    return 0;
#endif
}

void GUi_EndWindow(void) {
    if (builtin_now()) { bo().end_window(); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::End();
#endif
}

int GUi_BeginChild(const char* id, float w, float h, int border) {
    // 内置轨的子区域：独立裁剪域 + 独立竖向滚动 + 可选边框。
    // 返回 0 时（栈满/退化尺寸）宿主的内联提交会落到父区域里，仍然能画。
    if (builtin_now()) return bo().begin_child(id ? id : "child", w, h, border);
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    const ImGuiChildFlags child_flags = border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None;
    return ImGui::BeginChild(id ? id : "child", ImVec2(w, h), child_flags) ? 1 : 0;
#else
    (void)id; (void)w; (void)h; (void)border;
    return 0;
#endif
}

void GUi_EndChild(void) {
    if (builtin_now()) { bo().end_child(); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndChild();
#endif
}

int GUi_BeginMenuBar(void) {
    if (builtin_now()) { bo().begin_menu_bar(); return 1; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::BeginMenuBar() ? 1 : 0;
#else
    return 0;
#endif
}

void GUi_EndMenuBar(void) {
    if (builtin_now()) { bo().end_menu_bar(); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndMenuBar();
#endif
}

int GUi_BeginMenu(const char* label) {
    // 菜单本体是弹层：点开时返回 1，宿主在里面提交 MenuItem，结束时 EndMenu
    if (builtin_now()) return bo().begin_menu(label ? label : "");
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::BeginMenu(label ? label : "") ? 1 : 0;
#else
    (void)label;
    return 0;
#endif
}

void GUi_EndMenu(void) {
    if (builtin_now()) { bo().end_menu(); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndMenu();
#endif
}

int GUi_MenuItem(const char* label, const char* shortcut, int enabled) {
    if (builtin_now()) return bo().menu_item(label ? label : "", shortcut, enabled);
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::MenuItem(label ? label : "", shortcut, false, enabled != 0) ? 1 : 0;
#else
    (void)label; (void)shortcut; (void)enabled;
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// 基础控件
// ---------------------------------------------------------------------------

void GUi_Text(const char* text) {
    if (builtin_now()) { bo().text(text); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::TextUnformatted(text ? text : "");
#else
    (void)text;
#endif
}

void GUi_TextColored(int color, const char* text) {
    if (builtin_now()) { bo().text_colored(text_token_rgba(color), text); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    const ImVec4 c = text_color(color);
    ImGui::TextColored(c, "%s", text ? text : "");
#else
    (void)color; (void)text;
#endif
}

void GUi_Separator(void) {
    if (builtin_now()) { bo().separator(); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::Separator();
#endif
}

void GUi_Spacing(void) {
    // 内置轨的 spacing(-1) = 空一行（主题行间距）。
    if (builtin_now()) { bo().spacing(-1.0f); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::Spacing();
#endif
}

void GUi_SameLine(float spacing) {
    if (builtin_now()) { bo().same_line(spacing); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::SameLine(spacing);
#else
    (void)spacing;
#endif
}

void GUi_LabelText(const char* label, const char* text) {
    if (builtin_now()) { bo().label_text(label, text); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::LabelText(label ? label : "", "%s", text ? text : "");
#else
    (void)label; (void)text;
#endif
}

int GUi_Button(const char* label) {
    if (builtin_now()) return bo().button(label);
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::Button(label ? label : "") ? 1 : 0;
#else
    (void)label;
    return 0;
#endif
}

int GUi_ButtonSize(const char* label, float w, float h) {
    if (builtin_now()) return bo().button_size(label, w, h);
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::Button(label ? label : "", ImVec2(w, h)) ? 1 : 0;
#else
    (void)label; (void)w; (void)h;
    return 0;
#endif
}

int GUi_SmallButton(const char* label) {
    // 内置轨没有独立的"小按钮"样式：退化成普通按钮（尺寸随标签），
    // 保持行为可用而不是消失。
    if (builtin_now()) return bo().button(label);
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::SmallButton(label ? label : "") ? 1 : 0;
#else
    (void)label;
    return 0;
#endif
}

int GUi_Selectable(const char* label, int selected) {
    // 内置轨没有选中态高亮：当成按钮用，返回值语义一致（点中了=1）。
    if (builtin_now()) { (void)selected; return bo().button(label); }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::Selectable(label ? label : "", selected != 0) ? 1 : 0;
#else
    (void)label; (void)selected;
    return 0;
#endif
}

void GUi_SetItemAllowOverlap(void) {
    if (builtin_now()) return;   // 内置轨不解析重叠，无需登记
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::SetNextItemAllowOverlap();
#endif
}

int GUi_Checkbox(const char* label, int* inout_value) {
    if (builtin_now()) return inout_value ? bo().checkbox(label, inout_value) : 0;
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !inout_value) return 0;
    bool v = *inout_value != 0;
    if (!ImGui::Checkbox(label ? label : "", &v)) return 0;
    *inout_value = v ? 1 : 0;
    return 1;
#else
    (void)label; (void)inout_value;
    return 0;
#endif
}

int GUi_SliderFloat(const char* label, float* inout_value,
                    float min_value, float max_value, const char* fmt) {
    if (builtin_now()) {
        (void)fmt;   // 内置轨的滑块不显示数字（只有轨道 + 填充 + 手柄）
        return inout_value ? bo().slider_float(label, inout_value, min_value, max_value) : 0;
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !inout_value) return 0;
    return ImGui::SliderFloat(label ? label : "", inout_value, min_value, max_value,
                              fmt ? fmt : "%.3f")
               ? 1
               : 0;
#else
    (void)label; (void)inout_value; (void)min_value; (void)max_value; (void)fmt;
    return 0;
#endif
}

// DragFloat 系列靠"按住横向拖拽改值"的语义，内置轨的滑块是"点/拖到某个
// 位置=那个值"，两者不能混用（调用方按 1.0f 的量级调 speed，映射成像素会
// 差几个数量级）。所以内置轨下这三个是纯显示：画一条只读滑块反映当前值，
// 返回 0（没有变化）。
static int builtin_readonly_slider(const char* label, float* v, float lo, float hi) {
    if (!v || !builtin_now()) return 0;
    float tmp = *v;
    bo().slider_float(label, &tmp, lo, hi);
    return 0;
}

int GUi_DragFloat(const char* label, float* inout_value, float speed) {
    // 内置轨：按住左右拖改值（Ctrl 精调 1/10），没有上下界（不限幅）
    if (builtin_now()) {
        if (!inout_value) return 0;
        return bo().drag_float(label, inout_value, speed, 0.0f, 0.0f);
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !inout_value) return 0;
    return ImGui::DragFloat(label ? label : "", inout_value, speed) ? 1 : 0;
#else
    (void)label; (void)inout_value; (void)speed;
    return 0;
#endif
}

int GUi_DragFloat2(const char* label, float v[2], float speed) {
    if (builtin_now()) { (void)label; (void)v; (void)speed; return 0; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !v) return 0;
    return ImGui::DragFloat2(label ? label : "", v, speed) ? 1 : 0;
#else
    (void)label; (void)v; (void)speed;
    return 0;
#endif
}

int GUi_DragFloat3(const char* label, float v[3], float speed) {
    if (builtin_now()) { (void)label; (void)v; (void)speed; return 0; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !v) return 0;
    return ImGui::DragFloat3(label ? label : "", v, speed) ? 1 : 0;
#else
    (void)label; (void)v; (void)speed;
    return 0;
#endif
}

int GUi_DragFloat4(const char* label, float v[4], float speed) {
    if (builtin_now()) { (void)label; (void)v; (void)speed; return 0; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !v) return 0;
    return ImGui::DragFloat4(label ? label : "", v, speed) ? 1 : 0;
#else
    (void)label; (void)v; (void)speed;
    return 0;
#endif
}

int GUi_ColorEdit4(const char* label, float v[4]) {
    // 内置轨：色块 + 弹层里的 R/G/B/A 四条滑条（点色块打开）
    if (builtin_now()) {
        if (!v) return 0;
        int changed = 0;
        char popup_id[96];
        std::snprintf(popup_id, sizeof(popup_id), "##color_%s", label ? label : "edit");
        char btn_id[96];
        std::snprintf(btn_id, sizeof(btn_id), "##swatch_%s", label ? label : "edit");
        if (bo().color_button(btn_id, v)) {
            float lx = 0.0f, ly = 0.0f, lw = 0.0f, lh = 0.0f;
            bo().last_item_rect(&lx, &ly, &lw, &lh);
            bo().open_popup(popup_id, lx, ly + lh, 220.0f, 0.0f);
        }
        if (bo().begin_popup(popup_id)) {
            if (label && *label) bo().text(label);
            if (bo().slider_float("R", &v[0], 0.0f, 1.0f)) changed = 1;
            if (bo().slider_float("G", &v[1], 0.0f, 1.0f)) changed = 1;
            if (bo().slider_float("B", &v[2], 0.0f, 1.0f)) changed = 1;
            if (bo().slider_float("A", &v[3], 0.0f, 1.0f)) changed = 1;
            bo().end_popup();
        }
        return changed;
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !v) return 0;
    return ImGui::ColorEdit4(label ? label : "", v, ImGuiColorEditFlags_Float) ? 1 : 0;
#else
    (void)label; (void)v;
    return 0;
#endif
}

int GUi_ColorButton(const char* label, const float rgba[4]) {
    if (builtin_now()) return bo().color_button(label, rgba);
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    const ImVec4 c = rgba ? ImVec4(rgba[0], rgba[1], rgba[2], rgba[3])
                          : ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    return ImGui::ColorButton(label ? label : "", c,
                              ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop)
               ? 1
               : 0;
#else
    (void)label; (void)rgba;
    return 0;
#endif
}

int GUi_InputText(const char* label, char* buf, int buf_size) {
    if (builtin_now()) return bo().input_text(label, buf, buf_size);
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !buf || buf_size <= 0) return 0;
    return ImGui::InputText(label ? label : "", buf, static_cast<size_t>(buf_size)) ? 1 : 0;
#else
    (void)label; (void)buf; (void)buf_size;
    return 0;
#endif
}

void GUi_SetKeyboardFocusHere(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::SetKeyboardFocusHere();
#endif
}

int GUi_IsItemDeactivatedAfterEdit(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsItemDeactivatedAfterEdit() ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_Combo(const char* label, int* current, const char* const* items, int count) {
    // 内置轨没有弹出列表：退化成"点一下换下一个"的按钮。语义与 ImGui 的
    // Combo 一致（变化时返回 1 并写回 *current），只是交互方式不同——
    // 对工具栏里的少量枚举选项够用，等真需要下拉时再给内置轨加弹层。
    if (builtin_now()) {
        if (!current || !items || count <= 0) return 0;
        const int cur = (*current < 0 || *current >= count) ? 0 : *current;

        // 按钮显示"标签: 当前项"，点开是弹层列表
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s: %s", label ? label : "",
                      items[cur] ? items[cur] : "");
        char popup_id[96];
        std::snprintf(popup_id, sizeof(popup_id), "##combo_%s", label ? label : "c");

        int changed = 0;
        if (bo().button(buf)) {
            float lx = 0.0f, ly = 0.0f, lw = 0.0f, lh = 0.0f;
            bo().last_item_rect(&lx, &ly, &lw, &lh);
            bo().open_popup(popup_id, lx, ly + lh, lw > 120.0f ? lw : 120.0f, 0.0f);
        }
        if (bo().begin_popup(popup_id)) {
            for (int i = 0; i < count; ++i) {
                if (bo().menu_item(items[i] ? items[i] : "", nullptr, 1)) {
                    *current = i;
                    changed = 1;
                }
            }
            bo().end_popup();
        }
        return changed;
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !current || !items || count <= 0) return 0;
    return ImGui::Combo(label ? label : "", current, items, count) ? 1 : 0;
#else
    (void)label; (void)current; (void)items; (void)count;
    return 0;
#endif
}

void GUi_ProgressBar(float fraction, float w, float h, const char* overlay) {
    // 内置轨的进度条总是撑满内容区（w/h 是 ImGui 专用尺寸参数）。
    if (builtin_now()) { (void)w; (void)h; bo().progress_bar(fraction, overlay); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::ProgressBar(fraction, ImVec2(w, h), overlay);
#else
    (void)fraction; (void)w; (void)h; (void)overlay;
#endif
}

// ---------------------------------------------------------------------------
// 树 / 折叠
// ---------------------------------------------------------------------------

int GUi_TreeNode(const char* label, int default_open) {
    // 内置轨：展开态存在控件里（default_open 只在首次创建时生效）
    if (builtin_now()) return bo().tree_node(label ? label : "", default_open);
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (default_open) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    return ImGui::TreeNodeEx(label ? label : "", flags) ? 1 : 0;
#else
    (void)label; (void)default_open;
    return 0;
#endif
}

void GUi_TreePop(void) {
    if (builtin_now()) { bo().tree_pop(); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::TreePop();
#endif
}

int GUi_CollapsingHeader(const char* label, int default_open) {
    if (builtin_now()) return bo().collapsing_header(label ? label : "", default_open);
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    const ImGuiTreeNodeFlags flags =
        default_open ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
    return ImGui::CollapsingHeader(label ? label : "", flags) ? 1 : 0;
#else
    (void)label; (void)default_open;
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// 图片
// ---------------------------------------------------------------------------

void GUi_Image(uint64_t tex, float w, float h) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    // 必须显式给 uv，不能用 Image() 的默认值：这版 ImGui 的默认是
    // (0,0)-(1,1)（"第一行在 v=0"，字体图集按这个约定上传），而 OpenGL 的
    // 渲染目标纹理第一行是图像的**下**边 —— 用默认值会把视口画面上下颠倒。
    // 详见 IImGuiBackend::texture_needs_v_flip 的注释。
    const ImVec2 uv0 = g_texture_v_flip ? ImVec2(0.0f, 1.0f) : ImVec2(0.0f, 0.0f);
    const ImVec2 uv1 = g_texture_v_flip ? ImVec2(1.0f, 0.0f) : ImVec2(1.0f, 1.0f);
    ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(w, h), uv0, uv1);
#else
    (void)tex; (void)w; (void)h;
#endif
}

// ---------------------------------------------------------------------------
// 弹出菜单
// ---------------------------------------------------------------------------

void GUi_OpenPopupOnItemClick(const char* id, int mouse_button) {
    if (builtin_now()) {
        if (!id) return;
        // 右键（或指定键）点中刚提交的 item 才开：位置贴它的下沿
        if (bo().is_item_clicked(mouse_button)) {
            bo().open_popup_on_item(id, 160.0f);
        }
        return;
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    const ImGuiPopupFlags flags =
        (mouse_button == GUI_MOUSE_RIGHT) ? ImGuiPopupFlags_MouseButtonRight
                                          : ImGuiPopupFlags_MouseButtonLeft;
    ImGui::OpenPopupOnItemClick(id ? id : nullptr, flags);
#else
    (void)id; (void)mouse_button;
#endif
}

void GUi_OpenPopup(const char* id) {
    // 内置轨：位置贴"刚提交的 item"的下沿（与 ImGui 的 OpenPopup 一样要求
    // 在前一个 item 之后调用）
    if (builtin_now()) {
        if (id) bo().open_popup_on_item(id, 160.0f);
        return;
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !id) return;
    ImGui::OpenPopup(id);
#else
    (void)id;
#endif
}

int GUi_BeginPopup(const char* id) {
    if (builtin_now()) return id ? bo().begin_popup(id) : 0;
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !id) return 0;
    return ImGui::BeginPopup(id) ? 1 : 0;
#else
    (void)id;
    return 0;
#endif
}

void GUi_EndPopup(void) {
    if (builtin_now()) { bo().end_popup(); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndPopup();
#endif
}

void GUi_CloseCurrentPopup(void) {
    if (builtin_now()) { bo().close_current_popup(); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::CloseCurrentPopup();
#endif
}

void GUi_SetTooltip(const char* text) {
    // 内置轨带 0.35 秒悬停延时（宿主时钟为 0 时立即出），画在鼠标右下
    if (builtin_now()) { bo().set_tooltip(text); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !text) return;
    ImGui::SetTooltip("%s", text);
#else
    (void)text;
#endif
}

void GUi_OpenModal(const char* name, float w, float h) {
    if (builtin_now()) { bo().open_modal(name ? name : "modal", w, h); return; }
    // ImGui 轨没有独立的 modal 概念：用普通 popup 居中模拟
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !name) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->GetCenter().y),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (w > 0.0f && h > 0.0f) ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::OpenPopup(name);
#else
    (void)name; (void)w; (void)h;
#endif
}

int GUi_BeginModal(const char* name, float w, float h) {
    if (builtin_now()) {
        if (!name) return 0;
        // 点外面不关、Esc 关（模态语义在模型层里）
        return bo().begin_modal(name, w, h);
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !name) return 0;
    return ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_NoSavedSettings) ? 1 : 0;
#else
    (void)name; (void)w; (void)h;
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// 拖放
// ---------------------------------------------------------------------------

int GUi_BeginDragDropSource(void) {
    // 内置轨：按住刚提交的 item 并拖出 4px 即成立
    if (builtin_now()) return bo().begin_drag_drop_source();
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    // SourceAllowNullID：图标的条目（比如只带 ##id 的 Selectable）也要能拖。
    return ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID) ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_SetDragDropPayloadString(const char* type, const char* text) {
    if (builtin_now()) {
        if (!type || !type[0]) return 0;
        bo().set_drag_drop_payload_string(type, text);
        return 1;
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !type || !type[0]) return 0;
    const char* src = text ? text : "";
    // 连结尾的 '\0' 一起送，接收端拿到的就是一段合法的 C 字符串。
    return ImGui::SetDragDropPayload(type, src, std::strlen(src) + 1) ? 1 : 0;
#else
    (void)type; (void)text;
    return 0;
#endif
}

void GUi_EndDragDropSource(void) {
    if (builtin_now()) { bo().end_drag_drop_source(); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndDragDropSource();
#endif
}

int GUi_BeginDragDropTarget(void) {
    if (builtin_now()) return bo().begin_drag_drop_target();
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::BeginDragDropTarget() ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_AcceptDragDropPayloadString(const char* type, char* out_buf, int buf_size) {
    if (builtin_now()) {
        if (!type || !type[0]) return GUI_DRAG_NONE;
        // 悬停期间给 PREVIEW（宿主能画高亮），松手那帧才 DELIVER
        if (!bo().begin_drag_drop_target()) return GUI_DRAG_NONE;
        char tmp[256] = {};
        if (bo().accept_drag_drop_payload_string(type, tmp, sizeof(tmp))) {
            if (out_buf && buf_size > 0) {
                std::strncpy(out_buf, tmp, static_cast<std::size_t>(buf_size) - 1);
                out_buf[buf_size - 1] = '\0';
            }
            return GUI_DRAG_DELIVER;
        }
        return GUI_DRAG_PREVIEW;
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !type || !type[0]) return GUI_DRAG_NONE;
    // AcceptBeforeDelivery：悬停期间也返回 payload（但 IsDelivery 为假），
    // 这样目标能每帧画高亮，而真正的投放只发生一次。
    const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
        type, ImGuiDragDropFlags_AcceptBeforeDelivery);
    if (!payload) return GUI_DRAG_NONE;
    if (!payload->IsDelivery()) return GUI_DRAG_PREVIEW;
    if (out_buf && buf_size > 0 && payload->Data && payload->DataSize > 0) {
        const char* src = static_cast<const char*>(payload->Data);
        std::strncpy(out_buf, src, static_cast<std::size_t>(buf_size) - 1);
        out_buf[buf_size - 1] = '\0';
    }
    return GUI_DRAG_DELIVER;
#else
    (void)type; (void)out_buf; (void)buf_size;
    return GUI_DRAG_NONE;
#endif
}

void GUi_EndDragDropTarget(void) {
    if (builtin_now()) { bo().end_drag_drop_target(); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndDragDropTarget();
#endif
}

// ---------------------------------------------------------------------------
// 布局 / 命中查询
// ---------------------------------------------------------------------------

void GUi_SetCursorPos(float x, float y) {
    if (builtin_now()) { bo().set_cursor_pos(x, y); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::SetCursorPos(ImVec2(x, y));
#else
    (void)x; (void)y;
#endif
}

void GUi_GetCursorPos(float* out_x, float* out_y) {
    if (out_x) *out_x = 0.0f;
    if (out_y) *out_y = 0.0f;
    // 内置轨里光标由控件流推进，宿主持有不了它；退回"上一个 item 的左上角"，
    // 这对"在它下面接着排"的常见用法是同一个答案。
    if (builtin_now()) {
        bo().last_item_rect(out_x, out_y, nullptr, nullptr);
        return;
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    const ImVec2 p = ImGui::GetCursorPos();
    if (out_x) *out_x = p.x;
    if (out_y) *out_y = p.y;
#endif
}

void GUi_GetContentAvail(float* out_w, float* out_h) {
    if (out_w) *out_w = 0.0f;
    if (out_h) *out_h = 0.0f;
    if (builtin_now()) { bo().get_content_avail(out_w, out_h); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    const ImVec2 s = ImGui::GetContentRegionAvail();
    if (out_w) *out_w = s.x;
    if (out_h) *out_h = s.y;
#endif
}

void GUi_SetNextItemWidth(float w) {
    if (builtin_now()) { bo().set_next_item_width(w); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::SetNextItemWidth(w);
#else
    (void)w;
#endif
}

void GUi_Indent(float px) {
    if (builtin_now()) { bo().indent(px); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::Indent(px > 0.0f ? px : 0.0f);
#else
    (void)px;
#endif
}

void GUi_Unindent(float px) {
    if (builtin_now()) { bo().unindent(px); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::Unindent(px > 0.0f ? px : 0.0f);
#else
    (void)px;
#endif
}

float GUi_GetWindowScrollY(void) {
    if (builtin_now()) return bo().window_scroll_y();
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0.0f;
    return ImGui::GetScrollY();
#else
    return 0.0f;
#endif
}

void GUi_SetWindowScrollY(float v) {
    if (builtin_now()) { bo().set_window_scroll_y(v); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::SetScrollY(v);
#else
    (void)v;
#endif
}

float GUi_GetTextLineHeight(void) {
    if (builtin_now()) return bo().text_line_height();
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0.0f;
    return ImGui::GetTextLineHeight();
#else
    return 0.0f;
#endif
}

float GUi_GetFrameHeight(void) {
    if (builtin_now()) return bo().frame_height();
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0.0f;
    return ImGui::GetFrameHeight();
#else
    return 0.0f;
#endif
}

float GUi_CalcTextWidth(const char* text) {
    if (builtin_now()) return bo().calc_text_width(text);
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !text) return 0.0f;
    return ImGui::CalcTextSize(text).x;
#else
    (void)text;
    return 0.0f;
#endif
}

void GUi_LastItemRect(float* out_x, float* out_y, float* out_w, float* out_h) {
    if (out_x) *out_x = 0.0f;
    if (out_y) *out_y = 0.0f;
    if (out_w) *out_w = 0.0f;
    if (out_h) *out_h = 0.0f;
    if (builtin_now()) { bo().last_item_rect(out_x, out_y, out_w, out_h); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (out_x) *out_x = min.x;
    if (out_y) *out_y = min.y;
    if (out_w) *out_w = max.x - min.x;
    if (out_h) *out_h = max.y - min.y;
#endif
}

int GUi_IsItemHovered(void) {
    if (builtin_now()) return bo().is_item_hovered();
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsItemHovered() ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_IsItemClicked(int mouse_button) {
    if (builtin_now()) {
        // 刚提交的 item 被某个键点中（右键是上下文菜单的入口）
        const int b = (mouse_button == GUI_MOUSE_RIGHT) ? 1 : 0;
        return bo().is_item_clicked(b);
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsItemClicked(mouse_button == GUI_MOUSE_RIGHT ? 1 : 0) ? 1 : 0;
#else
    (void)mouse_button;
    return 0;
#endif
}

int GUi_IsItemActive(void) {
    if (builtin_now()) return bo().is_item_active();
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsItemActive() ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_IsAnyItemActive(void) {
    if (builtin_now()) return bo().is_any_item_active();
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsAnyItemActive() ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_GetModifiers(void) {
    if (builtin_now()) {
        // 内置轨的修饰键由本帧输入快照给出（位定义与 GUI_MOD_* 一致）
        const int raw = bo().get_modifiers();
        int mods = 0;
        if (raw & 2) mods |= GUI_MOD_CTRL;
        if (raw & 1) mods |= GUI_MOD_SHIFT;
        if (raw & 4) mods |= GUI_MOD_ALT;
        return mods;
    }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    const ImGuiIO& io = ImGui::GetIO();
    int mods = 0;
    if (io.KeyCtrl)  mods |= GUI_MOD_CTRL;
    if (io.KeyShift) mods |= GUI_MOD_SHIFT;
    if (io.KeyAlt)   mods |= GUI_MOD_ALT;
    if (io.KeySuper) mods |= GUI_MOD_SUPER;
    return mods;
#else
    return 0;
#endif
}

void GUi_MousePos(float* out_x, float* out_y) {
    if (out_x) *out_x = 0.0f;
    if (out_y) *out_y = 0.0f;
    if (builtin_now()) { bo().mouse_pos(out_x, out_y); return; }
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    const ImVec2 p = ImGui::GetIO().MousePos;
    if (out_x) *out_x = p.x;
    if (out_y) *out_y = p.y;
#endif
}

float GUi_MouseDeltaX(void) {
    // 内置轨不维护自己的 delta（控件交互只看位置与边沿）。宿主要 delta 时
    // 直接读 GInput_GetMouseDelta —— 那才是权威来源。
    if (builtin_now()) return 0.0f;
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0.0f;
    return ImGui::GetIO().MouseDelta.x;
#else
    return 0.0f;
#endif
}

float GUi_MouseDeltaY(void) {
    if (builtin_now()) return 0.0f;
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0.0f;
    return ImGui::GetIO().MouseDelta.y;
#else
    return 0.0f;
#endif
}

int GUi_IsMouseDown(int button) {
    if (builtin_now()) return bo().is_mouse_down(button);
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsMouseDown(button) ? 1 : 0;
#else
    (void)button;
    return 0;
#endif
}

int GUi_IsMouseClicked(int button) {
    if (builtin_now()) return bo().is_mouse_clicked(button);
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsMouseClicked(button) ? 1 : 0;
#else
    (void)button;
    return 0;
#endif
}

int GUi_IsMouseReleased(int button) {
    if (builtin_now()) return bo().is_mouse_released(button);
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsMouseReleased(button) ? 1 : 0;
#else
    (void)button;
    return 0;
#endif
}

int GUi_WantCaptureMouse(void) {
    if (builtin_now()) return bo().want_capture_mouse();
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::GetIO().WantCaptureMouse ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_WantCaptureKeyboard(void) {
    if (builtin_now()) return bo().want_capture_keyboard();
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::GetIO().WantCaptureKeyboard ? 1 : 0;
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// 样式
// ---------------------------------------------------------------------------

void GUi_PushTextStyle(int kind) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::PushStyleColor(ImGuiCol_Text, text_color(kind));
#else
    (void)kind;
#endif
}

void GUi_PopTextStyle(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::PopStyleColor();
#endif
}

// ---------------------------------------------------------------------------
// 稳定 id / 状态 / 布局 / 主题 / 输入（全部与 ImGui 无关：关掉 ImGui 的
// 构建里模型层照常工作，只有"画"这一步不存在）
// ---------------------------------------------------------------------------

// C 头里的 token 编号与 ui_theme.h 的枚举必须逐一对上：两边各写一份是
// 漂移的经典来源，用静态断言钉死。
static_assert(GUI_THEME_COLOR_WINDOW_BG == static_cast<int>(gryce_ui::THEME_COLOR_WINDOW_BG),
              "theme color token mismatch");
static_assert(GUI_THEME_COLOR_COUNT == static_cast<int>(gryce_ui::THEME_COLOR_COUNT),
              "theme color token count mismatch");
static_assert(GUI_THEME_COLOR_CHECK_MARK == static_cast<int>(gryce_ui::THEME_COLOR_CHECK_MARK),
              "theme color token mismatch (extended)");
static_assert(GUI_THEME_COLOR_TAB_DIMMED_SELECTED ==
                  static_cast<int>(gryce_ui::THEME_COLOR_TAB_DIMMED_SELECTED),
              "theme color token mismatch (extended tail)");
static_assert(GUI_THEME_METRIC_ROW_H == static_cast<int>(gryce_ui::THEME_METRIC_ROW_H),
              "theme metric token mismatch");
static_assert(GUI_THEME_METRIC_COUNT == static_cast<int>(gryce_ui::THEME_METRIC_COUNT),
              "theme metric token count mismatch");
static_assert(GUI_THEME_METRIC_GRAB_MIN_SIZE ==
                  static_cast<int>(gryce_ui::THEME_METRIC_GRAB_MIN_SIZE),
              "theme metric token mismatch (extended tail)");
static_assert(GUI_THEME_PRESET_COUNT == static_cast<int>(gryce_ui::THEME_PRESET_COUNT),
              "theme preset count mismatch");
static_assert(GUI_THEME_PRESET_HIGH_CONTRAST ==
                  static_cast<int>(gryce_ui::THEME_PRESET_HIGH_CONTRAST),
              "theme preset mismatch");
static_assert(GUI_ANIM_EASE_COUNT == gryce_ui::kAnimEaseCount, "anim ease count mismatch");
static_assert(GUI_ANIM_EASE_OUT_BOUNCE ==
                  static_cast<int>(gryce_ui::AnimEase::EaseOutBounce),
              "anim ease token mismatch");
static_assert(GUI_ANIM_LOOP_PINGPONG == static_cast<int>(gryce_ui::AnimLoop::PingPong),
              "anim loop token mismatch");

uint64_t GUi_MakeId(const char* label, uint64_t seed) {
    return gryce_ui::hash_label(label, seed);
}

void GUi_StateBeginFrame(uint32_t frame) {
    gryce_ui::state().begin_frame(frame);
}

int GUi_StateGetFloat(uint64_t id, float* out) {
    const gryce_ui::StateEntry* e = gryce_ui::state().find(id);
    if (!e || !out) return 0;
    *out = e->f;
    return 1;
}

int GUi_StateGetInt(uint64_t id, int* out) {
    const gryce_ui::StateEntry* e = gryce_ui::state().find(id);
    if (!e || !out) return 0;
    *out = static_cast<int>(e->i);
    return 1;
}

int GUi_StateGetBool(uint64_t id, int* out) {
    const gryce_ui::StateEntry* e = gryce_ui::state().find(id);
    if (!e || !out) return 0;
    *out = e->b ? 1 : 0;
    return 1;
}

int GUi_StateGetVec4(uint64_t id, float out[4]) {
    const gryce_ui::StateEntry* e = gryce_ui::state().find(id);
    if (!e || !out) return 0;
    for (int i = 0; i < 4; ++i) out[i] = e->vec[i];
    return 1;
}

int GUi_StateGetText(uint64_t id, char* out, int cap) {
    if (!out || cap <= 0) return 0;
    const size_t n = gryce_ui::state().get_text(id, out, static_cast<size_t>(cap));
    return n > 0 ? 1 : 0;
}

void GUi_StateSetFloat(uint64_t id, float v) { gryce_ui::state().set_float(id, v); }
void GUi_StateSetInt(uint64_t id, int v) { gryce_ui::state().set_int(id, v); }
void GUi_StateSetBool(uint64_t id, int v) { gryce_ui::state().set_bool(id, v != 0); }

void GUi_StateSetVec4(uint64_t id, const float v[4]) {
    gryce_ui::state().set_vec4(id, v);
}

void GUi_StateSetText(uint64_t id, const char* s) {
    gryce_ui::state().set_text(id, s);
}

int GUi_StateCount(void) {
    return static_cast<int>(gryce_ui::state().size());
}

void GUi_StateClear(void) {
    gryce_ui::state().clear();
}

void GUi_LayoutReset(const GRUiRect* viewport) {
    if (!viewport) return;
    gryce_ui::Rect r{viewport->x, viewport->y, viewport->w, viewport->h};
    gryce_ui::layout().reset(r);
}

void GUi_LayoutBeginPanel(const GRUiRect* area, float pad) {
    if (!area) return;
    gryce_ui::Rect r{area->x, area->y, area->w, area->h};
    gryce_ui::layout().begin_panel(r, pad);
}

void GUi_LayoutEndPanel(void) {
    gryce_ui::layout().end_panel();
}

int GUi_LayoutDepth(void) {
    return gryce_ui::layout().depth();
}

void GUi_LayoutContentRect(GRUiRect* out) {
    if (!out) return;
    const gryce_ui::Rect r = gryce_ui::layout().content();
    out->x = r.x; out->y = r.y; out->w = r.w; out->h = r.h;
}

float GUi_LayoutAvailHeight(void) {
    return gryce_ui::layout().avail_height();
}

void GUi_LayoutBeginRow(float height) {
    gryce_ui::layout().begin_row(height);
}

void GUi_LayoutEndRow(void) {
    gryce_ui::layout().end_row();
}

int GUi_LayoutCell(float w, float h, GRUiRect* out) {
    const gryce_ui::Rect r = gryce_ui::layout().cell(w, h);
    if (out) {
        out->x = r.x; out->y = r.y; out->w = r.w; out->h = r.h;
    }
    return 1;
}

void GUi_LayoutSpace(float px) {
    gryce_ui::layout().space(px);
}

void GUi_LayoutIndent(float px) {
    gryce_ui::layout().indent(px);
}

// ---------------------------------------------------------------------------
// 组件树布局（纯数据：不画东西，回调内外都可调）
// ---------------------------------------------------------------------------
namespace {

gryce_ui::Component to_component(const GUiCompSpec* s) {
    gryce_ui::Component c{};
    if (!s) return c;
    c.parent = s->parent;
    c.kind   = static_cast<gryce_ui::CompKind>(s->kind);
    c.rect   = gryce_ui::Rect{s->x, s->y, s->w, s->h};
    for (int i = 0; i < 4; ++i) {
        c.margin[i]  = s->margin[i];
        c.padding[i] = s->padding[i];
    }
    c.flex  = s->flex;
    c.align = static_cast<gryce_ui::CompAlign>(s->align);
    c.gap   = s->gap;
    c.cols  = s->cols;
    return c;
}

void from_component(const gryce_ui::Component& c, GUiCompSpec* s) {
    if (!s) return;
    s->parent = c.parent;
    s->kind   = static_cast<int>(c.kind);
    s->x = c.rect.x; s->y = c.rect.y; s->w = c.rect.w; s->h = c.rect.h;
    for (int i = 0; i < 4; ++i) {
        s->margin[i]  = c.margin[i];
        s->padding[i] = c.padding[i];
    }
    s->flex  = c.flex;
    s->align = static_cast<int>(c.align);
    s->gap   = c.gap;
    s->cols  = c.cols;
}

// kind / align 越界的 spec 直接拒绝：静默夹到合法值会让"我传的 99 怎么了"
// 这种问题没有答案。
bool comp_spec_valid(const GUiCompSpec* s) {
    if (!s) return false;
    if (s->kind < GUI_COMP_KIND_LEAF || s->kind > GUI_COMP_KIND_GRID) return false;
    if (s->align < GUI_COMP_ALIGN_START || s->align > GUI_COMP_ALIGN_STRETCH) return false;
    return true;
}

} // namespace

uint64_t GUi_CompAdd(const GUiCompSpec* spec) {
    if (!comp_spec_valid(spec)) return 0;
    return gryce_ui::components().add(to_component(spec));
}

int GUi_CompSet(uint64_t id, const GUiCompSpec* spec) {
    if (!comp_spec_valid(spec)) return 0;
    return gryce_ui::components().set(id, to_component(spec)) ? 1 : 0;
}

int GUi_CompRemove(uint64_t id) {
    return gryce_ui::components().remove(id) ? 1 : 0;
}

void GUi_CompClear(void) {
    gryce_ui::components().clear();
}

int GUi_CompResolve(void) {
    gryce_ui::components().resolve();
    return gryce_ui::components().count();
}

int GUi_CompRect(uint64_t id, GRUiRect* out) {
    if (!out) return 0;
    const gryce_ui::Rect* r = gryce_ui::components().rect_of(id);
    if (!r) return 0;
    out->x = r->x; out->y = r->y; out->w = r->w; out->h = r->h;
    return 1;
}

int GUi_CompGet(uint64_t id, GUiCompSpec* out) {
    if (!out) return 0;
    const gryce_ui::Component* c = gryce_ui::components().find(id);
    if (!c) return 0;
    from_component(*c, out);
    return 1;
}

int GUi_CompKind(uint64_t id) {
    const gryce_ui::Component* c = gryce_ui::components().find(id);
    return c ? static_cast<int>(c->kind) : -1;
}

uint64_t GUi_CompParent(uint64_t id) {
    const gryce_ui::Component* c = gryce_ui::components().find(id);
    return c ? c->parent : 0;
}

int GUi_CompChildCount(uint64_t id) {
    return gryce_ui::components().child_count(id);
}

uint64_t GUi_CompChildAt(uint64_t id, int index) {
    return gryce_ui::components().child_at(id, index);
}

int GUi_CompCount(void) {
    return gryce_ui::components().count();
}

void GUi_ThemeSetPreset(int preset) {
    gryce_ui::set_preset(preset);
}

uint32_t GUi_ThemeColor(int token) {
    return gryce_ui::color(token);
}

void GUi_ThemeSetColor(int token, uint32_t rgba) {
    gryce_ui::set_color(token, rgba);
}

float GUi_ThemeMetric(int token) {
    return gryce_ui::metric(token);
}

void GUi_ThemeApply(void) {
    gryce_ui::apply();
}

int GUi_ThemePresetCount(void) {
    return gryce_ui::THEME_PRESET_COUNT;
}

int GUi_ThemePresetName(int preset, char* out_buf, int cap) {
    if (!out_buf || cap <= 0) return 0;
    const char* name = gryce_ui::theme_preset_name(preset);
    const size_t n = std::strlen(name);
    if (n >= static_cast<size_t>(cap)) {
        std::memcpy(out_buf, name, static_cast<size_t>(cap) - 1);
        out_buf[cap - 1] = '\0';
        return cap - 1;
    }
    std::memcpy(out_buf, name, n + 1);
    return static_cast<int>(n);
}

// ---------------------------------------------------------------------------
// 动画系统（纯数据：与 ImGui 无关，回调内外都可调）
//
// 块一的三个纯函数直接转发模型层；块二的播放器是进程内唯一实例
// （多播放器场景直接实例化 gryce_ui::Animator，不走 C 面）。
// ---------------------------------------------------------------------------
namespace {

gryce_ui::AnimEase to_ease(int ease) {
    if (ease < 0 || ease >= gryce_ui::kAnimEaseCount) return gryce_ui::AnimEase::Linear;
    return static_cast<gryce_ui::AnimEase>(ease);
}

gryce_ui::AnimLoop to_loop(int loop) {
    if (loop < 0 || loop > static_cast<int>(gryce_ui::AnimLoop::PingPong)) {
        return gryce_ui::AnimLoop::Once;
    }
    return static_cast<gryce_ui::AnimLoop>(loop);
}

// C 结构体 -> 模型层轨道。越界枚举退回默认值（C 调用方可能传来垃圾）。
gryce_ui::AnimClipTrack to_track(const GUiAnimTrack* t) {
    gryce_ui::AnimClipTrack out{};
    if (!t) return out;
    std::strncpy(out.channel, t->channel, sizeof(out.channel) - 1);
    out.channel[sizeof(out.channel) - 1] = '\0';
    out.delay    = t->delay;
    out.is_color = t->is_color ? 1 : 0;
    out.ease     = to_ease(t->ease);
    out.duration = t->duration;
    out.color_from.r = t->color_from[0];
    out.color_from.g = t->color_from[1];
    out.color_from.b = t->color_from[2];
    out.color_from.a = t->color_from[3];
    out.color_to.r = t->color_to[0];
    out.color_to.g = t->color_to[1];
    out.color_to.b = t->color_to[2];
    out.color_to.a = t->color_to[3];
    for (int i = 0; i < t->key_count && i < gryce_ui::AnimTrack::kMaxKeys; ++i) {
        out.keys.add(t->keys[i].t, t->keys[i].v, to_ease(t->keys[i].ease));
    }
    return out;
}

gryce_ui::AnimClip to_clip(const GUiAnimClip* c) {
    gryce_ui::AnimClip out{};
    if (!c) return out;
    std::strncpy(out.name, c->name, sizeof(out.name) - 1);
    out.name[sizeof(out.name) - 1] = '\0';
    out.duration = c->duration;
    out.loop     = to_loop(c->loop);
    for (int i = 0; i < c->track_count && i < gryce_ui::AnimClip::kMaxTracks; ++i) {
        out.add_track(to_track(&c->tracks[i]));
    }
    return out;
}

} // namespace

float GUi_AnimEase(int ease, float t) {
    return gryce_ui::anim_ease(to_ease(ease), t);
}

void GUi_AnimTween(float from, float to, float duration, float delay, int ease, float t,
                   float* out) {
    if (!out) return;
    gryce_ui::AnimTween tw;
    tw.from     = from;
    tw.to       = to;
    tw.duration = duration;
    tw.delay    = delay;
    tw.ease     = to_ease(ease);
    *out = tw.value_at(t);
}

void GUi_AnimColorLerp(const float a[4], const float b[4], float t, float out[4]) {
    if (!out) return;
    gryce_ui::AnimColor ca{}, cb{};
    if (a) { ca.r = a[0]; ca.g = a[1]; ca.b = a[2]; ca.a = a[3]; }
    if (b) { cb.r = b[0]; cb.g = b[1]; cb.b = b[2]; cb.a = b[3]; }
    const gryce_ui::AnimColor r = gryce_ui::anim_color_lerp(ca, cb, t);
    out[0] = r.r; out[1] = r.g; out[2] = r.b; out[3] = r.a;
}

int GUi_AnimEaseName(int ease, char* out_buf, int cap) {
    if (!out_buf || cap <= 0) return 0;
    // 与 to_ease 的"退回 Linear"不同：名字必须忠实。越界就是没有名字，
    // 回退成 "Linear" 会让调用方把一段非法动画显示成线性动画。
    const char* name = (ease >= 0 && ease < GUI_ANIM_EASE_COUNT)
                           ? gryce_ui::anim_ease_name(static_cast<gryce_ui::AnimEase>(ease))
                           : "";
    const size_t n = std::strlen(name);
    if (n >= static_cast<size_t>(cap)) {
        std::memcpy(out_buf, name, static_cast<size_t>(cap) - 1);
        out_buf[cap - 1] = '\0';
        return cap - 1;
    }
    std::memcpy(out_buf, name, n + 1);
    return static_cast<int>(n);
}

float GUi_AnimClipDuration(const GUiAnimClip* clip) {
    return to_clip(clip).computed_duration();
}

void GUi_AnimPlay(const GUiAnimClip* clip, int restart) {
    gryce_ui::animator().play(to_clip(clip), restart != 0);
}

void GUi_AnimStop(void) { gryce_ui::animator().stop(); }
void GUi_AnimPause(void) { gryce_ui::animator().pause(); }
void GUi_AnimResume(void) { gryce_ui::animator().resume(); }
void GUi_AnimSetSpeed(float speed) { gryce_ui::animator().set_speed(speed); }
void GUi_AnimSeek(float t) { gryce_ui::animator().seek(t); }
void GUi_AnimAdvance(float dt) { gryce_ui::animator().advance(dt); }

int GUi_AnimIsPlaying(void) { return gryce_ui::animator().is_playing() ? 1 : 0; }
int GUi_AnimIsFinished(void) { return gryce_ui::animator().is_finished() ? 1 : 0; }
float GUi_AnimTime(void) { return gryce_ui::animator().time(); }

int GUi_AnimChannelCount(void) { return gryce_ui::animator().channel_count(); }

int GUi_AnimChannelName(int index, char* out_buf, int cap) {
    if (!out_buf || cap <= 0) return 0;
    const char* name = gryce_ui::animator().channel_name(index);
    const size_t n = std::strlen(name);
    if (n >= static_cast<size_t>(cap)) {
        std::memcpy(out_buf, name, static_cast<size_t>(cap) - 1);
        out_buf[cap - 1] = '\0';
        return cap - 1;
    }
    std::memcpy(out_buf, name, n + 1);
    return static_cast<int>(n);
}

int GUi_AnimChannelFloat(const char* name, float* out) {
    float v = 0.0f;
    const int ok = gryce_ui::animator().channel_float(name, &v) ? 1 : 0;
    if (out) *out = v;
    return ok;
}

int GUi_AnimChannelColor(const char* name, float out[4]) {
    gryce_ui::AnimColor c{};
    if (!gryce_ui::animator().channel_color(name, &c)) return 0;
    if (out) {
        out[0] = c.r; out[1] = c.g; out[2] = c.b; out[3] = c.a;
    }
    return 1;
}

int GUi_ShouldDispatchShortcut(void) {
    return gryce_ui::should_dispatch_shortcut() ? 1 : 0;
}

int GUi_ShortcutAdd(uint32_t key, uint32_t mods, int command) {
    return gryce_ui::shortcuts().add(key, mods, command);
}

int GUi_ShortcutFind(uint32_t key, uint32_t mods) {
    return gryce_ui::shortcuts().find(key, mods);
}

void GUi_ShortcutClear(void) {
    gryce_ui::shortcuts().clear();
}

} // extern "C"
