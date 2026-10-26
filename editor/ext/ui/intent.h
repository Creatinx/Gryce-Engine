// ===========================================================================
// editor/ext/ui/intent.h — 界面意图队列
//
// 绘制回调运行在渲染器的锁内（见 render_api.h 的「编辑器 UI 托管」），因此在
// 回调里调任何 GEdit_* / GScene_* / GCore_* 都会死锁。界面的做法是：
//
//   回调内   只把"用户想干什么"记成一条 Intent（纯数据，不碰引擎）
//   tick()   在锁外把队列里的 Intent 逐条执行
//
// 代价是界面操作有一帧延迟（下一帧才生效），换来的是"回调里可以随便画、
// 绝不会因为某个面板手滑调了引擎而整个编辑器卡死"。
//
// 视口内的鼠标交互（拾取 / Gizmo 拖拽）不走这个队列：它们需要每帧读鼠标
// 状态且对延迟敏感，直接在 tick 里处理（见 ui.cpp 的 apply_view_input）。
// ===========================================================================

#ifndef GRYCE_EDITOR_UI_INTENT_H
#define GRYCE_EDITOR_UI_INTENT_H

#include "api/types.h"

#include <cstdint>
#include <string>

namespace gryce::editor::ui {
namespace intent {

enum IntentKind : int {
    // 选择
    kSelect = 0, kSelectAdd, kSelectToggle, kSelectClear,
    // 层级
    kCreate, kDestroy, kRename, kReparent,
    // 变换
    kSetPosition, kSetRotation, kSetScale,
    // 组件
    kSetPropertyNumber, kSetPropertyBool, kSetPropertyVec, kSetPropertyString,
    kRemoveComponent, kAddComponent,
    // 资产
    kImportAsset,
    // 文档 / 历史 / 播放
    kSave, kUndo, kRedo, kPlay, kStep,
    // 撤销分组（一次连续拖拽 = 一步）
    kBeginEdit, kEndEdit,
    // 视口
    kGizmoMode, kDisplayMode,
};

struct Intent {
    int kind = kSelect;
    GEntityHandle handle = 0;
    GEntityHandle other = 0;    // 父实体
    uint64_t hash = 0;          // 组件类型哈希
    float f0 = 0.0f;
    float f1 = 0.0f;
    float f2 = 0.0f;
    float f3 = 1.0f;
    double number = 0.0;
    float vec[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // 向量类属性（flag 存分量数）
    int flag = 0;
    std::string a;              // 名字 / 属性名 / 路径 / 显示模式 / 分组标签
    std::string b;              // 组件类型名
};

// 入队。同一帧内同类选择意图会被合并（只保留最后一条），
// 避免"鼠标划过十个节点"时排十条无用命令。
void push_select(int kind, GEntityHandle handle);
void push(const Intent& intent);

// 执行并清空。只能在 UI 回调之外调用。
void flush();

void reset();

int pending_count();

} // namespace intent
} // namespace gryce::editor::ui

#endif // GRYCE_EDITOR_UI_INTENT_H
