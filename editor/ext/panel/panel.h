// ===========================================================================
// editor/ext/panel/panel.h — 面板数据模型
//
// ext 只产出**数据**，不画界面（../README.md 第 6 节）。这样扩展不依赖
// imgui.h，也不会被 ImGui 版本绑死；换 UI 框架只改渲染侧。
//
// 面板数据每帧都会被问一次，所以这里的原则是"能少算就少算"：
// 层级树和检视结果按帧缓存，同一帧内重复调用直接返回上次的结果。
// ===========================================================================

#ifndef GRYCE_EDITOR_PANEL_H
#define GRYCE_EDITOR_PANEL_H

#include "api/types.h"

#include <cstdint>
#include <string>

namespace gryce::editor::panel {

void reset();

// 场景层级树：{"ok":1,"count":7,"roots":[{"handle":1,"name":"A",
// "children":[{"handle":2,...}]}]}
std::string hierarchy_json();

// 实体检视：变换 + 组件 + 反射属性值。
std::string inspector_json(GEntityHandle handle);

// 资产浏览：引擎列举 + 按 kind 分组。
std::string assets_json(const char* dir);

// 一帧统计。
std::string stats_json();

// 每帧调用，让上面几个缓存在新的一帧失效。
void tick();

} // namespace gryce::editor::panel

#endif // GRYCE_EDITOR_PANEL_H
