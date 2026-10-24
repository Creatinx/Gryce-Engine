// ===========================================================================
// engine/ui/ui_widget.h — retained 控件层（内置 UI 渲染路径第 3 阶段）
//
// 解决的问题：前三个阶段给了引擎"画什么"的能力（drawlist + 渲染桥），但没人
// 定义"什么是按钮"——命中测试、按下/点击/拖拽、键盘焦点、控件的值与绘制。
// 本文件补上这一层，且**不依赖 ImGui、不依赖渲染器、不依赖窗口**：喂一份
// 输入快照进去，就能单测"点一下滑块值变多少"。
//
// 与 ui_component（组件树布局）的分工：组件树回答"控件摆在哪"（结构是数据、
// 跨帧存活），本层回答"控件是什么、怎么响应输入、画成什么样"。两者通过
// Rect 接头——组件树算出 rect，喂给本层当几何。
//
// 两种用法（同一份代码）：
//   * **retained**：宿主建一棵树、每帧改几何，调 update_all() + draw()；
//   * **即时模式**：宿主每帧重新提交控件（id 稳定），touch() 即取即用，
//     帧末 sweep() 回收没再出现的控件——GUi_* 的 C 面就是这么实现的
//     （见 ui_builtin.h）。
//
// 交互语义（与 ImGui 对齐的子集，均有单测）：
//   * hovered   = 鼠标几何包含（按控件自己的矩形判，不解析遮挡）；
//   * pressed   = 按下时鼠标在本控件内（active 归属它）且按键仍按住；
//   * clicked   = active 归属它、本帧松开、且松开时鼠标仍在其中——
//                 拖到外面松开 = 取消，不产生点击；
//   * active    = "按下时归谁"：同一帧里**后提交**的包含鼠标的控件拿到
//                 active（提交序 = 绘制序 = 上层序），重叠时上层拿按下；
//   * 焦点      = 文本输入框专有：点它聚焦，点别的控件或空白处转移/清空；
//   * 标签      = 纯显示，不参与命中测试（面板背景一类的大矩形因此不会
//                 抢走上面按钮的鼠标）。
//
// 已知边界（等需要时再收，接口不变）：
//   * 不解析重叠遮挡（hovered 是几何包含；本 UI 的确定性布局不产生重叠，
//     弹出层尚未实现）；
//   * 无滚动、无工具提示、无拖放、无键盘导航（Tab 切换焦点）；
//   * 文本无光标渲染（编辑正确，视觉上看不到插入符）。
// ===========================================================================

#ifndef GRYCE_UI_WIDGET_H
#define GRYCE_UI_WIDGET_H

#include <cstdint>
#include <cstring>

#include "ui/ui_drawlist.h"
#include "ui/ui_layout.h"
#include "ui/ui_theme.h"

namespace gryce_ui {

// ---------------------------------------------------------------------------
// WidgetKind — 控件种类
// ---------------------------------------------------------------------------
enum class WidgetKind : uint32_t {
    Button    = 0,
    Label     = 1,   // 纯显示：不参与命中测试
    Slider    = 2,   // value ∈ [min_value, max_value]
    Checkbox  = 3,   // checked
    TextInput = 4,   // text（键盘焦点）
    Panel     = 5,   // 窗口背景板：填充 + 边框，不参与命中测试
    Progress  = 6,   // 进度条：value = fraction，label = 浮层文字，不参与命中测试
    Separator = 7,   // 分隔线，不参与命中测试
    Count,
};

// 控件文案 / 输入内容容量。
// label 要容纳**整行文本**：Label 控件用它显示正文，48 字节会把一句 50 字符的
// 说明截掉尾巴（早期内置 UI 的实际症状）。96 = 一行折行前的大致上限。
constexpr int kWidgetLabelCap = 96;
constexpr int kWidgetTextCap  = 128;

// 默认字号（逻辑像素）。主题度量里没有字号一项：行高/内边距都按它推算，
// 换主题不换字号——要可变字号时加成主题度量（只能加在枚举末尾）。
constexpr float kWidgetFontSize = 13.0f;

// ---------------------------------------------------------------------------
// UiInputFrame — 一帧的输入快照
//
// 宿主收集（轮询或事件均可），在帧开头一次性喂给 WidgetTree::begin_frame。
// 模型层不自己读窗口/时钟：单测里输入是精确的，帧循环里喂什么由宿主定。
// ---------------------------------------------------------------------------
struct UiInputFrame {
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    bool  mouse_down[3]     = {};  // 按住（左/右/中）
    bool  mouse_clicked[3]  = {};  // 本帧刚按下（边沿）
    bool  mouse_released[3] = {};  // 本帧刚松开（边沿）

