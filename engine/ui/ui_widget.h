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
// 文本编辑（单行，与常见编辑器一致，均有单测）：
//   * 插入点 + 选区锚点（字节偏移）、Shift+方向键扩选、Ctrl+A 全选；
//   * 方向键按 UTF-8 码点边界移动，Ctrl+方向键按词移动，Home/End 首尾；
//   * Backspace/Delete 有选区时删选区、无选区时删前一/后一个码点；
//   * Ctrl+C/X/V 走进程级剪贴板（ui_clipboard()，宿主也可直接读写）；
//   * 点击定位插入点（按文本的真实宽度换算，见 UiTextMetrics）；
//   * 输入法预编辑（preedit）以带下划线的形式画在插入点处。
//
// 绘制顺序：控件带 layer（0 = 窗口层，1 = 弹层）。WidgetTree::draw 分两遍画，
// 所以弹层内容永远盖在窗口内容之上，与提交顺序无关。
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
    Panel     = 5,   // 窗口 / 弹层背景板：填充 + 边框，不参与命中测试
    Progress  = 6,   // 进度条：value = fraction，label = 浮层文字，不参与命中测试
    Separator = 7,   // 分隔线，不参与命中测试
    DragFloat = 8,   // 按住左右移动改值（value / min_value / max_value）
    TreeNode  = 9,   // 折叠三角 + 标签；open = 展开态
    Header    = 10,  // 折叠头（整行可点）；open = 展开态
    MenuItem  = 11,  // 菜单项（label + 右对齐快捷键）；enabled = 可用
    ScrollBar = 12,  // 垂直滚动条；value = 0..1 位置，thumb = 可见比例
    Tooltip   = 13,  // 浮层板 + 文字（纯绘制，不参与命中测试）
    ClipBegin = 14,  // 绘制专用：push_clip(rect)，不参与命中测试
    ClipEnd   = 15,  // 绘制专用：pop_clip，不参与命中测试
    Bar       = 16,  // 装饰性横条（菜单栏底/分组标题底），不参与命中测试
    TitleBar  = 17,  // 窗口标题栏：可拖动、左侧折叠框（交互在 BuiltinUi 里）
    Frame     = 18,  // 纯框：填充 + 边框，不参与命中测试、不切裁剪（子区域用）
    Count,
};

// 控件文案 / 输入内容容量。
// label 要容纳**整行文本**：Label 控件用它显示正文，48 字节会把一句 50 字符的
// 说明截掉尾巴（早期内置 UI 的实际症状）。96 = 一行折行前的大致上限。
constexpr int kWidgetLabelCap = 96;
constexpr int kWidgetTextCap  = 128;
constexpr int kWidgetPreeditCap = 64;

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
    float wheel = 0.0f;            // 本帧滚轮增量（正 = 上滚，内容往下看）

    // 本帧键入的 Unicode 码点（聚焦的 TextInput 消费；宿主未提供字符通道时
    // 恒为空，输入框仍可用退格，见 platform InputManager 的 char 回调）
    static constexpr int kMaxChars = 64;
    uint32_t chars[kMaxChars] = {};
    int      char_count = 0;
    bool     backspace = false;   // 兼容字段（等价于 keys 里出现 KeyBackspace）

    // 本帧按下的非字符键（GLFW 键码，见下面的 Key* 常量）。方向键/Home/End/
    // Delete 一类编辑键走这里——字符通道只送可打印码点。
    static constexpr int kMaxKeys = 16;
    int keys[kMaxKeys] = {};
    int key_count = 0;

    // 修饰键（本帧快照，位见 Mod* 常量）。Ctrl+C/V/X/A、Shift+方向键看它。
    static constexpr uint32_t ModShift = 1u;
    static constexpr uint32_t ModCtrl  = 2u;
    static constexpr uint32_t ModAlt   = 4u;
    static constexpr uint32_t ModSuper = 8u;
    uint32_t mods = 0;

    // 输入法预编辑文本（UTF-8，空 = 无组合）。宿主没有输入法时恒为空。
    char preedit[kWidgetPreeditCap] = {};
    int  preedit_caret = -1;   // 预编辑内的插入点（字节偏移，-1 = 末尾）

    // 秒（宿主时钟）。只用于插入符闪烁：0 = 恒显（单测/截图可复现），
    // 非 0 时按 1.06 秒一个周期闪。
    float time = 0.0f;

    // GLFW 键码（与平台层 keys[] 索引同一套）
    enum : int {
        KeyTab = 258, KeyEnter = 257, KeyEsc = 256, KeyBackspace = 259,
        KeyDelete = 261, KeyRight = 262, KeyLeft = 263, KeyDown = 264, KeyUp = 265,
        KeyHome = 268, KeyEnd = 269, KeyA = 65, KeyC = 67, KeyV = 86, KeyX = 88,
        KeyZ = 90,
    };

    bool key_pressed(int code) const {
        for (int i = 0; i < key_count; ++i) {
            if (keys[i] == code) return true;
        }
        return false;
    }
    bool any_key() const { return key_count > 0; }
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
// 进程级剪贴板（Ctrl+C/X/V 与宿主共用一份）
//
// 放在模型层而不是树里：同一进程里可能有多棵树（每个面板一棵），剪贴板是
// 进程级资源。宿主也可以直接读写（比如从系统剪贴板同步）。
// ---------------------------------------------------------------------------
struct UiClipboard {
    char  text[kWidgetTextCap * 2] = {};
    uint32_t seq = 0;   // 每次写入 +1（宿主可据此同步到系统剪贴板）
};
UiClipboard& ui_clipboard();

