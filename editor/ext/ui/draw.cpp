// ===========================================================================
// editor/ext/ui/draw.cpp — 界面布局与面板绘制
//
// 只用引擎的 UI C 接口（api/ui_api.h），不 include imgui.h。这个文件里
// **一条引擎 API 都不许调**（除了 GUi_*）：它跑在渲染器的锁内，任何
// GEntity_* / GScene_* / GEdit_* 都会死锁。要改东西一律入队 Intent。
//
// 布局：一整块主窗口 + 固定分区（左层级 / 中视口 / 右检视 / 下资产），
// 不用 docking。docking 的布局会随 imgui.ini 漂移，集成测试与截图比对就
// 不可复现了；而且面板顺序对编辑器来说本来就是固定的。
// ===========================================================================

#include "ui/ui_internal.h"

#include "ui/intent.h"

#include "panel/model.h"
#include "panel/panel.h"
#include "session/session.h"
#include "view/gizmo.h"
#include "view/viewport.h"

#include "api/ui_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace gryce::editor::ui {

// 意图层的东西（Intent / kXxx / push / push_select）在本文件里用得极密，
// 逐个 using 只会让开头堆成一片，这里整层引入。
using namespace intent;

// 面板模型里的名字都是"编辑器概念"，这里统一引入，避免每处都写 panel::。
using panel::assets;
using panel::hierarchy;
using panel::inspector;
using panel::invalidate_assets;
using panel::stats;
using panel::AssetItem;
using panel::AssetList;
using panel::ComponentData;
using panel::HierarchyNode;
using panel::InspectorData;
using panel::PropertyValue;
using panel::StatsData;
using panel::kBool;
using panel::kColor;
using panel::kDouble;
using panel::kFloat;
using panel::kInt;
using panel::kQuat;
using panel::kString;
using panel::kVec2;
using panel::kVec3;
using panel::kVec3i;
using panel::kVec4;

