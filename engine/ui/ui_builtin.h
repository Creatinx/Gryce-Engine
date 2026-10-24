// ===========================================================================
// engine/ui/ui_builtin.h — 内置即时模式 UI 层（WidgetTree + UiDrawList 之上）
//
// 解决的问题： retained 控件层要求宿主自己建树、自己管几何；而编辑器面板的
// 写法是"每帧从头描述一遍界面"（ImGui 风格）。本层在这之上实现即时模式语义：
// 窗口栈 + 光标流 + SameLine，控件 id 由 (窗口, 标签) 哈希稳定生成，宿主只
// 写线性代码，不维护任何树。
//
// 与 ImGui 的对应关系（子集，语义逐条对齐，均有单测）：
//   * 窗口        begin_window / end_window（可选标题栏 + 拖动 + 折叠，见 flags）；
//   * 光标流      每个 item 占一行，行高 = item 高 + 行间距；
//     SameLine   下一个 item 放到上一个 item 右边，继承其 y/h；
//   * 结构        tree_node / tree_pop / collapsing_header / begin_menu_bar /
//     begin_menu / menu_item / begin_child；
//   * 控件        Text/TextColored/Button/Checkbox/SliderFloat/DragFloat/
//     InputText/Separator/LabelText/ProgressBar/ColorButton；
//   * 弹层        open_popup / begin_popup / end_popup / close_current_popup /
//     begin_modal / set_tooltip（点击外部或 Esc 自动关闭，画在窗口之上）；
//   * 拖放        begin_drag_drop_source / set_drag_drop_payload_string /
//     begin_drag_drop_target / accept_drag_drop_payload_string；
//   * 滚动        内容超出窗口时自动出现覆盖式滚动条，滚轮与拖动都能滚；
//   * 查询        last_item_rect / is_item_hovered / is_item_active。
//
// 值的权威在调用方（和 ImGui 一样）：checkbox/slider 的变量、input 的缓冲区
// 都由宿主持有。每帧的同步顺序是 ensure() → 写值进控件 → touch() 跑交互 →
// 读回值——所以宿主持 undo/重置改值不会被拖拽盖掉，拖拽也不会被旧值盖掉。
//
// 边界（接口不变，需要时再收）：
//   * 只支持一层弹层（菜单/下拉/提示互斥），没有子菜单与嵌套弹层；
//   * 窗口不重叠假设：绘制按提交序单遍走，遇 Panel 切换裁剪域；真的嵌套
//     交叉开窗口（A item → B begin → A item）时后开的窗口会裁掉前一个的
//     尾部 item。本 UI 的确定性面板布局不产生交叉；
//   * 无多行文本编辑、无 Tab 键导航；
//   * LabelText 是"标签: 值"合成字符串，不是双色两段式；
//   * 标签在同一个窗口里必须唯一（同标签 = 同一控件，与 ImGui 相同）。
// ===========================================================================

#ifndef GRYCE_UI_BUILTIN_H
#define GRYCE_UI_BUILTIN_H

#include <cstdint>

#include "ui/ui_widget.h"

namespace gryce_ui {

// 窗口 flags（与 api/ui_api.h 的 GUI_WIN_* 一一对应；这里不 include C 头，
// 免得模型层被 C ABI 反向依赖）。
enum BuiltinWindowFlags : uint32_t {
    kWinNoTitleBar = 1u << 0,
    kWinNoResize   = 1u << 1,
    kWinNoMove     = 1u << 2,
    kWinNoCollapse = 1u << 3,
    kWinMenuBar    = 1u << 5,
    kWinNoScrollbar = 1u << 6,
    // 内置轨专有：显式要求标题栏。默认不给——引擎里大多数面板是"已经排好
    // 位置的固定区域"，多一行标题栏会白白吃掉内容高度（也与老截图不一致）。
    kWinTitleBar   = 1u << 8,
    kWinResizable  = 1u << 9,
};

class BuiltinUi {
public:
    // 同时开着的窗口上限。超出时 begin_window 返回 0 且不提交 chrome——
    // 调用方应当看见这个失败，而不是静默少一个面板。
    static constexpr int kMaxWindows = 8;
    static constexpr int kMaxIndent  = 16;

    // ---- 帧 ----
    // input = 本帧输入快照；theme/metrics = 本帧的主题与文本度量（自动尺寸
    // 要用：按钮宽 = 文字宽 + 内边距）。三者都在帧开头喂一次。
    void begin_frame(const UiInputFrame& input, const Theme& theme,
                     const UiTextMetrics& metrics);
    // 帧末：回收本帧未提交的控件（即时模式每帧重新描述），清拖拽捕获，
    // 关掉"点到外面"的弹层。
    void end_frame();
    // 绘制整帧：窗口 chrome 与内容按提交序进 drawlist，每个窗口的内容裁在
    // 自己的矩形里（长文本/溢出控件不淌出面板）。分两遍：窗口层在前、弹层在后。
    void draw(UiDrawList& dl);

