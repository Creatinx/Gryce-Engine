// ===========================================================================
// engine/ui/ui_layout.h — 确定性布局栈（无 ImGui 依赖的纯逻辑）
//
// 解决的问题：工具型界面（面板、工具栏、属性行）的矩形如果每个调用方手算，
// 同一份逻辑会写出十几种稍有不同的版本，而"窗口尺寸变化时视口矩形是什么"
// 这种问题就没有单测可写——它散落在各处的 SetCursorPos 调用里。
//
// 本布局栈把矩形计算收成一组纯函数：给定相同的调用序列，输出相同的矩形，
// 与窗口、与 ImGui、与 GPU 全都无关。因此"任意窗口尺寸下视口拿到确定矩形"
// 可以直接断言（见 test/ui_tests.cpp 的 layout 套件）。
//
// 与 ImGui 的关系：布局栈**不替代** ImGui 的自动排版，它服务的是"需要精确
// 矩形"的场景——视口分区、 gizmo 原点、属性行对齐。控件照旧用 GUi_* 画，
// 画在哪由布局栈算。混用时注意：GUi_BeginWindow 等自带排版，不要同时用
// ImGui 的光标和本布局栈排同一区域。
//
// 线程模型：只在 UI 线程（回调内）使用，一次帧内 reset → 若干面板 → 结束。
// ===========================================================================

#ifndef GRYCE_UI_LAYOUT_H
#define GRYCE_UI_LAYOUT_H

namespace gryce_ui {

// 屏幕空间矩形（绝对坐标，左上角原点，单位逻辑像素）。
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    float right() const { return x + w; }
    float bottom() const { return y + h; }
};

class Layout {
public:
    static constexpr int   kMaxDepth      = 16;
    static constexpr float kDefaultSpacing = 4.0f;

    // 重置到视口根区域（等价于一个无内边距的面板）。
    void reset(const Rect& viewport);

    // 压入 / 弹出面板区域。area 是绝对矩形；pad 是四边内边距（内容区 = area
    // 向内缩 pad）。光标复位到内容区左上角。
    void begin_panel(const Rect& area, float pad = 6.0f);
    void end_panel();

    int  depth() const { return depth_; }
    Rect content() const;              // 当前面板内容区
    float avail_height() const;        // 内容区底部到当前光标的剩余高度
    float cursor_y() const;

    // 行布局。begin_row 会先结束尚未 end_row 的行（防忘）。
    void begin_row(float height);
    // 光标下移 max(行高, 行内最高格) + spacing。按最高格推进而不是按
    // begin_row 的行高：cell(0, 30) 这种显式高度的格子不该被下一行盖住。
    void end_row();
    // 行内取一格。w <= 0 = 填满本行剩余宽度；h <= 0 = 用行高。
    // 返回的矩形是绝对坐标，直接喂给 GUi_* 或自定义绘制。
    Rect cell(float w, float h);
    // 额外垂直间距（先结束未收尾的行）。
    void space(float px);
    // 水平缩进：作用于下一次 begin_row 的行首（树形层级每层一级）。
    void indent(float px);
    void unindent(float px);

    void  set_spacing(float px);
    float spacing() const;

private:
    struct Level {
        Rect  area;        // 内容区（已扣内边距）
        float row_h    = 0.0f;
        float row_max_h = 0.0f;   // 本行已取格子里的最大高度（end_row 的推进量）
        float cursor_x = 0.0f;
        float cursor_y = 0.0f;
        float indent   = 0.0f;
        bool  row_open = false;
    };

    Level& cur() { return stack_[depth_ - 1]; }
    const Level& cur() const { return stack_[depth_ - 1]; }

    Level  stack_[kMaxDepth];
    int    depth_    = 0;
    float  spacing_  = kDefaultSpacing;
};

} // namespace gryce_ui

#endif // GRYCE_UI_LAYOUT_H
