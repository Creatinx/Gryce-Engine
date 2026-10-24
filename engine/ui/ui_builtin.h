// ===========================================================================
// engine/ui/ui_builtin.h — 内置即时模式 UI 层（WidgetTree + UiDrawList 之上）
//
// 解决的问题： retained 控件层要求宿主自己建树、自己管几何；而编辑器面板的
// 写法是"每帧从头描述一遍界面"（ImGui 风格）。本层在这之上实现即时模式语义：
// 窗口栈 + 光标流 + SameLine，控件 id 由 (窗口, 标签) 哈希稳定生成，宿主只
// 写线性代码，不维护任何树。
//
// 与 ImGui 的对应关系（子集，语义逐条对齐，均有单测）：
//   * BeginWindow/EndWindow  → begin_window/end_window（无标题栏、无折叠、
//     无拖动；flags 参数在本层被忽略——chrome 只有底 + 边框）；
//   * 光标流                 → 每个 item 占一行，行高 = item 高 + 行间距；
//     SameLine               → 下一个 item 放到上一个 item 右边，继承其 y/h；
//   * 控件                   → Text/TextColored/Button/Checkbox/SliderFloat/
//     InputText/Separator/LabelText/ProgressBar/ColorButton；
//   * 查询                   → last_item_rect / is_item_hovered /
//     is_item_active（对刚提交的那一个 item）。
//
// 值的权威在调用方（和 ImGui 一样）：checkbox/slider 的变量、input 的缓冲区
// 都由宿主持有。每帧的同步顺序是 ensure() → 写值进控件 → touch() 跑交互 →
// 读回值——所以宿主持 undo/重置改值不会被拖拽盖掉，拖拽也不会被旧值盖掉。
//
// 边界（接口不变，需要时再收）：
//   * 窗口不重叠假设：绘制按提交序单遍走，遇 Panel 切换裁剪域；真的嵌套
//     交叉开窗口（A item → B begin → A item）时后开的窗口会裁掉前一个的
//     尾部 item。本 UI 的确定性面板布局不产生交叉；
//   * 无圆角（drawlist 只有矩形）、无滚动、无工具提示、无拖放、无弹层；
//   * LabelText 是"标签: 值"合成字符串（48 字符截断），不是双色两段式；
//   * 标签在同一个窗口里必须唯一（同标签 = 同一控件，与 ImGui 相同）。
// ===========================================================================

#ifndef GRYCE_UI_BUILTIN_H
#define GRYCE_UI_BUILTIN_H

#include <cstdint>

#include "ui/ui_widget.h"

namespace gryce_ui {

class BuiltinUi {
public:
    // 同时开着的窗口上限。超出时 begin_window 返回 0 且不提交 chrome——
    // 调用方应当看见这个失败，而不是静默少一个面板。
    static constexpr int kMaxWindows = 8;

    // ---- 帧 ----
    // input = 本帧输入快照；theme/metrics = 本帧的主题与文本度量（自动尺寸
    // 要用：按钮宽 = 文字宽 + 内边距）。三者都在帧开头喂一次。
    void begin_frame(const UiInputFrame& input, const Theme& theme,
                     const UiTextMetrics& metrics);
    // 帧末：回收本帧未提交的控件（即时模式每帧重新描述），清拖拽捕获。
    void end_frame();
    // 绘制整帧：窗口 chrome 与内容按提交序进 drawlist，每个窗口的内容裁在
    // 自己的矩形里（长文本/溢出控件不淌出面板）。
    void draw(UiDrawList& dl);

    // ---- 窗口栈 ----
    // 返回 1 = 开起来了（宿主照常提交内容）；0 = 栈满 / 矩形退化，本帧的
    // 内容提交全部变成 no-op（与 ImGui 的 collapsed 返回 false 同构）。
    int  begin_window(const char* name, const Rect& rect);
    void end_window();
    int  window_depth() const { return depth_; }
    Rect current_window_rect() const;

