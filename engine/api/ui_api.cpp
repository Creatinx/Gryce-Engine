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
#include "api/ui_op_queue.h"
#include "api/ui_op_queue_internal.h"
#include "ui/ui_component.h"
#include "ui/ui_id.h"
#include "ui/ui_input.h"
#include "ui/ui_layout.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"

#include <cstddef>
#include <cstring>

#ifdef GRYCE_HAS_IMGUI
#include <imgui.h>
#endif

namespace {

bool g_in_frame = false;

#ifdef GRYCE_HAS_IMGUI
// 本后端的纹理是否需要翻 V 才与 ImGui 的 uv 约定一致（见 ui_api_internal.h）。
// 默认 false = D3D12 / Vulkan 约定。render_api.cpp 在建好 ImGui 后端后会覆盖。
bool g_texture_v_flip = false;
#endif

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

// 语义文本色取自主题 token（ui_theme.h）：换主题时这些颜色一起换，
// 不再有散落在控件实现里的硬编码色值。无 ImGui 时同样可算（token 是纯数据）。
ImVec4 text_color(int kind) {
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
    const uint32_t c = gryce_ui::color(token);
    return ImVec4(static_cast<float>((c >> 24) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 16) & 0xFF) / 255.0f,
                  static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(c & 0xFF) / 255.0f);
}
#else
inline bool ready() { return false; }
#endif

} // namespace

namespace gryce_ui {

void enter_callback() { g_in_frame = true; }
void leave_callback() { g_in_frame = false; }

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
static_assert(GUI_THEME_METRIC_ROW_H == static_cast<int>(gryce_ui::THEME_METRIC_ROW_H),
              "theme metric token mismatch");
static_assert(GUI_THEME_METRIC_COUNT == static_cast<int>(gryce_ui::THEME_METRIC_COUNT),
              "theme metric token count mismatch");

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
