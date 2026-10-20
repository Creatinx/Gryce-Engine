// ===========================================================================
// editor/ext/ui/ui.cpp — 界面帧的生命周期
//
// 一帧的顺序（全部在 UI 回调之外）：
//   1. flush()            执行上一帧界面攒下的意图（选择/创建/改名/属性…）
//   2. apply_view_input() 消费上一帧采到的鼠标：Gizmo 拖拽 或 点选拾取
//   3. view::set_size()   视口渲染尺寸跟随面板里实际画出来的矩形
//   4. panel::rebuild_*() 重建面板数据（UI 与 JSON 共用同一份）
//   5. 取 ImGui 纹理 id   回调内不能调 GRender_*，所以必须在这里取好
//
// 为什么输入要跨一帧：ImGui 的鼠标状态只在回调内有效，而拾取（GScene_PickRay）
// 必须加锁调用。所以回调只管把鼠标记下来，真正的几何计算放在下一帧的 tick。
// ===========================================================================

#include "ui/ui.h"

#include "ui/intent.h"
#include "ui/ui_internal.h"

#include "panel/model.h"
#include "panel/panel.h"
#include "session/session.h"
#include "view/gizmo.h"
#include "view/viewport.h"

#include "api/render_api.h"
#include "api/scene_api.h"

#include "common/json_writer.h"

namespace gryce::editor::ui {

namespace {

DrawState g_state;

void draw_thunk(void* user) { draw_all(user); }

// 拾取：用视口相机自己发射射线，不走 GScene_PickScreen —— 后者用的是场景里
// 的 Camera 实体，而编辑器视口的朝向来自 GViewport_SetEditorTransform，
// 两者可以完全无关。用错相机的表现是"点左边选中右边"。
GEntityHandle pick_at(float sx, float sy) {
    GVec3 origin{};
    GVec3 dir{};
    if (!view::screen_ray(sx, sy, &origin, &dir)) return 0;
    return GScene_PickRay(&origin, &dir, 0.0f);
}

void apply_view_input() {
    const ViewInput in = g_state.input;
    g_state.input = ViewInput{};   // 消费掉，等回调重新采

    if (g_state.dragging) {
        // 拖拽期间不判 hover：鼠标划出视口边界时用户仍然在拖，此时中断
        // 会让物体停在半路上，而且没有抬起事件收尾。
        if (in.dx != 0.0f || in.dy != 0.0f) gizmo::drag(in.dx + in.dy);
        if (in.released) {
            gizmo::end_drag();
            g_state.dragging = false;
            g_state.drag_axis = -1;
        }
        return;
    }

    if (!in.hovered || !in.clicked) return;

    // Gizmo 的端口用**窗口绝对坐标**（它画在 ImGui 的绝对坐标系里，引擎的
    // 屏幕原点由 set_screen_origin 告知）；拾取走 screen_ray，用视口内坐标。
    // 两者混用会偏出正好一个面板宽度，症状是"gizmo 抓不住、点击选错物体"。
    const int axis = gizmo::hit_test(in.x + g_state.view.x, in.y + g_state.view.y);
    if (axis >= 0 && gizmo::begin_drag(axis)) {
        g_state.dragging = true;
        g_state.drag_axis = axis;
        return;
    }

    const GEntityHandle hit = pick_at(in.x, in.y);
    if (hit > 0) session::select(hit);
    else session::clear_selection();
}

} // namespace

DrawState& state() { return g_state; }

bool is_expanded(GEntityHandle handle) {
    for (GEntityHandle h : g_state.expanded) {
        if (h == handle) return true;
    }
    return false;
}

void toggle_expanded(GEntityHandle handle) {
    for (std::size_t i = 0; i < g_state.expanded.size(); ++i) {
        if (g_state.expanded[i] == handle) {
            g_state.expanded.erase(g_state.expanded.begin() +
                                   static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
    g_state.expanded.push_back(handle);
}

void reset() {
    g_state = DrawState{};
    intent::reset();
}

void init() {
    reset();
    // 回调设上去，引擎就会接管 ImGui（CreateContext + 三后端渲染后端）。
    // 数值上等价于打开引擎的调试面板路径，但画的是我们自己的窗口。
    GRender_SetUICallback(&draw_thunk, nullptr);
    g_state.enabled = true;
}

void shutdown() {
    GRender_SetUICallback(nullptr, nullptr);
    g_state.enabled = false;
}

void tick(float dt) {
    (void)dt;

    // 1. 意图：上一帧界面点的按钮在这一帧生效
    intent::flush();

    // 2. 视口输入：拾取 / Gizmo 拖拽
    if (g_state.enabled) apply_view_input();

    // 3. 视口尺寸 + 屏幕原点跟随实际绘制矩形（阈值 16：面板被折叠到底时不要
    //    设成 0，引擎会把渲染目标建成 1x1 并让整条 HDR 链重建，白卡一帧）。
    if (g_state.view.w >= 16.0f && g_state.view.h >= 16.0f) {
        view::set_size(static_cast<int>(g_state.view.w + 0.5f),
                       static_cast<int>(g_state.view.h + 0.5f));
        view::set_screen_origin(g_state.view.x, g_state.view.y);
    }

    // 4. 面板数据：UI 与脚本读同一份（model.cpp 里读引擎，这里不锁冲突）
    const GEntityHandle selected = session::selection().primary;
    panel::rebuild_hierarchy();
    panel::rebuild_inspector(selected);
    panel::rebuild_assets(g_state.dir_buf);
    panel::rebuild_stats();

    // 5. 纹理 id：GRender_GetImGuiTextureId 会加锁，只能在回调外取
    g_state.viewport_tex = GRender_GetImGuiTextureId(view::viewport_texture());
}

int enabled() { return g_state.enabled ? 1 : 0; }

int set_enabled(int on) {
    const bool want = on != 0;
    if (want == g_state.enabled) return 1;
    g_state.enabled = want;

    // 关掉时把回调也摘掉：留着回调会让引擎继续持有一整套 ImGui（每帧仍有
    // NewFrame/Render 的开销），而运行时宿主并不需要。
    GRender_SetUICallback(want ? &draw_thunk : nullptr, nullptr);
    return 1;
}

int available() { return GRender_HasImGui() ? 1 : 0; }

int frame_count() { return g_state.frames; }

int wants_keyboard() { return g_state.capture_keyboard ? 1 : 0; }
int wants_mouse() { return g_state.capture_mouse ? 1 : 0; }
int pointer_in_view() { return g_state.pointer_in_view ? 1 : 0; }

std::string state_json() {
    Json j;
    j.begin_object();
    j.kv("ok", 1);
    j.kv("enabled", g_state.enabled);
    j.kv("hasImGui", available() != 0);
    j.kv("frames", g_state.frames);
    j.kv("draws", g_state.draws);
    j.kv("viewportTex", static_cast<unsigned long long>(g_state.viewport_tex));
    j.kv("viewX", g_state.view.x);
    j.kv("viewY", g_state.view.y);
    j.kv("viewWidth", g_state.last_w);
    j.kv("viewHeight", g_state.last_h);
    j.kv("pointerInView", g_state.pointer_in_view);
    j.kv("captureKeyboard", g_state.capture_keyboard);
    j.kv("captureMouse", g_state.capture_mouse);
    j.kv("dragging", g_state.dragging);
    j.kv("gizmoOperation", gizmo::operation());
    j.kv("gizmoSpace", gizmo::space());
    j.kv("displayMode", view::display_mode());
    j.kv("pending", intent::pending_count());
    j.end_object();
    return j.str();
}

} // namespace gryce::editor::ui