namespace {

constexpr float kToolbarHeight = 34.0f;
constexpr float kLeftWidth = 236.0f;
constexpr float kRightWidth = 306.0f;
constexpr float kBottomHeight = 168.0f;
constexpr float kIndentStep = 13.0f;

const char* const kDisplayModes[] = {"Shaded", "Wireframe"};
const char* const kGizmoNames[] = {"Move", "Rotate", "Scale"};
const char* const kSpaceNames[] = {"World", "Local"};

// ---------------------------------------------------------------------------
// 旋转：界面上用欧拉角（人看得懂），引擎里存四元数。
// 顺序取 X→Y→Z（与大多数编辑器默认一致）；只在这个面板里用，不往引擎里塞。
// ---------------------------------------------------------------------------

constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;
constexpr float kRad2Deg = 180.0f / 3.14159265358979323846f;

void quat_to_euler(const float q[4], float out_deg[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];

    const float sinr = 2.0f * (w * x + y * z);
    const float cosr = 1.0f - 2.0f * (x * x + y * y);
    const float rx = std::atan2(sinr, cosr);

    float sinp = 2.0f * (w * y - z * x);
    if (sinp > 1.0f) sinp = 1.0f;
    if (sinp < -1.0f) sinp = -1.0f;
    const float ry = std::asin(sinp);

    const float siny = 2.0f * (w * z + x * y);
    const float cosy = 1.0f - 2.0f * (y * y + z * z);
    const float rz = std::atan2(siny, cosy);

    out_deg[0] = rx * kRad2Deg;
    out_deg[1] = ry * kRad2Deg;
    out_deg[2] = rz * kRad2Deg;
}

void euler_to_quat(const float deg[3], float out[4]) {
    const float hx = deg[0] * kDeg2Rad * 0.5f;
    const float hy = deg[1] * kDeg2Rad * 0.5f;
    const float hz = deg[2] * kDeg2Rad * 0.5f;
    const float cx = std::cos(hx), sx = std::sin(hx);
    const float cy = std::cos(hy), sy = std::sin(hy);
    const float cz = std::cos(hz), sz = std::sin(hz);

    out[3] = cx * cy * cz + sx * sy * sz;
    out[0] = sx * cy * cz - cx * sy * sz;
    out[1] = cx * sy * cz + sx * cy * sz;
    out[2] = cx * cy * sz - sx * sy * cz;
}

// ---------------------------------------------------------------------------
// 通用小工具
// ---------------------------------------------------------------------------

Intent make(int kind) {
    Intent it;
    it.kind = kind;
    return it;
}

// 一次连续拖拽合成一步撤销：第一个改动开分组，最后一个控件失活时收尾。
void ensure_edit_open(DrawState& st, const char* label) {
    if (st.edit_open) return;
    Intent it = make(kBeginEdit);
    it.a = label;
    push(it);
    st.edit_open = true;
}

void close_edit_if_idle(DrawState& st) {
    if (!st.edit_open) return;
    // IsAnyItemActive 为真说明还有滑块在被拖 / 输入框还在焦点上，
    // 这时候收分组会把"拖一半"的状态截断成一步。
    if (GUi_IsAnyItemActive()) return;
    push(make(kEndEdit));
    st.edit_open = false;
}

void toolbar_gap() {
    GUi_SameLine(0);
    GUi_TextColored(GUI_TEXT_DIM, "|");
    GUi_SameLine(0);
}

// ---------------------------------------------------------------------------
// 菜单栏 / 工具栏
// ---------------------------------------------------------------------------

void draw_menu_bar(DrawState& st) {
    if (!GUi_BeginMenuBar()) return;

    if (GUi_BeginMenu("File")) {
        if (GUi_MenuItem("Save", "Ctrl+S", 1)) push(make(kSave));
        GUi_Separator();
        GUi_TextColored(GUI_TEXT_DIM, "Close the window to quit");
        GUi_EndMenu();
    }

    if (GUi_BeginMenu("Edit")) {
        if (GUi_MenuItem("Undo", "Ctrl+Z", stats().undo > 0)) push(make(kUndo));
        if (GUi_MenuItem("Redo", "Ctrl+Y", stats().redo > 0)) push(make(kRedo));
        GUi_Separator();
        if (GUi_MenuItem("Delete", "Del", stats().selection_count > 0)) push(make(kDestroy));
        GUi_EndMenu();
    }

    if (GUi_BeginMenu("Create")) {
        if (GUi_MenuItem("Empty Entity", nullptr, 1)) {
            Intent it = make(kCreate);
            it.a = "Entity";
            push(it);
        }
        if (GUi_MenuItem("Child of Selection", nullptr, stats().selected > 0)) {
            Intent it = make(kCreate);
            it.a = "Entity";
            it.other = stats().selected;
            push(it);
        }
        GUi_EndMenu();
    }

    if (GUi_BeginMenu("View")) {
        if (GUi_MenuItem("Shaded", nullptr, 1)) {
            Intent it = make(kDisplayMode);
            it.a = "Shaded";
            push(it);
        }
        if (GUi_MenuItem("Wireframe", nullptr, 1)) {
            Intent it = make(kDisplayMode);
            it.a = "Wireframe";
            push(it);
        }
        GUi_EndMenu();
    }

    if (GUi_BeginMenu("Help")) {
        GUi_TextColored(GUI_TEXT_DIM, "W/E/R   move / rotate / scale");
        GUi_TextColored(GUI_TEXT_DIM, "F       frame selected");
        GUi_TextColored(GUI_TEXT_DIM, "Del     delete selection");
        GUi_EndMenu();
    }

    GUi_EndMenuBar();
}

void draw_toolbar(DrawState& st) {
    // 固定高度的子区域：工具栏按钮行数变了也不会把下面的面板挤歪。
    GUi_BeginChild("##toolbar", 0, kToolbarHeight, 0);

    if (GUi_Button("Save")) push(make(kSave));
    toolbar_gap();

    if (GUi_Button("Undo")) push(make(kUndo));
    GUi_SameLine(0);
    if (GUi_Button("Redo")) push(make(kRedo));
    toolbar_gap();

    const bool playing = stats().playing;
    if (GUi_Button(playing ? "Pause" : "Play")) {
        Intent it = make(kPlay);
        it.flag = playing ? 0 : 1;
        push(it);
    }
    GUi_SameLine(0);
    if (GUi_Button("Step")) push(make(kStep));
    toolbar_gap();

    for (int i = 0; i < 3; ++i) {
        // 用按钮模拟单选：当前模式按下去是"亮"的（靠文本前缀区分，
        // 因为 C 接口没有 push/pop 按钮色）。
        const bool on = gizmo::operation() == i;
        char label[32];
        std::snprintf(label, sizeof(label), "%s%s", on ? "[x] " : "", kGizmoNames[i]);
        if (GUi_SmallButton(label)) {
            Intent it = make(kGizmoMode);
            it.flag = i;
            it.other = gizmo::space();
            push(it);
        }
        GUi_SameLine(0);
    }
    toolbar_gap();

    {
        const bool on = gizmo::space() == 1;
        if (GUi_SmallButton(on ? "[x] Local" : "[ ] Local")) {
            Intent it = make(kGizmoMode);
            it.flag = gizmo::operation();
            it.other = on ? 0 : 1;
            push(it);
        }
    }
    toolbar_gap();

    int mode = 0;
    const char* current = view::display_mode();
    for (int i = 0; i < 2; ++i) {
        if (std::strcmp(current, kDisplayModes[i]) == 0) mode = i;
    }
    GUi_SetNextItemWidth(120.0f);
    if (GUi_Combo("##display", &mode, kDisplayModes, 2)) {
        Intent it = make(kDisplayMode);
        it.a = kDisplayModes[mode];
        push(it);
    }

    GUi_EndChild();
}

// ---------------------------------------------------------------------------
// 层级树
// ---------------------------------------------------------------------------

void draw_nodes(DrawState& st, const std::vector<HierarchyNode>& nodes,
                GEntityHandle selected) {
    for (const HierarchyNode& node : nodes) {
        float cx = 0.0f;
        float cy = 0.0f;
        GUi_GetCursorPos(&cx, &cy);
        if (node.depth > 0) {
            // 没有 Indent/Unindent 接口，直接按深度偏移光标。
            GUi_SetCursorPos(cx + static_cast<float>(node.depth) * kIndentStep, cy);
        }

        const bool has_children = !node.children.empty();
        const bool open = is_expanded(node.handle);

        if (has_children) {
            char arrow[24];
            std::snprintf(arrow, sizeof(arrow), "%s##a%lld", open ? "v" : ">",
                          static_cast<long long>(node.handle));
            if (GUi_SmallButton(arrow)) toggle_expanded(node.handle);
            GUi_SameLine(4.0f);
        }

        if (st.rename_requested && st.rename_handle == node.handle) {
            GUi_SetNextItemWidth(-1.0f);
            if (st.rename_focus) {
                GUi_SetKeyboardFocusHere();
                st.rename_focus = false;
            }
            GUi_InputText("##rename", st.rename_buf, static_cast<int>(sizeof(st.rename_buf)));
            // 回车或失焦后提交；中途按 Esc 放弃（值没变时不提交，等于放弃）。
            if (GUi_IsItemDeactivatedAfterEdit()) {
                Intent it = make(kRename);
                it.handle = node.handle;
                it.a = st.rename_buf;
                push(it);
                st.rename_requested = false;
                st.rename_handle = 0;
            }
        } else {
            char label[192];
            std::snprintf(label, sizeof(label), "%s##h%lld", node.name.c_str(),
                          static_cast<long long>(node.handle));
            if (GUi_Selectable(label, node.handle == selected)) {
                push_select(kSelect, node.handle);
            }

            char popup[48];
            std::snprintf(popup, sizeof(popup), "node_menu##%lld",
                          static_cast<long long>(node.handle));
            GUi_OpenPopupOnItemClick(popup, GUI_MOUSE_RIGHT);
            if (GUi_BeginPopup(popup)) {
                if (GUi_MenuItem("Rename", "F2", 1)) {
                    st.rename_requested = true;
                    st.rename_focus = true;
                    st.rename_handle = node.handle;
                    std::snprintf(st.rename_buf, sizeof(st.rename_buf), "%s",
                                  node.name.c_str());
                }
                if (GUi_MenuItem("Create Child", nullptr, 1)) {
                    Intent it = make(kCreate);
                    it.a = "Entity";
                    it.other = node.handle;
                    push(it);
                }
                if (GUi_MenuItem("Focus", "F", 1)) {
                    // 视角调整在 JS 侧做（它持有相机状态），这里只请求选中。
                    push_select(kSelect, node.handle);
                }
                GUi_Separator();
                if (GUi_MenuItem("Delete", "Del", 1)) {
                    Intent it = make(kDestroy);
                    it.handle = node.handle;
                    push(it);
                }
                GUi_EndPopup();
            }
        }

        if (has_children && open) draw_nodes(st, node.children, selected);
    }
}

void draw_hierarchy(DrawState& st) {
    GUi_TextColored(GUI_TEXT_DIM, "Hierarchy");
    GUi_Separator();

    const GEntityHandle selected = session::selection().primary;
    draw_nodes(st, hierarchy(), selected);

    if (hierarchy().empty()) {
        GUi_TextColored(GUI_TEXT_DIM, "(empty scene)");
    }
}

// ---------------------------------------------------------------------------
// 检视器
// ---------------------------------------------------------------------------

void draw_inspector(DrawState& st) {
    GUi_TextColored(GUI_TEXT_DIM, "Inspector");
    GUi_Separator();

    const InspectorData& in = inspector();
    if (!in.valid) {
        GUi_TextColored(GUI_TEXT_DIM, "nothing selected");
        return;
    }

    // 名字：回车/失焦才提交，逐个字符提交会把一次改名写成十几个撤销步。
    if (st.name_handle != in.handle) {
        st.name_handle = in.handle;
        std::snprintf(st.name_buf, sizeof(st.name_buf), "%s", in.name.c_str());
    }
    GUi_SetNextItemWidth(-1.0f);
    GUi_InputText("##name", st.name_buf, static_cast<int>(sizeof(st.name_buf)));
    if (GUi_IsItemDeactivatedAfterEdit()) {
        Intent it = make(kRename);
        it.handle = in.handle;
        it.a = st.name_buf;
        push(it);
    }

    // 本地副本只在没有控件处于编辑态时同步；否则拖拽中的值会被上一帧的
    // 引擎值覆盖，手感是"拖一下弹回去"。
    if (st.transform_handle != in.handle || !GUi_IsAnyItemActive()) {
        st.transform_handle = in.handle;
        st.pos_buf[0] = in.position[0];
        st.pos_buf[1] = in.position[1];
        st.pos_buf[2] = in.position[2];
        st.scale_buf[0] = in.scale[0];
        st.scale_buf[1] = in.scale[1];
        st.scale_buf[2] = in.scale[2];
        quat_to_euler(in.rotation, st.euler_buf);
    }

    GUi_Separator();
    GUi_TextColored(GUI_TEXT_DIM, "Transform");

    GUi_SetNextItemWidth(-1.0f);
    if (GUi_DragFloat3("Position", st.pos_buf, 0.02f)) {
        ensure_edit_open(st, "transform");
        Intent it = make(kSetPosition);
        it.handle = in.handle;
        it.f0 = st.pos_buf[0];
        it.f1 = st.pos_buf[1];
        it.f2 = st.pos_buf[2];
        push(it);
    }

    GUi_SetNextItemWidth(-1.0f);
    if (GUi_DragFloat3("Rotation", st.euler_buf, 0.5f)) {
        ensure_edit_open(st, "transform");
        float q[4] = {0, 0, 0, 1};
        euler_to_quat(st.euler_buf, q);
        Intent it = make(kSetRotation);
        it.handle = in.handle;
        it.f0 = q[0];
        it.f1 = q[1];
        it.f2 = q[2];
        it.f3 = q[3];
        push(it);
    }

    GUi_SetNextItemWidth(-1.0f);
    if (GUi_DragFloat3("Scale", st.scale_buf, 0.02f)) {
        // 引擎拒收 0 缩放（奇异矩阵会让整棵子树的法线变成 NaN），
        // 界面上直接夹住，避免"拖到 0 就消失"这种莫名其妙的故障。
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(st.scale_buf[i]) < 0.001f) {
                st.scale_buf[i] = st.scale_buf[i] < 0.0f ? -0.001f : 0.001f;
            }
        }
        ensure_edit_open(st, "transform");
        Intent it = make(kSetScale);
        it.handle = in.handle;
        it.f0 = st.scale_buf[0];
        it.f1 = st.scale_buf[1];
        it.f2 = st.scale_buf[2];
        push(it);
    }

    GUi_Separator();

    for (const ComponentData& comp : in.components) {
        if (!GUi_CollapsingHeader(comp.name.c_str(), 1)) continue;

        for (const PropertyValue& p : comp.properties) {
            // 标量 / 布尔可编辑，其余只读：GEdit_SetProperty 只接受单个数值，
            // 想改 vec3 得走另一条命令面（暂未提供），这里如实显示而不是
            // 给一个拖了没反应的控件。
            switch (p.type) {
                case kFloat:
                case kDouble: {
                    float v = static_cast<float>(p.number);
                    GUi_SetNextItemWidth(-1.0f);
                    if (GUi_DragFloat(p.name.c_str(), &v, 0.01f)) {
                        ensure_edit_open(st, "property");
                        Intent it = make(kSetPropertyNumber);
                        it.handle = in.handle;
                        it.hash = comp.hash;
                        it.a = p.name;
                        it.number = static_cast<double>(v);
                        push(it);
                    }
                    break;
                }
                case kInt: {
                    float v = static_cast<float>(p.number);
                    GUi_SetNextItemWidth(-1.0f);
                    if (GUi_DragFloat(p.name.c_str(), &v, 1.0f)) {
                        ensure_edit_open(st, "property");
                        Intent it = make(kSetPropertyNumber);
                        it.handle = in.handle;
                        it.hash = comp.hash;
                        it.a = p.name;
                        it.number = static_cast<double>(std::llround(v));
                        push(it);
                    }
                    break;
                }
                case kBool: {
                    int v = p.boolean ? 1 : 0;
                    if (GUi_Checkbox(p.name.c_str(), &v)) {
                        Intent it = make(kSetPropertyBool);
                        it.handle = in.handle;
                        it.hash = comp.hash;
                        it.a = p.name;
                        it.flag = v;
                        push(it);
                    }
                    break;
                }
                case kString:
                    GUi_LabelText(p.name.c_str(), p.text.c_str());
                    break;
                case kVec2:
                case kVec3:
                case kVec3i:
                case kVec4:
                case kQuat:
                case kColor: {
                    char buf[160] = {};
                    int used = 0;
                    for (int i = 0; i < p.count && used < static_cast<int>(sizeof(buf)) - 1; ++i) {
                        const int n = std::snprintf(buf + used,
                                                    sizeof(buf) - static_cast<std::size_t>(used),
                                                    i ? " %.3f" : "%.3f",
                                                    static_cast<double>(p.vec[i]));
                        if (n <= 0) break;
                        used += n;
                    }
                    GUi_LabelText(p.name.c_str(), buf);
                    break;
                }
                default:
                    GUi_LabelText(p.name.c_str(), "-");
                    break;
            }
        }
        GUi_Separator();
    }
}

