// ===========================================================================
// editor/ext/command/commands.h — 编辑命令
//
// 全部转 GEdit_*。ext 在这里补三件事：
//
// 1. 参数校验。入队接口没有返回值通道（只回 1/0 表示"收不收"），参数错了
//    只能在日志里追，所以 NaN、句柄 <= 0、空名字一律在入队前挡掉。
//
// 2. 待确认创建。GEdit_CreateEntity 只入队、不回句柄，而编辑器建完就要选中
//    它。这里靠引擎"创建即选中"的约定：入队时记 pending，下一帧比对
//    GEntity_GetSelected 拿到新句柄。连续创建两个时只有最后一个能确认 ——
//    这是入队模型的固有限制，不是这里能修好的，见 README 第 6 节。
//
// 3. 复合操作合并成一步撤销。多选删除、批量设置都要 BeginEdit/EndEdit 包一层，
//    否则撤一次只回退一个实体，用户要点 N 次。
// ===========================================================================

#ifndef GRYCE_EDITOR_COMMANDS_H
#define GRYCE_EDITOR_COMMANDS_H

#include "api/types.h"

#include <cstdint>
#include <string>

namespace gryce::editor::commands {

// 每帧推进待确认队列、收集事件。返回本帧事件名（供 tick 的 JSON 输出）。
void tick();
void reset();

// 本帧事件数组：["created:12","save"]。取走后清空，避免同一个事件被
// 反复上报（UI 会因此重复弹提示）。
std::string take_events_json();

int create(const char* name, GEntityHandle parent);
int destroy(GEntityHandle handle);
int rename(GEntityHandle handle, const char* name);
int reparent(GEntityHandle handle, GEntityHandle parent);

int set_position(GEntityHandle handle, float x, float y, float z);
int set_rotation(GEntityHandle handle, float x, float y, float z, float w);
int set_scale(GEntityHandle handle, float x, float y, float z);
int translate(GEntityHandle handle, float dx, float dy, float dz);

int set_property_number(GEntityHandle handle, uint64_t type_hash,
                        const char* prop, double value);
int set_property_bool(GEntityHandle handle, uint64_t type_hash,
                      const char* prop, int value);

int add_component(GEntityHandle handle, const char* type_name);
int remove_component(GEntityHandle handle, uint64_t type_hash);

int set_material(GEntityHandle handle, const char* mesh_path,
                 float r, float g, float b, float roughness, float metallic);

int import_asset(const char* path);

int delete_selection();
int save(const char* path);
int set_play(int play);
int toggle_play();
int step_frame();

// 选中实体的世界位置中心（含子节点）。没有选中时 ok=0。
std::string focus_json();

// 组件类型名 → 哈希。查引擎的类型注册表，失败返回 0。
uint64_t type_hash_for(const char* type_name);

// 分组：嵌套调用按最外层计（引擎侧只有一个 group_open 标志）。
void begin_edit(const char* label);
void end_edit();
// 强制收掉所有未闭合的分组。退出、换文档、清历史时必须调 ——
// 留一个开着的分组会让引擎的下一次快照把两次无关操作合并成一步。
void abort_edit();
const char* current_group_label();

} // namespace gryce::editor::commands

#endif // GRYCE_EDITOR_COMMANDS_H
