// ===========================================================================
// engine/ui/ui_layout_tree_json.h — 布局树的 JSON 往返
//
// 解决的问题：UiLayoutTree 只能用代码建（add/set），面板定义因此没法存盘、
// 没法在测试里用一段 JSON 文字驱动、也没法整棵打出来看。这个模块补上持久化
// 与"文本即用例"这两件事。
//
// 输出形状：
//   { "version": 1,
//     "nodes": [ { "id": "12", "parent": "0", "kind": "column",
//                  "rect": [x, y, w, h], "margin": [t, r, b, l],
//                  "flex": 0.0, "align": "start", "gap": 4.0,
//                  "padding": [t, r, b, l], "cols": 1 }, ... ] }
//
// 三条约定：
//   1. **节点按存储序输出**（父先于子，见 UiLayoutTree::node_at）。读回时按
//      同序 add，所以 add 的"parent 必须已存在"约束天然满足，不需要拓扑排序。
//      同时 JSON 里第 i 个节点对应读回后 node_at(i) 的节点——宿主持有的旧 id
//      据此配对换新，按下标不按值。
//   2. **id 走十进制字符串**，与 ui_snapshot.h 同一理由：JS 的 JSON.parse 只有
//      53 位整数精度，数字形式过一轮就丢低位，两个节点会莫名其妙共父级。
//      注意 id **不会**被读回（树的策略是"id 会话内单调递增、永不复用"，
//      让外部 id 进来就等于让野 id 复活），输出它只为调试可读。
//   3. **读回宽容**：缺字段用默认值、未知 kind/align 名按 Leaf/Start、坏节点
//      跳过；返回 false 只表示顶层结构不对（不是对象，或 nodes 不是数组）。
//      宽容的理由是"人 hand-edit 过的文件也要能读"，不是"随便什么都能读"。
// ===========================================================================

#ifndef GRYCE_UI_LAYOUT_TREE_JSON_H
#define GRYCE_UI_LAYOUT_TREE_JSON_H

#include <string>

#include <nlohmann/json.hpp>

#include "ui/ui_layout_tree.h"

namespace gryce_ui {

nlohmann::json layout_tree_to_json(const UiLayoutTree& t);
bool layout_tree_from_json(const nlohmann::json& j, UiLayoutTree& out);

// kind / align 的名字（"column" / "stretch" 这类）。查不到返回 false，
// 此时 out 不被写入（调用方保留自己的默认值）。
const char* layout_kind_name(UiLayoutKind k);
bool layout_kind_from_name(const std::string& s, UiLayoutKind& out);
const char* layout_align_name(UiLayoutAlign a);
bool layout_align_from_name(const std::string& s, UiLayoutAlign& out);

} // namespace gryce_ui

#endif // GRYCE_UI_LAYOUT_TREE_JSON_H