// ---------------------------------------------------------------------------
// 视口
// ---------------------------------------------------------------------------

void draw_viewport(DrawState& st, float w, float h) {
    if (st.viewport_tex == 0) {
        GUi_TextColored(GUI_TEXT_WARN, "viewport texture unavailable");
        st.view = ViewRect{};
        return;
    }
    if (w < 8.0f) w = 8.0f;
    if (h < 8.0f) h = 8.0f;

    GUi_Image(st.viewport_tex, w, h);

    float x = 0.0f, y = 0.0f, iw = 0.0f, ih = 0.0f;
    GUi_LastItemRect(&x, &y, &iw, &ih);
    if (iw < 1.0f || ih < 1.0f) {
        st.view = ViewRect{};
        return;
    }
    st.view = ViewRect{x, y, iw, ih};
    st.last_w = static_cast<int>(iw + 0.5f);
    st.last_h = static_cast<int>(ih + 0.5f);

    // 采样鼠标：ImGui 的鼠标状态只在帧内有效，而拾取要加锁，所以这里只
    // 记录，真正的命中计算放到下一次 tick。
    float mx = 0.0f, my = 0.0f;
    GUi_MousePos(&mx, &my);
    ViewInput in;
    in.hovered = GUi_IsItemHovered() != 0;
    in.clicked = GUi_IsMouseClicked(GUI_MOUSE_LEFT) != 0;
    in.down = GUi_IsMouseDown(GUI_MOUSE_LEFT) != 0;
    in.released = GUi_IsMouseReleased(GUI_MOUSE_LEFT) != 0;
    in.x = mx - x;
    in.y = my - y;
    in.dx = GUi_MouseDeltaX();
    in.dy = GUi_MouseDeltaY();
    st.input = in;

    // 坐标提示：画在视口左下角，方便对齐"看到的位置"与"点到的位置"。
    if (in.hovered) {
        char tip[96];
        std::snprintf(tip, sizeof(tip), "%d x %d  (%.0f, %.0f)", st.last_w, st.last_h,
                      static_cast<double>(in.x), static_cast<double>(in.y));
        GUi_TextColored(GUI_TEXT_DIM, tip);
    }
}