    // 本帧键入的 Unicode 码点（聚焦的 TextInput 消费；宿主未提供字符通道时
    // 恒为空，输入框仍可用退格，见 platform InputManager 的 char 回调）
    static constexpr int kMaxChars = 64;
    uint32_t chars[kMaxChars] = {};
    int      char_count = 0;
    bool     backspace = false;
};

// ---------------------------------------------------------------------------
// UiTextMetrics — 文本度量（布局与绘制共用的唯一口径）
//
// 为什么是接口而不是写死的常数：引擎侧最终应该用 FontAtlas 的真实度量，
// 而单测/无字体环境用估算。两边必须走同一个接口，否则"布局按真实宽度、
// 绘制按估算宽度"会互相漂移。默认实现是估算（ASCII 0.5em/字符、非 ASCII
// 1em/字符、行高 1.25em）——够确定性布局与命中测试用（命中矩形始终比文本大）。
// ---------------------------------------------------------------------------
struct UiTextMetrics {
    virtual ~UiTextMetrics() = default;
    virtual float width(const char* text, float font_size) const;
    virtual float line_height(float font_size) const;
};

// 默认度量实现（也是 UiTextMetrics 虚函数的默认行为）。
float ui_approx_text_width(const char* text, float font_size);

// ---------------------------------------------------------------------------
// Widget — 单个控件
// ---------------------------------------------------------------------------
struct Widget {
    uint64_t  id   = 0;
    WidgetKind kind = WidgetKind::Button;
    Rect      rect{};                    // 绝对坐标（左上角原点）
    char      label[kWidgetLabelCap] = {};

    // —— 值（按种类解释）——
    float value     = 0.0f;              // Slider：当前值
    float min_value = 0.0f;              // Slider：最小值
    float max_value = 1.0f;              // Slider：最大值
    bool  checked   = false;             // Checkbox
    char  text[kWidgetTextCap] = {};     // TextInput：内容（'\0' 结尾）

    bool enabled = true;

    // ---- 颜色覆盖（内置即时模式层的两个特例控件用）----
    // ColorButton 的底色就是被选的颜色本身，TextColored 的文字色由调用方给。
    // 不覆盖时一律走主题 token——覆盖只是"这一帧这个控件"的例外。
    bool     bg_overridden = false;
    uint32_t bg_color      = 0;   // 0xRRGGBBAA
    bool     text_overridden = false;
    uint32_t text_color      = 0;

    // 所属窗口（即时模式层打戳，用于绘制时按窗口分组裁剪；0 = 无窗口）。
    uint64_t scope = 0;

    // —— 每帧由 touch() 刷新的交互标志（对宿主只读）——
    bool hovered = false;
    bool pressed = false;   // 正被按住（且是按在它身上开始的）
    bool clicked = false;   // 本帧在它身上完成一次点击
};

// 控件绘制（主题给颜色/度量，度量器给文本宽高）。focus_id 只对 TextInput
// 有意义（聚焦时边框用强调色）。WidgetTree::draw 与内置即时模式层的窗口
// 分组裁剪都走这一个函数——两处画出来的东西必须逐字节一致。
void ui_draw_widget(const Widget& w, const Theme& theme, const UiTextMetrics& metrics,
                    uint64_t focus_id, UiDrawList& dl);

// ---------------------------------------------------------------------------
// WidgetTree — 控件表 + 交互状态 + 绘制
// ---------------------------------------------------------------------------
class WidgetTree {
public:
    // 同时可见的带状态控件到不了这个量级；装不下说明 id 在泄漏（每帧新建
    // 不回收），那是 sweep/clear 漏了，不是调大容量的理由。
    static constexpr int kCapacity = 512;

