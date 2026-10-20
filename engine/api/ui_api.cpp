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

#include <cstddef>
#include <cstring>

#ifdef GRYCE_HAS_IMGUI
#include <imgui.h>
#endif

namespace {

bool g_in_frame = false;

#ifdef GRYCE_HAS_IMGUI
inline bool ready() { return g_in_frame && ImGui::GetCurrentContext() != nullptr; }

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

// 语义色。值取自引擎暗色主题附近，但不暴露 ImGuiCol_*，改主题不影响调用方。
ImVec4 text_color(int kind) {
    switch (kind) {
        case GUI_TEXT_DIM:    return ImVec4(0.62f, 0.64f, 0.68f, 1.0f);
        case GUI_TEXT_OK:     return ImVec4(0.45f, 0.82f, 0.52f, 1.0f);
        case GUI_TEXT_WARN:   return ImVec4(0.95f, 0.78f, 0.36f, 1.0f);
        case GUI_TEXT_ERROR:  return ImVec4(0.94f, 0.45f, 0.42f, 1.0f);
        case GUI_TEXT_ACCENT: return ImVec4(0.42f, 0.72f, 0.98f, 1.0f);
        default:              return ImVec4(0.90f, 0.91f, 0.93f, 1.0f);
    }
}
#else
inline bool ready() { return false; }
#endif

} // namespace

namespace gryce_ui {

void enter_callback() { g_in_frame = true; }
void leave_callback() { g_in_frame = false; }

} // namespace gryce_ui

extern "C" {

int GUi_InFrame(void) { return ready() ? 1 : 0; }

void GUi_GetDisplaySize(float* out_w, float* out_h) {
    if (out_w) *out_w = 0.0f;
    if (out_h) *out_h = 0.0f;
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
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::End();
#endif
}

int GUi_BeginChild(const char* id, float w, float h, int border) {
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
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndChild();
#endif
}

int GUi_BeginMenuBar(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::BeginMenuBar() ? 1 : 0;
#else
    return 0;
#endif
}

void GUi_EndMenuBar(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndMenuBar();
#endif
}

int GUi_BeginMenu(const char* label) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::BeginMenu(label ? label : "") ? 1 : 0;
#else
    (void)label;
    return 0;
#endif
}

void GUi_EndMenu(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndMenu();
#endif
}

int GUi_MenuItem(const char* label, const char* shortcut, int enabled) {
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
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::TextUnformatted(text ? text : "");
#else
    (void)text;
#endif
}

void GUi_TextColored(int color, const char* text) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    const ImVec4 c = text_color(color);
    ImGui::TextColored(c, "%s", text ? text : "");
#else
    (void)color; (void)text;
#endif
}

void GUi_Separator(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::Separator();
#endif
}

void GUi_Spacing(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::Spacing();
#endif
}

void GUi_SameLine(float spacing) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::SameLine(spacing);
#else
    (void)spacing;
#endif
}

void GUi_LabelText(const char* label, const char* text) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::LabelText(label ? label : "", "%s", text ? text : "");
#else
    (void)label; (void)text;
#endif
}

int GUi_Button(const char* label) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::Button(label ? label : "") ? 1 : 0;
#else
    (void)label;
    return 0;
#endif
}

int GUi_ButtonSize(const char* label, float w, float h) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::Button(label ? label : "", ImVec2(w, h)) ? 1 : 0;
#else
    (void)label; (void)w; (void)h;
    return 0;
#endif
}

int GUi_SmallButton(const char* label) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::SmallButton(label ? label : "") ? 1 : 0;
#else
    (void)label;
    return 0;
#endif
}

int GUi_Selectable(const char* label, int selected) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::Selectable(label ? label : "", selected != 0) ? 1 : 0;
#else
    (void)label; (void)selected;
    return 0;
#endif
}

void GUi_SetItemAllowOverlap(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::SetNextItemAllowOverlap();
#endif
}

int GUi_Checkbox(const char* label, int* inout_value) {
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

int GUi_DragFloat(const char* label, float* inout_value, float speed) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !inout_value) return 0;
    return ImGui::DragFloat(label ? label : "", inout_value, speed) ? 1 : 0;
#else
    (void)label; (void)inout_value; (void)speed;
    return 0;
#endif
}

int GUi_DragFloat2(const char* label, float v[2], float speed) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !v) return 0;
    return ImGui::DragFloat2(label ? label : "", v, speed) ? 1 : 0;
#else
    (void)label; (void)v; (void)speed;
    return 0;
#endif
}

int GUi_DragFloat3(const char* label, float v[3], float speed) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !v) return 0;
    return ImGui::DragFloat3(label ? label : "", v, speed) ? 1 : 0;
#else
    (void)label; (void)v; (void)speed;
    return 0;
#endif
}