// ---------------------------------------------------------------------------
// 资产 / 统计
// ---------------------------------------------------------------------------

void draw_assets(DrawState& st) {
    GUi_TextColored(GUI_TEXT_DIM, "Assets");
    GUi_Separator();

    GUi_SetNextItemWidth(-90.0f);
    GUi_InputText("##dir", st.dir_buf, static_cast<int>(sizeof(st.dir_buf)));
    GUi_SameLine(0);
    if (GUi_Button("Refresh")) {
        // 只清缓存，不碰引擎（扫盘发生在下一次 tick 的 rebuild_assets）。
        panel::invalidate_assets();
    }

    const AssetList& list = assets();
    char header[160];
    std::snprintf(header, sizeof(header), "%s  (%d files)", list.dir.c_str(),
                  static_cast<int>(list.items.size()));
    GUi_TextColored(GUI_TEXT_DIM, header);

    GUi_BeginChild("##asset_list", 0, 0, 0);
    for (std::size_t i = 0; i < list.items.size(); ++i) {
        const AssetItem& item = list.items[i];
        const bool selected = st.asset_sel == static_cast<GEntityHandle>(i + 1);
        char label[480];
        std::snprintf(label, sizeof(label), "%s  [%s]##asset%llu", item.name.c_str(),
                      item.kind.c_str(), static_cast<unsigned long long>(i));

        if (GUi_Selectable(label, selected)) {
            st.asset_sel = static_cast<GEntityHandle>(i + 1);
            std::snprintf(st.asset_sel_path, sizeof(st.asset_sel_path), "%s",
                          item.path.c_str());
        }

        char popup[64];
        std::snprintf(popup, sizeof(popup), "asset_menu##%llu",
                      static_cast<unsigned long long>(i));
        GUi_OpenPopupOnItemClick(popup, GUI_MOUSE_RIGHT);
        if (GUi_BeginPopup(popup)) {
            if (GUi_MenuItem("Instantiate", nullptr, 1)) {
                Intent it = make(kImportAsset);
                it.a = item.path;
                push(it);
            }
            GUi_EndPopup();
        }
    }
    if (list.items.empty()) {
        GUi_TextColored(GUI_TEXT_DIM, list.scanned ? "(no assets)" : "(directory not found)");
    }
    GUi_EndChild();

    if (st.asset_sel_path[0]) {
        if (GUi_Button("Instantiate")) {
            Intent it = make(kImportAsset);
            it.a = st.asset_sel_path;
            push(it);
        }
    }
}

