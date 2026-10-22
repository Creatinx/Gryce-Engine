// ===========================================================================
// engine/api/ui_op_queue_internal.h — 操作队列的进程内唯一实例（不属于公开 ABI）
//
// GUi_PushOp / GUi_DrainOps 等 C 接口都作用在同一个队列上；渲染层也需要它
// （每渲染帧推进一次帧计数，见 GRender_EndFrame）。公开头文件只暴露 C ABI，
// 这个访问器给引擎内部的两处实现用。
//
// 实例是函数局部 static：首次调用时初始化，C++11 起初始化本身线程安全。
// 队列自己带互斥量（叶子锁），跨线程 push / drain 不需要调用方再配锁。
// ===========================================================================

#ifndef GRYCE_UI_OP_QUEUE_INTERNAL_H
#define GRYCE_UI_OP_QUEUE_INTERNAL_H

#include "api/ui_op_queue.h"

namespace gryce_ui {

// 进程内唯一的 UI 操作队列。
OpQueue& op_queue();

} // namespace gryce_ui

#endif // GRYCE_UI_OP_QUEUE_INTERNAL_H