int GUi_DragFloat4(const char* label, float v[4], float speed) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !v) return 0;
    return ImGui::DragFloat4(label ? label : "", v, speed) ? 1 : 0;
#else
    (void)label; (void)v; (void)speed;
    return 0;
#endif
}

int GUi_ColorEdit4(const char* label, float v[4]) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !v) return 0;
    return ImGui::ColorEdit4(label ? label : "", v, ImGuiColorEditFlags_Float) ? 1 : 0;
#else
    (void)label; (void)v;
    return 0;
#endif
}

int GUi_InputText(const char* label, char* buf, int buf_size) {
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
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !current || !items || count <= 0) return 0;
    return ImGui::Combo(label ? label : "", current, items, count) ? 1 : 0;
#else
    (void)label; (void)current; (void)items; (void)count;
    return 0;
#endif
}

void GUi_ProgressBar(float fraction, float w, float h, const char* overlay) {
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
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::TreePop();
#endif
}

int GUi_CollapsingHeader(const char* label, int default_open) {
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
    ImGui::Image(static_cast<ImTextureID>(tex), ImVec2(w, h));
#else
    (void)tex; (void)w; (void)h;
#endif
}

// ---------------------------------------------------------------------------
// 弹出菜单
// ---------------------------------------------------------------------------

void GUi_OpenPopupOnItemClick(const char* id, int mouse_button) {
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
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !id) return;
    ImGui::OpenPopup(id);
#else
    (void)id;
#endif
}

int GUi_BeginPopup(const char* id) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready() || !id) return 0;
    return ImGui::BeginPopup(id) ? 1 : 0;
#else
    (void)id;
    return 0;
#endif
}

void GUi_EndPopup(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndPopup();
#endif
}

void GUi_CloseCurrentPopup(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::CloseCurrentPopup();
#endif
}

// ---------------------------------------------------------------------------
// 拖放
// ---------------------------------------------------------------------------

int GUi_BeginDragDropSource(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    // SourceAllowNullID：图标的条目（比如只带 ##id 的 Selectable）也要能拖。
    return ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID) ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_SetDragDropPayloadString(const char* type, const char* text) {
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
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndDragDropSource();
#endif
}

int GUi_BeginDragDropTarget(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::BeginDragDropTarget() ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_AcceptDragDropPayloadString(const char* type, char* out_buf, int buf_size) {
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
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::EndDragDropTarget();
#endif
}

// ---------------------------------------------------------------------------
// 布局 / 命中查询
// ---------------------------------------------------------------------------

void GUi_SetCursorPos(float x, float y) {
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
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    const ImVec2 s = ImGui::GetContentRegionAvail();
    if (out_w) *out_w = s.x;
    if (out_h) *out_h = s.y;
#endif
}

void GUi_SetNextItemWidth(float w) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    ImGui::SetNextItemWidth(w);
#else
    (void)w;
#endif
}

float GUi_GetTextLineHeight(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0.0f;
    return ImGui::GetTextLineHeight();
#else
    return 0.0f;
#endif
}

float GUi_GetFrameHeight(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0.0f;
    return ImGui::GetFrameHeight();
#else
    return 0.0f;
#endif
}

float GUi_CalcTextWidth(const char* text) {
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
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsItemHovered() ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_IsItemClicked(int mouse_button) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsItemClicked(mouse_button == GUI_MOUSE_RIGHT ? 1 : 0) ? 1 : 0;
#else
    (void)mouse_button;
    return 0;
#endif
}

int GUi_IsItemActive(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsItemActive() ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_IsAnyItemActive(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsAnyItemActive() ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_GetModifiers(void) {
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
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return;
    const ImVec2 p = ImGui::GetIO().MousePos;
    if (out_x) *out_x = p.x;
    if (out_y) *out_y = p.y;
#endif
}

float GUi_MouseDeltaX(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0.0f;
    return ImGui::GetIO().MouseDelta.x;
#else
    return 0.0f;
#endif
}

float GUi_MouseDeltaY(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0.0f;
    return ImGui::GetIO().MouseDelta.y;
#else
    return 0.0f;
#endif
}

int GUi_IsMouseDown(int button) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsMouseDown(button) ? 1 : 0;
#else
    (void)button;
    return 0;
#endif
}

int GUi_IsMouseClicked(int button) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsMouseClicked(button) ? 1 : 0;
#else
    (void)button;
    return 0;
#endif
}

int GUi_IsMouseReleased(int button) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::IsMouseReleased(button) ? 1 : 0;
#else
    (void)button;
    return 0;
#endif
}

int GUi_WantCaptureMouse(void) {
#ifdef GRYCE_HAS_IMGUI
    if (!ready()) return 0;
    return ImGui::GetIO().WantCaptureMouse ? 1 : 0;
#else
    return 0;
#endif
}

int GUi_WantCaptureKeyboard(void) {
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

} // extern "C"