// ---------------------------------------------------------------------------
// Widget — 单个控件
// ---------------------------------------------------------------------------
struct Widget {
    uint64_t  id   = 0;
    WidgetKind kind = WidgetKind::Button;
    Rect      rect{};                    // 绝对坐标（左上角原点）
    char      label[kWidgetLabelCap] = {};

    // —— 值（按种类解释）——
    float value     = 0.0f;              // Slider/DragFloat/ScrollBar：当前值
    float min_value = 0.0f;              // Slider/DragFloat：最小值
    float max_value = 1.0f;              // Slider/DragFloat：最大值
    bool  checked   = false;             // Checkbox
    bool  open      = false;             // TreeNode/Header：展开态
    char  text[kWidgetTextCap] = {};     // TextInput：内容（'\0' 结尾）

    // —— 文本编辑态（TextInput）——
    int   caret      = 0;    // 插入点（字节偏移，恒落在码点边界上）
    int   sel_anchor = -1;   // 选区锚点（字节偏移，-1 = 无选区）
    float scroll_x   = 0.0f; // 文本水平滚动（保证插入点可见）
    char  preedit[kWidgetPreeditCap] = {};   // 输入法预编辑（带下划线显示）

    // —— ScrollBar ——
    float thumb = 0.3f;      // 滑块长度占轨道的比例（可见高度 / 内容高度）

    bool enabled = true;

    // ---- 外观覆盖（美化）----
    // 角半径：< 0 = 按控件种类用主题默认（按钮/输入框 4、窗口 6、弹层 4 ...）。
    // 宿主显式给 0 表示"这个就是直角"——模态遮罩铺满全屏时必须这样，否则四角
    // 会露出没压暗的底图。
    float corner_radius = -1.0f;
    // 投影：浮窗/弹层/提示打开，平铺面板关掉（阴影会被邻居盖住，白花顶点）。
    bool  shadow = false;

    // ---- 颜色覆盖（内置即时模式层的特例控件用）----
    // ColorButton 的底色就是被选的颜色本身，TextColored 的文字色由调用方给。
    // 不覆盖时一律走主题 token——覆盖只是"这一帧这个控件"的例外。
    bool     bg_overridden = false;
    uint32_t bg_color      = 0;   // 0xRRGGBBAA
    bool     text_overridden = false;
    uint32_t text_color      = 0;

    // 所属窗口（即时模式层打戳，用于绘制时按窗口分组裁剪；0 = 无窗口）。
    uint64_t scope = 0;
    // 层级：0 = 窗口层，1 = 弹层（弹层永远画在窗口之上，见 WidgetTree::draw）。
    uint8_t  layer = 0;

    // 命中测试的有效域（窗口/弹层的内容矩形）。控件滚出可见区后不响应鼠标，
    // 否则"看不见但能点中"会误触发。has_clip 为假时按 rect 自身判。
    bool has_clip = false;
    Rect clip{};

    // —— 每帧由 touch() 刷新的交互标志（对宿主只读）——
    bool hovered = false;
    bool pressed = false;   // 正被按住（且是按在它身上开始的）
    bool clicked = false;   // 本帧在它身上完成一次点击

    // —— 状态过渡动画（主题 animation 块驱动；由 advance_animations 推进）——
    // anim_to == 0 表示"不在动画集合"：绘制走静态目标色（单测/截图路径）。
    // 目标切换时把当前插值位置记为 anim_from，从新目标重新计时——快速来回
    // 悬停不会跳色。
    uint32_t anim_from  = 0;   // 过渡起点色（0xRRGGBBAA）
    uint32_t anim_to    = 0;   // 过渡目标色；0 = 未初始化/不在动画集合
    float    anim_clock = 0.0f; // 距本次目标切换的秒数
};