    // ---- 光标流 ----
    // 下一个 item 放到上一个 item 右边（继承其 y 与行高）。spacing < 0 时
    // 用主题的 item_spacing。只对下一个 item 生效。
    void same_line(float spacing = -1.0f);
    // 光标下移 px（SameLine 之后用来收回行距）。
    void spacing(float px);

    // ---- 控件（无窗口时全部 no-op）----
    // 文本按内容区宽度自动折行（ImGui 的 TextUnformatted 默认行为）。
    void text(const char* s);
    void text_colored(uint32_t rgba, const char* s);
    void label_text(const char* label, const char* text);
    void separator();
    int  button(const char* label);
    int  button_size(const char* label, float w, float h);
    int  checkbox(const char* label, int* inout_value);
    int  slider_float(const char* label, float* inout_value, float vmin, float vmax);
    int  input_text(const char* label, char* buf, int buf_size);
    void progress_bar(float fraction, const char* overlay);
    int  color_button(const char* label, const float rgba[4]);

    // ---- 查询（对刚提交的 item）----
    Rect last_item_rect() const { return last_rect_; }
    bool is_item_hovered() const { return last_hovered_; }
    bool is_item_active() const { return last_pressed_; }

    // 鼠标是否落在任何一个已开窗口的矩形里。WantCaptureMouse 的语义：
    // 为真时宿主不该把这次点击派给视口（相机环绕、拾取）。
    bool wants_mouse() const { return mouse_in_window_; }
    // 键盘焦点在输入框上时返回 true：宿主不该派发快捷键，否则输入框里
    // 打 "Wire" 会在中途切走工具。
    bool wants_keyboard() const { return tree_.focus_id() != 0; }

    // 底层控件表（测试与调试用：提交序、存活数、id 作用域）。
    WidgetTree& tree() { return tree_; }

private:
    struct WindowState {
        uint64_t id    = 0;    // hash_label(窗口名)
        Rect     rect{};
        float    cursor_y = 0.0f;   // 下一个新行 item 的 y
        Rect     last_rect{};       // 行内上一个 item（SameLine 定位用）
        bool     pending_same_line = false;
        float    same_line_spacing = -1.0f;
    };

    // 光标流定位：返回本 item 的矩形并推进光标。无窗口返回全零矩形。
    Rect place(float w, float h);

    // 单行 Label 提交（换行后的每一行也走它）。
    void emit_label(const char* s, uint32_t rgba, bool colored);
    // 自动换行提交一行文本；放得下时等价于一次 emit_label。
    void text_wrapped(const char* s, uint32_t rgba, bool colored);

    // 记录"刚提交的 item"的查询状态（w 为空时清空）。
    void finish_impl(Widget* w);

    // 控件 id：交互控件 = hash(标签, 窗口 id)——同标签同窗口即同一控件
    // （与 ImGui 相同，滑块值/勾态/输入内容因此跨帧稳定）。无状态控件
    // （Label/Separator/Progress/Panel）另混入帧内提交序号：同一窗口里
    // 两个"##separator"必须是两个 slot，否则只能画出一个。
    uint64_t item_id(const char* label, bool stateless);
    float    pad_x() const;
    float    pad_y() const;
    float    item_spacing() const;
    float    line_height() const;
    float    frame_height() const;
    float    content_width() const;

    WidgetTree tree_;
    WindowState windows_[kMaxWindows];
    int        depth_ = 0;
    uint32_t   item_seq_ = 0;   // 帧内提交序号（无状态控件的 id 种子）

    const Theme*         theme_   = nullptr;
    const UiTextMetrics* metrics_ = nullptr;

    Rect  last_rect_{};
    bool  last_hovered_ = false;
    bool  last_pressed_ = false;
    bool  mouse_in_window_ = false;   // 本帧鼠标是否落在某个窗口矩形内
};

} // namespace gryce_ui

#endif // GRYCE_UI_BUILTIN_H
