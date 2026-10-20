// ===========================================================================
// engine/api/ui_api_internal.h — ui_api 的内部帧标记（不属于公开 ABI）
//
// render_api.cpp 在调用宿主 UI 回调前后各调一次，ui_api.cpp 据此判断
// "现在能不能画"。这样 GUi_* 在回调外被误调时是安全空操作，而不是
// 往一个处于 Idle 状态的 ImGui 上下文里塞控件（那会直接断言）。
// ===========================================================================

#ifndef GRYCE_UI_API_INTERNAL_H
#define GRYCE_UI_API_INTERNAL_H

namespace gryce_ui {

void enter_callback();
void leave_callback();

} // namespace gryce_ui

#endif // GRYCE_UI_API_INTERNAL_H
