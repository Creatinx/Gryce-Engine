// ===========================================================================
// editor/ext/session/session.h — 会话状态与选择集
//
// 引擎不知道"当前在编辑哪个文档"这件事：GScene_GetCurrentPath 只记最后一次
// 加载的路径，不区分"打开"和"另存为"，也没有脏标记。这些是编辑器概念，
// 归 ext（../README.md 规则三）。
//
// 选择集同理：引擎是单选（GEdit_SelectEntity 一次只认一个），多选、主选中、
// 按集合删除都在这里。主选中会同步给引擎，因为渲染侧的 Gizmo 读的是引擎的
// GEntity_GetSelected。
// ===========================================================================

#ifndef GRYCE_EDITOR_SESSION_H
#define GRYCE_EDITOR_SESSION_H

#include "api/types.h"

#include <string>
#include <vector>

namespace gryce::editor::session {

// 选择集。primary 是"最后点中的那个"，Inspector 显示它；
// items 是全部选中项，删除/复制作用于它。
struct Selection {
    GEntityHandle primary = 0;
    std::vector<GEntityHandle> items;

    bool contains(GEntityHandle h) const;
    void add(GEntityHandle h);
    void remove(GEntityHandle h);
    void clear();
};

// 回到初始状态：没有文档、没有选择、脏标记清除。
void reset();

// 打开文档。入队成功返回 1。换文档会清空选择集（旧句柄在新场景里无效）
// 与编辑历史（旧快照里的 UUID 不存在）。
int open_doc(const char* path);

// 关闭文档：只清状态，不写盘。
void close_doc();

// 保存。path 为空时用当前文档路径。
int save_doc(const char* path);

const std::string& doc_path();
const char* doc_name();          // 路径最后一段
bool dirty();
bool has_doc();

// 每帧：刷新脏标记、剔除已消失的选中项（撤销/删除会让句柄失效）。
void tick(float dt);

int select(GEntityHandle handle);
int select_add(GEntityHandle handle);
int select_toggle(GEntityHandle handle);
int select_remove(GEntityHandle handle);
void clear_selection();
const Selection& selection();

// 句柄是否还指向场景里存在的实体（撤销后旧句柄会失效）。
bool handle_alive(GEntityHandle handle);

std::string to_json();
std::string stats_json();

} // namespace gryce::editor::session

#endif // GRYCE_EDITOR_SESSION_H
