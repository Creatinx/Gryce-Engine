// ===========================================================================
// engine/api/ui_api_internal.h — ui_api 的内部共享件（不属于公开 ABI）
//
// render_api.cpp 在调用宿主 UI 回调前后各调一次，ui_api.cpp 据此判断
// "现在能不能画"。这样 GUi_* 在回调外被误调时是安全空操作，而不是
// 往一个处于 Idle 状态的 ImGui 上下文里塞控件（那会直接断言）。
//
// 后半部分是模型层的进程内唯一实例访问器：GUi_* C 接口与渲染层
// （每帧推进）都要够到同一份状态 / 布局 / 快捷键表。实例是函数局部
// static，首次调用时初始化，C++11 起初始化本身线程安全。
// ===========================================================================

#ifndef GRYCE_UI_API_INTERNAL_H
#define GRYCE_UI_API_INTERNAL_H

#include "ui/ui_input.h"
#include "ui/ui_layout.h"
#include "ui/ui_state.h"
#include "ui/ui_texture_v_flip.h"

namespace gryce_ui {

void enter_callback();
void leave_callback();

// 后端纹理采样约定（OpenGL 的第 0 行是图像下边）由 render_api.cpp 在
// GRender_Init 里写进 gryce_ui::set_texture_v_flip，声明见 ui_texture_v_flip.h。
// UI 回调运行在 g_renderer.mutex 已持有的路径上，回调内调任何 GRender_* 都会
// 二次加锁而死锁，所以判据只能初始化期落一次、绘制时只读。

// 跨帧控件状态（渲染层每帧 begin_frame 推进帧号 / 老化）。
StateStore& state();
// 确定性布局栈（一帧内 reset → 面板 → 结束，只在 UI 线程用）。
Layout& layout();
// 快捷键表（宿主线程注册 / 查询）。
ShortcutTable& shortcuts();

} // namespace gryce_ui

#endif // GRYCE_UI_API_INTERNAL_H