    // ---- 帧输入 ----
    void begin_frame(const UiInputFrame& input);
    const UiInputFrame& input() const { return input_; }

    // ---- 控件提交 ----
    // 取（没有则新建）并按本帧几何刷新交互标志；随后按种类落地标准交互语义
    // （复选框翻转 / 滑块拖拽 / 输入框编辑）。表满返回 nullptr——调用方应
    // 当看见这个失败（那一步不画也不交互），而不是静默少一个控件。
    Widget* touch(uint64_t id, WidgetKind kind, const Rect& rect, const char* label);

    // 与 touch 相同的取/建/刷新几何，但**不跑交互、不清交互标志**。用途：
    // 调用方的值是权威来源（滑块值、复选框勾态、输入框内容在宿主的变量里），
    // 必须在交互之前同步进控件，否则拖拽会被上一帧的旧值盖掉。即时模式层
    // 的流程统一是 ensure() → 同步值 → touch() → 读回值。
    Widget* ensure(uint64_t id, WidgetKind kind, const Rect& rect, const char* label);

    Widget*       find(uint64_t id);
    const Widget* find(uint64_t id) const;

    // retained 宿主：对整棵树跑一遍交互（用各控件现有的 rect/label）。
    void update_all();

    // 按提交序遍历存活控件（数组序 = 提交序 = 绘制序）。宿主一般不直接
    // 遍历（用 find()）；即时模式层的窗口分组裁剪需要这个序。
    template <typename F>
    void for_each_alive(F&& fn) const {
        for (int i = 0; i < used_; ++i) {
            if (slots_[i].alive) fn(slots_[i].w);
        }
    }

    // 帧末：松开即释放拖拽捕获；点击落在空白处（没有控件认领）时清键盘
    // 焦点。回收本帧未提交的控件是另一件事（sweep），即时模式宿主两个都调，
    // retained 宿主只调本函数。
    void end_frame();
    void sweep();

    // ---- 交互状态 ----
    uint64_t active_id() const { return active_id_; }  // 按下归属（拖拽捕获）
    uint64_t focus_id() const { return focus_id_; }    // 键盘焦点（TextInput）
    void clear_focus() { focus_id_ = 0; }

    // ---- 绘制 ----
    // 数组序 = 提交序 = 绘制序（后画者在上层）。主题给颜色/度量，度量器给
    // 文本宽高——两者都由调用方注入，模型层不持有渲染器也不持有字体。
    void draw(UiDrawList& dl, const Theme& theme, const UiTextMetrics& metrics) const;

    int  count() const { return alive_count_; }
    void clear();

private:
    struct Slot {
        Widget   w;
        bool     alive = false;
        uint32_t frame = 0;   // 最后一次被 touch 的帧号（sweep 判据）
        int      next_free = -1;   // 空闲链表（借用字段，见 alloc）
    };

    Slot*  alloc(uint64_t id);
    Slot*  find_slot(uint64_t id);
    void   apply_standard_interaction(Widget& w);

    Slot      slots_[kCapacity];
    int       used_ = 0;
    int       free_head_ = -1;
    int       alive_count_ = 0;
    uint32_t  frame_ = 0;

    UiInputFrame input_;
    uint64_t active_id_ = 0;
    uint64_t focus_id_  = 0;
    uint64_t press_claimed_ = 0;   // 本帧拿到按下的控件（0 = 点了空白）
};

} // namespace gryce_ui

#endif // GRYCE_UI_WIDGET_H
