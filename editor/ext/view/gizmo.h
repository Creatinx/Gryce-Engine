// ===========================================================================
// editor/ext/view/gizmo.h — Gizmo 交互
//
// 引擎已经把整条链路做完：屏幕空间绘制、命中测试、按轴投影换算、以及
// "拖拽整段记一步撤销"（GRender_GizmoBeginDrag 内部 BeginEdit）。ext 这里是
// 薄封装，只多记两件面板要用的事：拖拽态（UI 显示坐标提示）与轴名。
//
// 之所以不在这里重做一遍换算：换算需要相机基向量与视口尺寸，引擎在
// render_api.cpp 里已经拿着；ext 再算一份必然与绘制用的那份漂移。
// ===========================================================================

#ifndef GRYCE_EDITOR_GIZMO_H
#define GRYCE_EDITOR_GIZMO_H

#include <string>

namespace gryce::editor::gizmo {

void reset();

int enable(int enabled);
int set_mode(int operation, int space);

// 当前模式。UI 每帧都要画"哪个高亮"，而 GRender_GetGizmo* 会加锁，
// 在 UI 回调里调用会死锁，所以这里记一份。
int operation();
int space();

// {"operation":0,"space":0,"enabled":1,"axis":-1,"dragging":0,"axisName":"-"}
std::string mode_json();

// 命中：0/1/2 = X/Y/Z，3 = 缩放中心手柄，-1 = 未命中。
int hit_test(float sx, float sy);
int begin_drag(int axis);
int drag(float delta_px);
int end_drag();

bool dragging();
int axis();

} // namespace gryce::editor::gizmo

#endif // GRYCE_EDITOR_GIZMO_H
