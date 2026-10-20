// ===========================================================================
// editor/ext/view/gizmo.cpp — Gizmo 交互实现
// ===========================================================================

#include "view/gizmo.h"

#include "api/render_api.h"

#include "common/json_writer.h"

namespace gryce::editor::gizmo {

namespace {

int g_axis = -1;
bool g_dragging = false;
float g_accumulated = 0.0f;   // 本次拖拽累计像素，UI 显示"拖了多远"

// 与引擎保持一致的模式副本（默认值与 GRender 的默认值相同）。
// UI 回调里要靠它画高亮，而 GRender_GetGizmo* 会加锁。
int g_operation = 0;
int g_space = 0;

const char* axis_name(int axis) {
    switch (axis) {
        case 0: return "X";
        case 1: return "Y";
        case 2: return "Z";
        case 3: return "Center";
        default: return "-";
    }
}

} // namespace

void reset() {
    g_axis = -1;
    g_dragging = false;
    g_accumulated = 0.0f;
    g_operation = 0;
    g_space = 0;
}

int enable(int enabled) {
    GRender_SetGizmoEnabled(enabled != 0);
    return 1;
}

int set_mode(int operation, int space) {
    // 引擎只认 0/1/2 与 0/1，越界值会被当成平移/世界空间，静默走错模式。
    if (operation < 0 || operation > 2) return 0;
    if (space < 0 || space > 1) return 0;
    GRender_SetGizmoMode(operation, space);
    g_operation = operation;
    g_space = space;
    return 1;
}

int operation() { return g_operation; }
int space() { return g_space; }

std::string mode_json() {
    Json j;
    j.begin_object();
    j.kv("operation", GRender_GetGizmoOperation());
    j.kv("space", GRender_GetGizmoSpace());
    j.kv("enabled", GRender_IsGizmoEnabled());
    j.kv("axis", g_axis);
    j.kv("axisName", axis_name(g_axis));
    j.kv("dragging", g_dragging);
    j.kv("pixels", g_accumulated);
    j.end_object();
    return j.str();
}

int hit_test(float sx, float sy) {
    const int axis = GRender_GizmoHitTest(sx, sy);
    // 悬停态也记下来：UI 要把光标换成对应轴的双向箭头。
    g_axis = axis;
    return axis;
}

int begin_drag(int axis) {
    // 只有 0/1/2 是三轴；3 是缩放中心手柄（等比缩放），也允许。
    if (axis < 0 || axis > 3) return 0;
    if (g_dragging) return 0;   // 重复 Begin 会把引擎的分组标志顶掉

    g_axis = axis;
    g_dragging = true;
    g_accumulated = 0.0f;
    GRender_GizmoBeginDrag(axis);
    return 1;
}

int drag(float delta_px) {
    if (!g_dragging) return 0;
    // 单帧位移超过半屏基本是鼠标瞬移（切窗口回来、远程桌面），
    // 按它换算会把物体甩到天上去。夹住而不是丢弃：丢掉会让手感断。
    const float limit = 400.0f;
    if (delta_px > limit) delta_px = limit;
    if (delta_px < -limit) delta_px = -limit;

    g_accumulated += delta_px;
    return GRender_GizmoDrag(delta_px);
}

int end_drag() {
    if (!g_dragging) return 0;
    g_dragging = false;
    GRender_GizmoEndDrag();   // 引擎在这里 EndEdit，整段拖拽合成一步撤销
    return 1;
}

bool dragging() { return g_dragging; }
int axis() { return g_axis; }

} // namespace gryce::editor::gizmo
