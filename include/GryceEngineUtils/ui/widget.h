#pragma once

// GryceEngineUtils::ui::widget.h — Widget 基类 + 共享 UI 类型

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "GryceEngineUtils/renderer.h"
#include "GryceEngineUtils/ui/signal.h"

namespace GryceEngineUtils::ui {

class StyleSheet;
class Animation;

// 颜色：复用引擎 2D 颜色
using Color = gryce_engine::render::Color;

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    bool contains(float px, float py) const {
        return px >= x && px <= x + w && py >= y && py <= y + h;
    }
};

struct Margin {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

using Padding = Margin;

enum class Anchor {
    TopLeft, TopCenter, TopRight,
    CenterLeft, Center, CenterRight,
    BottomLeft, BottomCenter, BottomRight,
    Stretch
};

enum class TextAlign {
    Left, Center, Right
};

enum class LayoutType {
    Vertical, Horizontal, Grid, Absolute
};

// ---------------------------------------------------------------------------
// Style — 控件样式（默认值 + 样式表覆盖）
// ---------------------------------------------------------------------------
struct Style {
    Color background = Color(0.13f, 0.14f, 0.16f, 1.0f);
    Color background_hover = Color(0.22f, 0.24f, 0.27f, 1.0f);
    Color background_pressed = Color(0.08f, 0.09f, 0.11f, 1.0f);
    Color color = Color::white();
    float font_size = 16.0f;
    float border_radius = 0.0f;
    float border_width = 0.0f;
    Color border_color = Color::black();
    Padding padding{8.0f, 8.0f, 8.0f, 8.0f};
    Margin margin{0.0f, 0.0f, 0.0f, 0.0f};
    float opacity = 1.0f;
    std::string font_family = "sans-serif";
    TextAlign text_align = TextAlign::Left;

    struct Shadow {
        bool enabled = false;
        float x = 0.0f;
        float y = 2.0f;
        float blur = 4.0f;
        Color color = Color(0.0f, 0.0f, 0.0f, 0.3f);
    } box_shadow;

    bool has_background = false; // 样式表是否显式设置了背景
};

// ---------------------------------------------------------------------------
// Widget — 所有 UI 控件的根
// ---------------------------------------------------------------------------
class Widget {
public:
    explicit Widget(const char* id = nullptr);
    virtual ~Widget();

    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    const std::string& id() const { return id_; }
    const std::string& style_id() const { return style_id_; }

    // 布局
    void set_position(float x, float y);
    void set_size(float w, float h);
    void set_anchor(Anchor anchor);
    void set_margin(const Margin& m);
    void set_padding(const Padding& p);

    // 父子关系
    void add_child(Widget* child);
    void remove_child(Widget* child);
    Widget* parent() const { return parent_; }
    const std::vector<Widget*>& children() const { return children_; }

    // 可见性 / 透明度 / 缩放
    void set_visible(bool visible);
    bool visible() const { return visible_; }
    void set_opacity(float opacity);
    float opacity() const { return opacity_; }
    void set_scale(float scale);
    float scale() const { return scale_; }

    // 样式
    void set_style(const Style& style);
    Style& style() { style_dirty_ = true; return style_; }
    const Style& style() const { return style_; }
    void set_style_class(const char* cls);
    const std::string& style_class() const { return style_class_; }
    void set_style_id(const char* id);
    const Style& computed_style() const;

    // 状态（供样式表伪类匹配）
    bool hovered() const { return state_hovered_; }
    bool pressed() const { return state_pressed_; }
    bool focused() const { return state_focused_; }
    void set_hovered(bool on) { state_hovered_ = on; style_dirty_ = true; }
    void set_pressed(bool on) { state_pressed_ = on; style_dirty_ = true; }
    void set_focused(bool on) { state_focused_ = on; style_dirty_ = true; }

    // 启用/禁用（禁用控件不可交互，视觉降级）
    void set_enabled(bool enabled);
    bool enabled() const { return enabled_; }

    // 样式脏标记（样式表缓存用）
    void mark_style_dirty() { style_dirty_ = true; }

    // 鼠标事件开关（根控件禁用后不会因鼠标移动切换 hover 状态导致背景闪烁）
    void set_accepts_mouse(bool on) { accepts_mouse_ = on; }
    bool accepts_mouse() const { return accepts_mouse_; }