void draw_stats() {
    const StatsData& s = stats();
    GUi_TextColored(GUI_TEXT_DIM, "Stats");
    GUi_Separator();

    char line[192];
    std::snprintf(line, sizeof(line), "fps %.1f   gpu %.2f ms", static_cast<double>(s.fps),
                  static_cast<double>(s.gpu_ms));
    GUi_Text(line);

    std::snprintf(line, sizeof(line), "entities %d   selected %d", s.entities,
                  s.selection_count);
    GUi_Text(line);

    std::snprintf(line, sizeof(line), "undo %d   redo %d", s.undo, s.redo);
    GUi_Text(line);

    GUi_TextColored(s.dirty ? GUI_TEXT_WARN : GUI_TEXT_DIM,
                    s.dirty ? "modified" : "saved");
    GUi_TextColored(s.playing ? GUI_TEXT_OK : GUI_TEXT_DIM,
                    s.playing ? (s.paused ? "playing (paused)" : "playing")
                              : "editing");
}

} // namespace

void draw_all(void* user) {
    (void)user;

    DrawState& st = state();
    if (!st.enabled) return;

    ++st.frames;
    // 引擎没接管 ImGui（WebGL 构建 / 关掉 ImGui 的构建）时 GUi_* 全是空操作，
    // 这里提前退出，免得把"没画"算进 draws。
    if (!GUi_InFrame()) return;
    ++st.draws;

    float w = 0.0f;
    float h = 0.0f;
    GUi_GetDisplaySize(&w, &h);
    if (w < 64.0f || h < 64.0f) return;

    const int flags = GUI_WIN_NO_TITLE_BAR | GUI_WIN_NO_RESIZE | GUI_WIN_NO_MOVE |
                      GUI_WIN_NO_COLLAPSE | GUI_WIN_NO_SAVED_SETTINGS |
                      GUI_WIN_NO_SCROLLBAR | GUI_WIN_NO_BRING_TO_FRONT | GUI_WIN_MENU_BAR;

    if (!GUi_BeginWindow("##gryce_editor", 0.0f, 0.0f, w, h, flags)) {
        GUi_EndWindow();
        return;
    }

    draw_menu_bar(st);
    draw_toolbar(st);

    float avail_w = 0.0f;
    float avail_h = 0.0f;
    GUi_GetContentAvail(&avail_w, &avail_h);

    // 底部固定高度，剩下的给三栏。avail_h 已经扣掉了菜单栏与工具栏，
    // 所以工具栏换行也不会让底栏被挤出去。
    float body_h = avail_h - kBottomHeight - 8.0f;
    if (body_h < 120.0f) body_h = 120.0f;

    float center_w = avail_w - kLeftWidth - kRightWidth - 16.0f;
    if (center_w < 120.0f) center_w = 120.0f;

    GUi_BeginChild("##left", kLeftWidth, body_h, 1);
    draw_hierarchy(st);
    GUi_EndChild();

    GUi_SameLine(0);
    GUi_BeginChild("##center", center_w, body_h, 1);
    // 减掉边框与坐标提示占的高度，贴图才不会超出子区域。
    draw_viewport(st, center_w - 12.0f, body_h - 28.0f);
    GUi_EndChild();

    GUi_SameLine(0);
    GUi_BeginChild("##right", kRightWidth, body_h, 1);
    draw_inspector(st);
    GUi_EndChild();

    GUi_BeginChild("##bottom", 0, kBottomHeight, 1);
    GUi_BeginChild("##bottom_assets", avail_w * 0.58f, 0, 0);
    draw_assets(st);
    GUi_EndChild();
    GUi_SameLine(0);
    GUi_BeginChild("##bottom_stats", 0, 0, 0);
    draw_stats();
    GUi_EndChild();
    GUi_EndChild();

    GUi_EndWindow();

    // 捕获状态要在画完之后取：有没有控件被点中/聚焦，是画的过程中定下来的。
    st.capture_keyboard = GUi_WantCaptureKeyboard() != 0;
    st.capture_mouse = GUi_WantCaptureMouse() != 0;
    st.pointer_in_view = st.input.hovered;

    // 拖拽结束、输入框失焦之后才收撤销分组，否则一次拖动会被拆成几十步。
    close_edit_if_idle(st);
}

} // namespace gryce::editor::ui