    // HiDPI：逻辑坐标 → 像素的倍数。模型层只做乘法（鼠标除以它、几何乘回去
    // 由渲染桥负责），布局与命中测试始终在逻辑像素里。
    void  set_scale(float s);
    float scale() const { return scale_ > 0.0f ? scale_ : 1.0f; }
    // 屏幕尺寸（逻辑像素）：弹层/提示要夹在屏幕内，遮罩要铺满。
    void  set_display_size(float w, float h);

    // ---- 窗口栈 ----
    // 返回 1 = 开起来了（宿主照常提交内容）；0 = 栈满 / 矩形退化 / 已折叠，
    // 本帧的内容提交全部变成 no-op（与 ImGui 的 collapsed 返回 false 同构）。
    int  begin_window(const char* name, const Rect& rect, uint32_t flags = 0);
    void end_window();
    int  window_depth() const { return depth_; }
    Rect current_window_rect() const;
    // 有效矩形（含标题栏拖动累积的位移）
    Rect window_rect_by_name(const char* name) const;

    // 窗口滚动（宿主自己画滚动条/快捷键翻页时用）
    float window_scroll_y() const;
    void  set_window_scroll_y(float v);

    // ---- 光标流 ----
    // 下一个 item 放到上一个 item 右边（继承其 y 与行高）。spacing < 0 时
    // 用主题的 item_spacing。只对下一个 item 生效。
    void same_line(float spacing = -1.0f);
    // 光标下移 px（SameLine 之后用来收回行距）。
    void spacing(float px);
    // 缩进（树形层级）。indent < 0 用主题的 INDENT 度量。
    void indent(float px = -1.0f);
    void unindent(float px = -1.0f);

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
    int  drag_float(const char* label, float* inout_value, float speed,
                    float vmin, float vmax);
    int  input_text(const char* label, char* buf, int buf_size);
    void progress_bar(float fraction, const char* overlay);
    int  color_button(const char* label, const float rgba[4]);

    // ---- 结构 ----
    // 返回 1 = 展开（宿主照常提交子项，结束时必须调 tree_pop()）。
    int  tree_node(const char* label, int default_open);
    void tree_pop();
    int  collapsing_header(const char* label, int default_open);
    // 菜单栏：在窗口内容顶部占一行；菜单本体是弹层。
    void begin_menu_bar();
    void end_menu_bar();
    int  begin_menu(const char* label);   // 返回 1 = 菜单展开，配 end_menu()
    void end_menu();
    int  menu_item(const char* label, const char* shortcut, int enabled);
    // 子区域：独立裁剪 + 独立竖向滚动 + 可选边框。返回 1 = 可用。
    int  begin_child(const char* id, float w, float h, int border);
    void end_child();

    // ---- 弹层 ----
    // 位置语义：x/y 为左上角；h <= 0 表示按内容自适应（首帧用估算高度，
    // 次帧起用上一帧量到的高度）。
    void open_popup(const char* id, float x, float y, float w, float h);
    void open_popup_on_item(const char* id, float w = 160.0f);
    int  begin_popup(const char* id);
    void end_popup();
    void close_current_popup();
    int  is_popup_open(const char* id) const;
    // 模态：屏幕遮罩 + 居中面板，点外面不关，Esc 关。
    void open_modal(const char* name, float w, float h);
    int  begin_modal(const char* name, float w, float h);

    // 工具提示：鼠标悬停在"刚提交的 item"上超过 0.35 秒才出（宿主时钟为 0
    // 时立即出，便于单测与截图）。画在鼠标右下，是弹层。
    void set_tooltip(const char* text);

    // ---- 拖放 ----
    int  begin_drag_drop_source();     // 1 = 刚提交的 item 被拖起来了
    void set_drag_drop_payload_string(const char* type, const char* text);
    void end_drag_drop_source();
    int  begin_drag_drop_target();     // 1 = 刚提交的 item 正被别的东西悬停
    int  accept_drag_drop_payload_string(const char* type, char* out, int cap);
    void end_drag_drop_target();

    // ---- 查询（对刚提交的 item）----
    Rect last_item_rect() const { return last_rect_; }
    bool is_item_hovered() const { return last_hovered_; }
    bool is_item_active() const { return last_pressed_; }
    // 刚提交的 item 是否被某个鼠标键点中（1 = 右键，2 = 中键，0 = 左键）。
    bool is_item_clicked(int button) const;

    // 鼠标是否落在任何一个已开窗口 / 弹层的矩形里。WantCaptureMouse 的语义：
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
        Rect     rect{};       // 有效矩形（宿主矩形 + 拖动位移）
        Rect     host_rect{};  // 宿主给的矩形（每帧重给）
        float    offset_x = 0.0f, offset_y = 0.0f;   // 标题栏拖动累积
        float    cursor_y = 0.0f;   // 下一个新行 item 的 y（已含 -scroll_y）
        Rect     last_rect{};       // 行内上一个 item（SameLine 定位用）
        bool     pending_same_line = false;
        float    same_line_spacing = -1.0f;

        float    scroll_y   = 0.0f;
        float    content_h  = 0.0f;   // 本帧量到的内容高度（未减滚动）
        float    content_top = 0.0f;  // 内容区上沿（标题栏之下）

        float    indent_x = 0.0f;     // 当前缩进量
        int      indent_depth = 0;    // tree_pop 配对

