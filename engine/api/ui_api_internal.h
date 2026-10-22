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

namespace gryce_ui {

void enter_callback();
void leave_callback();

// 由 render_api.cpp 在 ImGui 后端建好后告知：本后端的纹理第一行是图像的
// 下边（OpenGL）还是上边（D3D12 / Vulkan）。GUi_Image 据此决定要不要翻 V。
//
// 为什么在这里缓存而不是让 GUi_Image 直接问渲染器：UI 回调运行在
// g_renderer.mutex 已持有的路径上，回调内调任何 GRender_* 都会二次加锁
// 而死锁。建后端时写一次、绘制时只读，是唯一安全的做法。
void set_texture_v_flip(bool flip);

// 跨帧控件状态（渲染层每帧 begin_frame 推进帧号 / 老化）。
StateStore& state();
// 确定性布局栈（一帧内 reset → 面板 → 结束，只在 UI 线程用）。
Layout& layout();
// 快捷键表（宿主线程注册 / 查询）。
ShortcutTable& shortcuts();

} // namespace gryce_ui

#endif // GRYCE_UI_API_INTERNAL_H
