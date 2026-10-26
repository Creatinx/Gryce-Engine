// ===========================================================================
// editor/ext/ui/ui_internal.h — ui.cpp 与 draw.cpp 之间的共享状态
//
// 分开的理由：帧的生命周期（采集输入、执行意图、重建数据）与像素布局
// （谁在左、谁在右、宽多少）是两件变动频率完全不同的事。放一个文件里时
// 每次调布局都要在一堆引擎调用之间找地方改。
// ===========================================================================

#ifndef GRYCE_EDITOR_UI_INTERNAL_H
#define GRYCE_EDITOR_UI_INTERNAL_H

#include "api/types.h"

#include <cstdint>
#include <vector>

namespace gryce::editor::ui {

// 视口矩形（屏幕坐标）。由绘制侧写入、由 tick 读取 —— 渲染目标的尺寸必须
// 等于"实际画出来的尺寸"，否则缩放窗口后点哪都偏。
struct ViewRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// 视口内鼠标输入：只在 UI 回调里采得到（ImGui 的鼠标状态是帧内的），
// 由下一次 tick 消费。x/y 是视口内坐标，dx/dy 是屏幕像素增量。
struct ViewInput {
    bool hovered = false;
    bool clicked = false;
    bool down = false;
    bool released = false;
    float x = 0.0f;
    float y = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
};

struct DrawState {
    bool enabled = true;
    int frames = 0;          // 回调被调用次数
    int draws = 0;           // 其中真的画进 ImGui 的次数
    int last_w = 0;
    int last_h = 0;

    uint64_t viewport_tex = 0;
    ViewRect view;
    ViewInput input;

    // ImGui 控件要可写的 char*，所以这些缓冲必须跨帧存活。
    char name_buf[128] = {};
    GEntityHandle name_handle = 0;
    char dir_buf[256] = "res:/models";
    GEntityHandle asset_sel = 0;
    char asset_sel_path[512] = {};

    // 一次连续编辑（拖拽滑块）是否已经开了撤销分组。
    bool edit_open = false;

    // 视口拖拽态：从按下到抬起之间，鼠标移出视口也要继续拖。
    bool dragging = false;
    int drag_axis = -1;

    // 上一帧 ImGui 的捕获状态。快捷键与相机导航要据此让路：
    // 在输入框里打字时按 W 不该切到移动工具。
    bool capture_keyboard = false;
    bool capture_mouse = false;
    bool pointer_in_view = false;

    // 层级树右键 → "Rename"：把焦点交给检视器的名字框（下一帧生效）。
    // 展开态不在这里 —— 树节点控件自己持有（GUI_TREE_DEFAULT_OPEN 只管初始）。
    bool rename_requested = false;
    GEntityHandle rename_handle = 0;

    // 检视器里变换控件的本地副本。引擎的当前值比界面晚一帧到达，直接拿
    // 模型值喂 DragFloat3 会在拖拽中被旧值回写，表现为"拖一下弹回去"。
    // 只在没有任何控件处于编辑态时才重新同步。
    GEntityHandle transform_handle = 0;
    float pos_buf[3] = {0, 0, 0};
    float euler_buf[3] = {0, 0, 0};
    float scale_buf[3] = {1, 1, 1};

    // 当前正在编辑的那个组件属性的本地副本。理由同 transform_buf：
    // 写入要下一帧才回到模型，不缓存的话拖拽时值会被旧值拉回去。
    // 只留一格 —— ImGui 同一时刻只可能有一个控件处于活动态。
    GEntityHandle prop_handle = 0;
    uint64_t prop_hash = 0;
    char prop_name[64] = {};
    int prop_count = 0;
    float prop_buf[4] = {0, 0, 0, 0};

    // "Add Component" 弹窗里的过滤框。
    char comp_filter[64] = {};
};

DrawState& state();

// 绘制整帧（在 UI 回调内调用，只能读状态、只能入队意图）。
void draw_all(void* user);

} // namespace gryce::editor::ui

#endif // GRYCE_EDITOR_UI_INTERNAL_H