        uint32_t flags = 0;
        bool     collapsed = false;
        bool     is_popup  = false;   // 弹层：layer 1、无 chrome、无滚动条
        bool     is_child  = false;   // 子区域：无 chrome、有滚动
        bool     in_menubar = false;
        uint8_t  layer = 0;
        float    title_h = 0.0f;
        // 被拒的窗口（退化矩形/栈满）也压栈以保证 Begin/End 配对，但它的
        // 内容提交全部是 no-op（valid = false）。
        bool     valid = true;
    };

    struct PopupState {
        uint64_t id = 0;
        float    x = 0.0f, y = 0.0f;
        float    w = 0.0f, h = 0.0f;      // h <= 0 = 自适应
        float    measured_h = 0.0f;       // 上一帧量到的高度（自适应用）
        bool     open = false;
        bool     modal = false;
        uint64_t open_frame = 0;
    };

    // 光标流定位：返回本 item 的矩形并推进光标。无窗口返回全零矩形。
    Rect place(float w, float h);
    // 当前区域可用（有开着的窗口且没被拒）
    bool usable() const { return depth_ > 0 && windows_[depth_ - 1].valid; }

    // 单行 Label 提交（换行后的每一行也走它）。
    void emit_label(const char* s, uint32_t rgba, bool colored);
    // 自动换行提交一行文本；放得下时等价于一次 emit_label。
    void text_wrapped(const char* s, uint32_t rgba, bool colored);

    // 记录"刚提交的 item"的查询状态（w 为空时清空）。
    void finish_impl(Widget* w);
    // 给控件打上所属窗口与层（含命中域）。
    void stamp(Widget* w);

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
    float    content_rect_h() const;
    float    indent_metric() const;

    // 把 WindowState 压栈（窗口 / 弹层 / 子区域共用）
    WindowState* push_state(uint64_t id, const Rect& rect, uint32_t flags,
                            bool is_popup, bool is_child);
    void pop_state();
    void submit_scrollbar(WindowState& win);
    void open_popup_id(uint64_t id, float x, float y, float w, float h, bool modal);
    int  begin_popup_id(uint64_t id);

    // 跨帧存活的窗口状态（宿主每帧重给矩形，位移/折叠/滚动必须活在别处）
    struct WindowPersist {
        uint64_t id = 0;
        float    offset_x = 0.0f, offset_y = 0.0f;
        float    scroll_y = 0.0f;
        float    content_h = 0.0f;   // 上一帧量到的内容高度（滚轮夹取用）
        bool     collapsed = false;

        // 标题栏拖动：按下帧记"鼠标起点 + 当时的位移"，之后按绝对位置还原。
        // 不能用逐帧增量——按下那一帧的增量会带上鼠标的绝对坐标（窗口瞬移），
        // 而且位移要晚一帧才生效（矩形在本帧 begin 时已经算完）。
        bool     drag_active = false;
        float    drag_mouse_x = 0.0f, drag_mouse_y = 0.0f;
        float    drag_offset_x = 0.0f, drag_offset_y = 0.0f;
    };
    WindowPersist* find_persist(uint64_t id);

    WidgetTree tree_;
    WindowState windows_[kMaxWindows];
    WindowPersist persist_[kMaxWindows];
    int        persist_count_ = 0;
    int        depth_ = 0;
    uint32_t   item_seq_ = 0;   // 帧内提交序号（无状态控件的 id 种子）

    const Theme*         theme_   = nullptr;
    const UiTextMetrics* metrics_ = nullptr;

    PopupState popup_;
    bool       popup_begun_ = false;
    bool       mouse_in_popup_ = false;
    bool       menu_begun_ = false;
    float      display_w_ = 0.0f, display_h_ = 0.0f;

    // 标题栏拖动 / 拖放阈值
    float      mouse_x_ = 0.0f, mouse_y_ = 0.0f;
    float      press_x_ = 0.0f, press_y_ = 0.0f;   // 本帧按下的位置

    // 弹层关闭记账：本帧因为"点到外面"关掉弹层时，记下点击位置。
    // 菜单按钮用它区分"刚把它自己关掉"和"该把它打开"（否则点一下会关了又开）。
    uint64_t   frame_no_ = 0;
    float      dismiss_x_ = -1.0f, dismiss_y_ = -1.0f;

    // 工具提示的悬停计时
    Rect       tooltip_rect_{};
    float      tooltip_since_ = 0.0f;
    bool       tooltip_visible_ = false;

    // 状态过渡动画的时钟（input.time 差分；time==0 = 无时钟，动画不走，
    // 单测/截图因此可复现）
    float      prev_anim_time_ = 0.0f;

    // 刚提交的 item 的 id（拖放源判据）
    uint64_t   last_id_ = 0;

    float      scale_ = 1.0f;

    Rect  last_rect_{};
    bool  last_hovered_ = false;
    bool  last_pressed_ = false;
    bool  mouse_in_window_ = false;   // 本帧鼠标是否落在某个窗口/弹层矩形内
};

} // namespace gryce_ui

#endif // GRYCE_UI_BUILTIN_H