    // 属性注入（从 .uif 解析结果设置属性）
    // key: 属性名（如 "text", "color", "onClick"）
    // value: 属性值（字符串形式）
    // 返回 true 表示该属性已被处理
    virtual bool set_property(const char* key, const char* value);

    // 事件回调函数名（从 .uif 的 onClick/onChange 等属性设置）
    const std::string& event_callback(const char* event_name) const;
    void set_event_callback(const char* event_name, const char* callback);

    // 在控件树中按 ID 查找子控件（递归搜索）
    // 返回第一个匹配的控件指针，未找到返回 nullptr
    Widget* find_element_by_id(const char* id);
    const Widget* find_element_by_id(const char* id) const;

    // 事件（信号）
    Signal<void(Widget*)> on_click;
    Signal<void(Widget*, bool)> on_hover; // entered(true)/left(false)
    Signal<void(Widget*)> on_focus;
    Signal<void(Widget*)> on_blur;

    // 渲染 / 更新（框架内部调用）
    virtual void draw(Renderer* renderer);
    virtual void update(float dt);

    // 命中测试
    virtual bool hit_test(float x, float y) const;

    // 全局点击回调：UIManager 在每次鼠标按下时遍历调用。
    // 弹出类控件（MenuBar/ComboBox 等）据此在点击自身区域外时关闭。
    virtual void on_global_click(Widget* clicked) { (void)clicked; }

    // 样式表需要
    virtual const char* type_name() const { return "Widget"; }

    // 布局结果
    const Rect& bounds() const { return bounds_; }
    void set_bounds(const Rect& r) { bounds_ = r; }
    void mark_layout_dirty() { layout_dirty_ = true; }
    // 内部：计算相对父容器的 bounds（绝对布局 + 锚点）
    virtual void compute_bounds(const Rect& parent_bounds);

    // 动画注册（UIManager 驱动）
    void add_animation(Animation* anim);
    void remove_animation(Animation* anim);

    // 对象存活标记（Signal::connect_member / 动画安全）
    std::shared_ptr<bool> signal_lifetime() const { return lifetime_; }
    bool alive() const { return *lifetime_; }

    // Flexbox 弹性属性
    void set_flex_grow(float grow);
    float flex_grow() const { return flex_grow_; }

    // 内容尺寸（Label/Button 自动高度用）
    virtual float preferred_height(float width) const { return size_h_ > 0.0f ? size_h_ : 0.0f; }
    virtual float preferred_width() const { return size_w_; }

    float position_x() const { return position_x_; }
    float position_y() const { return position_y_; }
    float size_w() const { return size_w_; }
    float size_h() const { return size_h_; }
    Anchor anchor() const { return anchor_; }
    const Margin& margin() const { return margin_; }
    const Padding& padding() const { return padding_; }

protected:
    std::string id_;
    std::string style_id_;
    std::string style_class_;
    Style style_;

    // 事件回调函数名映射（从 .uif 的 onClick/onChange 等属性设置）
    // key: 事件名（如 "click", "change"）, value: JS 函数名
    std::unordered_map<std::string, std::string> event_callbacks_;

    float position_x_ = 0.0f;
    float position_y_ = 0.0f;
    float size_w_ = 0.0f;
    float size_h_ = 0.0f;
    Anchor anchor_ = Anchor::TopLeft;
    Margin margin_;
    Padding padding_;
    float opacity_ = 1.0f;
    float scale_ = 1.0f;

    bool visible_ = true;
    bool enabled_ = true;
    bool accepts_mouse_ = true;
    bool style_dirty_ = true;
    Style cached_style_;
    bool state_hovered_ = false;
    bool state_pressed_ = false;
    bool state_focused_ = false;

    Widget* parent_ = nullptr;
    std::vector<Widget*> children_;
    Rect bounds_;
    bool layout_dirty_ = true;
    float flex_grow_ = 0.0f;

    // 动画列表（UIManager 经 add/remove 管理）
    std::vector<Animation*> animations_;

    std::shared_ptr<bool> lifetime_ = std::make_shared<bool>(true);

    friend class Animation;
    friend class StyleSheet;
    friend class UIManager;
};

} // namespace GryceEngineUtils::ui