// 绘制上下文：焦点（输入框高亮边框）与插入符是否可见（闪烁相位由树算好，
// 绘制本身不碰时钟——这样截图与单测都可复现）。
struct UiDrawContext {
    uint64_t focus_id     = 0;
    bool     caret_visible = true;
};

// 控件绘制（主题给颜色/度量，度量器给文本宽高）。
void ui_draw_widget(const Widget& w, const Theme& theme, const UiTextMetrics& metrics,
                    const UiDrawContext& ctx, UiDrawList& dl);

// 兼容旧签名（等价于 UiDrawContext{focus_id, true}）。
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
    // metrics 可空：空时插入点定位与选中区宽度退回估算（见 ui_approx_text_width）。
    // 宿主不该让"布局用真实度量、编辑用估算"长期并存——传进来才是正解。
    void begin_frame(const UiInputFrame& input, const UiTextMetrics* metrics = nullptr);
    const UiInputFrame& input() const { return input_; }
    const UiTextMetrics* metrics() const { return metrics_; }

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

    // 命中域：之后提交的控件都受这个矩形约束（滚出可见区的不响应鼠标）。
    // clear 之后回到"按控件自身矩形判"。
    void set_clip(const Rect& r);
    void clear_clip();

    // DragFloat 的灵敏度（每像素改多少）。即时模式层按标签设置，默认 1。
    void set_drag_speed(float per_pixel) { drag_speed_hint_ = per_pixel; }

    // retained 宿主：对整棵树跑一遍交互（用各控件现有的 rect/label）。
    void update_all();

    // 推进状态过渡动画（悬停/按下的填充色渐变，参数见 theme_anim()）。
    // 在交互落地之后、draw 之前调用一次；dt = 本帧秒数（0 = 时间没走，
    // 动画停在原地）。不参与动画的控件被跳过。retained 宿主不调用即
    // 得到"瞬变"行为，与单测/截图路径一致。
    void advance_animations(float dt, const Theme& theme);

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

    // ---- 拖放（即时模式层实现 drag_drop_*，状态放在这里以便跨帧存活）----
    bool  drag_active() const { return drag_active_; }
    const char* drag_payload_type() const { return drag_type_; }
    const char* drag_payload_text() const { return drag_text_; }
    void  begin_drag(uint64_t source_id, const char* type, const char* text);
    void  end_drag();

    // ---- 绘制 ----
    // 两遍：先 layer 0（窗口），再 layer 1（弹层）。数组序 = 提交序 = 绘制序。
    void draw(UiDrawList& dl, const Theme& theme, const UiTextMetrics& metrics) const;

    int  count() const { return alive_count_; }
    void clear();

    // 插入符可见性（由 begin_frame 按 input.time 算；time == 0 时恒真）。
    bool caret_visible() const { return caret_visible_; }

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
    void   edit_text(Widget& w);
    void   sync_text_scroll(Widget& w) const;

    Slot      slots_[kCapacity];
    int       used_ = 0;
    int       free_head_ = -1;
    int       alive_count_ = 0;
    uint32_t  frame_ = 0;

    UiInputFrame input_;
    const UiTextMetrics* metrics_ = nullptr;
    uint64_t active_id_ = 0;
    uint64_t focus_id_  = 0;
    uint64_t press_claimed_ = 0;   // 本帧拿到按下的控件（0 = 点了空白）
    bool     caret_visible_ = true;

    bool     drag_active_ = false;
    uint64_t drag_source_ = 0;
    char     drag_type_[32] = {};
    char     drag_text_[kWidgetTextCap] = {};

    // DragFloat 的拖拽基准（上一帧鼠标 x、当前被拖的控件、每像素灵敏度）
    float    drag_last_x_ = 0.0f;
    uint64_t drag_owner_  = 0;
    float    drag_speed_  = 0.0f;
    float    drag_speed_hint_ = 1.0f;

    // 提交期的命中域（见 set_clip）
    bool     clip_active_ = false;
    Rect     pending_clip_{};
};

// ---- 文本工具（宿主与内置层都会用，导出以便复用）----
int   utf8_text_length(const char* s);                       // 字节数
int   utf8_prev_boundary(const char* s, int pos);            // 上一个码点起点
int   utf8_next_boundary(const char* s, int pos);            // 下一个码点起点
// 字节偏移 → 像素 x（相对文本左端）；越界自动夹到 [0, len]。
float text_x_at(const char* s, int byte_pos, const UiTextMetrics& metrics, float font_size);
// 像素 x → 最近的码点边界（点击定位插入点）。
int   text_pos_at(const char* s, float x, const UiTextMetrics& metrics, float font_size);
// 按词移动（Ctrl+方向键）：跳过空白与标点。
int   text_word_left(const char* s, int pos);
int   text_word_right(const char* s, int pos);

} // namespace gryce_ui

#endif // GRYCE_UI_WIDGET_H
