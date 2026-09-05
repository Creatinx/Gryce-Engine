#include "GryceEngineUtils/ui/widget.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "GryceEngineUtils/ui/animation.h"
#include "GryceEngineUtils/ui/style_preset.h"
#include "GryceEngineUtils/ui/stylesheet.h"
#include "ui_draw.h"

namespace GryceEngineUtils::ui {

// ============================================================================
// 辅助：字符串解析工具
// ============================================================================
namespace {

// 解析颜色字符串（支持 #RRGGBB 和 #RRGGBBAA 格式）
bool parse_color_from_string(const char* str, Color& out) {
    if (!str || str[0] != '#') return false;
    unsigned long val = std::strtoul(str + 1, nullptr, 16);
    size_t len = std::strlen(str);
    if (len == 7) { // #RRGGBB
        out.r = ((val >> 16) & 0xFF) / 255.0f;
        out.g = ((val >> 8) & 0xFF) / 255.0f;
        out.b = (val & 0xFF) / 255.0f;
        out.a = 1.0f;
        return true;
    } else if (len == 9) { // #RRGGBBAA
        out.r = ((val >> 24) & 0xFF) / 255.0f;
        out.g = ((val >> 16) & 0xFF) / 255.0f;
        out.b = ((val >> 8) & 0xFF) / 255.0f;
        out.a = (val & 0xFF) / 255.0f;
        return true;
    }
    return false;
}

// 解析布局类型字符串
LayoutType parse_layout_type(const char* str) {
    if (!str) return LayoutType::Absolute;
    if (std::strcmp(str, "vertical") == 0) return LayoutType::Vertical;
    if (std::strcmp(str, "horizontal") == 0) return LayoutType::Horizontal;
    if (std::strcmp(str, "grid") == 0) return LayoutType::Grid;
    return LayoutType::Absolute;
}

// 解析锚点/对齐字符串
Anchor parse_anchor(const char* str) {
    if (!str) return Anchor::TopLeft;
    if (std::strcmp(str, "center") == 0) return Anchor::Center;
    if (std::strcmp(str, "left") == 0) return Anchor::CenterLeft;
    if (std::strcmp(str, "right") == 0) return Anchor::CenterRight;
    if (std::strcmp(str, "top") == 0) return Anchor::TopCenter;
    if (std::strcmp(str, "bottom") == 0) return Anchor::BottomCenter;
    if (std::strcmp(str, "stretch") == 0) return Anchor::Stretch;
    return Anchor::TopLeft;
}

// 解析文本对齐
TextAlign parse_text_align(const char* str) {
    if (!str) return TextAlign::Left;
    if (std::strcmp(str, "center") == 0) return TextAlign::Center;
    if (std::strcmp(str, "right") == 0) return TextAlign::Right;
    return TextAlign::Left;
}

// 解析边距字符串（格式: "all" 或 "top,right,bottom,left"）
void parse_margin(const char* str, Margin& out) {
    if (!str) return;
    float v = std::atof(str);
    out.left = out.top = out.right = out.bottom = v;
}

// 解析布尔值
bool parse_bool(const char* str) {
    if (!str) return false;
    return std::strcmp(str, "true") == 0 || std::strcmp(str, "1") == 0 || std::strcmp(str, "yes") == 0;
}

// 解析 float
float parse_float(const char* str, float default_val = 0.0f) {
    if (!str || !*str) return default_val;
    return static_cast<float>(std::atof(str));
}

// 解析 int
int parse_int(const char* str, int default_val = 0) {
    if (!str || !*str) return default_val;
    return std::atoi(str);
}

} // anonymous namespace

Widget::Widget(const char* id) {
    id_ = id ? id : "";
    style_id_ = id_;
}

Widget::~Widget() {
    *lifetime_ = false;
    // 通知父容器移除自己
    if (parent_) {
        parent_->remove_child(this);
    }
    // 先摘除子节点的父指针，再逐个析构：
    // 否则子节点析构时会反向调用 parent_->remove_child(this)，
    // 在遍历 children_ 的同时修改容器，导致迭代器失效/堆损坏。
    for (auto* child : children_) {
        if (child) child->parent_ = nullptr;
    }
    for (auto* child : children_) {
        delete child;
    }
    children_.clear();
}

void Widget::set_position(float x, float y) {
    position_x_ = x;
    position_y_ = y;
    layout_dirty_ = true;
    if (parent_) parent_->layout_dirty_ = true;
}

void Widget::set_size(float w, float h) {
    size_w_ = std::max(0.0f, w);
    size_h_ = std::max(0.0f, h);
    layout_dirty_ = true;
    if (parent_) parent_->layout_dirty_ = true;
}

void Widget::set_anchor(Anchor anchor) {
    anchor_ = anchor;
    layout_dirty_ = true;
}

void Widget::set_margin(const Margin& m) {
    margin_ = m;
    layout_dirty_ = true;
}

void Widget::set_padding(const Padding& p) {
    padding_ = p;
    layout_dirty_ = true;
}

void Widget::add_child(Widget* child) {
    if (!child || child == this) return;
    if (child->parent_ == this) return;
    if (child->parent_) {
        child->parent_->remove_child(child);
    }
    child->parent_ = this;
    children_.push_back(child);
    layout_dirty_ = true;
}

void Widget::remove_child(Widget* child) {
    auto it = std::find(children_.begin(), children_.end(), child);
    if (it != children_.end()) {
        children_.erase(it);
        if (child) child->parent_ = nullptr;
        layout_dirty_ = true;
    }
}

void Widget::set_flex_grow(float grow) {
    flex_grow_ = std::max(0.0f, grow);
    layout_dirty_ = true;
    if (parent_) parent_->layout_dirty_ = true;
}

void Widget::set_visible(bool visible) {
    visible_ = visible;
}

void Widget::set_opacity(float opacity) {
    opacity_ = std::clamp(opacity, 0.0f, 1.0f);
}

void Widget::set_scale(float scale) {
    scale_ = std::max(0.0f, scale);
}

void Widget::set_style(const Style& style) {
    style_ = style;
    style_dirty_ = true;
}

void Widget::set_style_class(const char* cls) {
    style_class_ = cls ? cls : "";
    style_dirty_ = true;
}

void Widget::set_style_id(const char* id) {
    style_id_ = id ? id : id_;
    style_dirty_ = true;
}

const Style& Widget::computed_style() const {
    // 样式表解析在 draw 前由 UIManager 统一写入 style_，
    // 这里直接返回当前生效样式
    return style_;
}

void Widget::compute_bounds(const Rect& parent_bounds) {
    float w = size_w_;
    float h = size_h_;
    if (w <= 0.0f) w = preferred_width();
    if (h <= 0.0f) h = preferred_height(w);

    const float ml = margin_.left;
    const float mt = margin_.top;
    const float mr = margin_.right;
    const float mb = margin_.bottom;

    float x = parent_bounds.x + position_x_ + ml;
    float y = parent_bounds.y + position_y_ + mt;

    switch (anchor_) {
        case Anchor::TopLeft:
            break;
        case Anchor::TopCenter:
            x = parent_bounds.x + parent_bounds.w * 0.5f - w * 0.5f + position_x_;
            break;
        case Anchor::TopRight:
            x = parent_bounds.x + parent_bounds.w - w - mr + position_x_;
            break;
        case Anchor::CenterLeft:
            y = parent_bounds.y + parent_bounds.h * 0.5f - h * 0.5f + position_y_;
            break;
        case Anchor::Center:
            x = parent_bounds.x + parent_bounds.w * 0.5f - w * 0.5f + position_x_;
            y = parent_bounds.y + parent_bounds.h * 0.5f - h * 0.5f + position_y_;
            break;
        case Anchor::CenterRight:
            x = parent_bounds.x + parent_bounds.w - w - mr + position_x_;
            y = parent_bounds.y + parent_bounds.h * 0.5f - h * 0.5f + position_y_;
            break;
        case Anchor::BottomLeft:
            y = parent_bounds.y + parent_bounds.h - h - mb + position_y_;
            break;
        case Anchor::BottomCenter:
            x = parent_bounds.x + parent_bounds.w * 0.5f - w * 0.5f + position_x_;
            y = parent_bounds.y + parent_bounds.h - h - mb + position_y_;
            break;
        case Anchor::BottomRight:
            x = parent_bounds.x + parent_bounds.w - w - mr + position_x_;
            y = parent_bounds.y + parent_bounds.h - h - mb + position_y_;
            break;
        case Anchor::Stretch:
            x = parent_bounds.x + ml;
            y = parent_bounds.y + mt;
            w = std::max(0.0f, parent_bounds.w - ml - mr);
            if (size_h_ <= 0.0f) {
                h = std::max(0.0f, parent_bounds.h - mt - mb);
            }
            break;
    }
    bounds_ = {x, y, w, h};
    layout_dirty_ = false;
}

bool Widget::hit_test(float x, float y) const {
    return visible_ && enabled_ && bounds_.contains(x, y);
}

void Widget::set_enabled(bool enabled) {
    enabled_ = enabled;
    style_dirty_ = true;
    if (!enabled_) {
        state_hovered_ = false;
        state_pressed_ = false;
    }
}

void Widget::draw(Renderer* renderer) {
    if (!visible_ || bounds_.w <= 0.0f || bounds_.h <= 0.0f) return;
    auto* r2d = renderer->renderer2d();
    if (!r2d) return;

    const Style& s = style_;
    float alpha = opacity_ * s.opacity;
    if (!enabled_) alpha *= 0.4f;

    Color bg = s.background;
    if (state_pressed_) bg = s.background_pressed;
    else if (state_hovered_) bg = s.background_hover;

    if (alpha > 0.0f) {
        if (s.border_width > 0.0f) {
            // 先画外圈圆角矩形（边框色），再画内缩背景（背景色），实现圆角边框
            const float bw = s.border_width;
            detail::draw_rounded_rect(r2d, bounds_.x, bounds_.y, bounds_.w, bounds_.h,
                                      s.border_radius, detail::with_alpha(s.border_color, alpha));
            detail::draw_rounded_rect(r2d, bounds_.x + bw, bounds_.y + bw,
                                      bounds_.w - bw * 2.0f, bounds_.h - bw * 2.0f,
                                      std::max(0.0f, s.border_radius - bw),
                                      detail::with_alpha(bg, alpha));
        } else {
            detail::draw_rounded_rect(r2d, bounds_.x, bounds_.y, bounds_.w, bounds_.h,
                                      s.border_radius, detail::with_alpha(bg, alpha));
        }
    }

    // 焦点环
    if (state_focused_ && enabled_) {
        detail::draw_focus_ring(r2d, bounds_.x, bounds_.y, bounds_.w, bounds_.h,
                                s.border_radius,
                                detail::with_alpha(Color(0.4f, 0.6f, 1.0f, 0.8f), alpha));
    }
}

void Widget::update(float dt) {
    // 动画由 UIManager 统一管理生命周期
    for (auto* anim : animations_) {
        if (anim) anim->update(dt);
    }
    // 不在这里删除动画——UIManager::update() 负责清理
}

void Widget::add_animation(Animation* anim) {
    if (!anim) return;
    if (std::find(animations_.begin(), animations_.end(), anim) == animations_.end()) {
        animations_.push_back(anim);
    }
}

void Widget::remove_animation(Animation* anim) {
    animations_.erase(std::remove(animations_.begin(), animations_.end(), anim),
                      animations_.end());
}

// ============================================================================
// find_element_by_id — 递归查找子控件
// ============================================================================
Widget* Widget::find_element_by_id(const char* id) {
    // 检查自身
    if (id_ == id) return this;
    // 递归查找子控件
    for (auto* child : children_) {
        Widget* found = child->find_element_by_id(id);
        if (found) return found;
    }
    return nullptr;
}

const Widget* Widget::find_element_by_id(const char* id) const {
    if (id_ == id) return this;
    for (const auto* child : children_) {
        const Widget* found = child->find_element_by_id(id);
        if (found) return found;
    }
    return nullptr;
}

// ============================================================================
// set_property — 通用属性注入
// ============================================================================
bool Widget::set_property(const char* key, const char* value) {
    if (!key || !value) return false;

    // 通用属性：所有控件共享
    if (std::strcmp(key, "id") == 0) {
        id_ = value;
        style_id_ = value;
        return true;
    }
    if (std::strcmp(key, "layout") == 0) {
        // 布局属性由 Panel 等容器处理，基类忽略
        return true;
    }
    if (std::strcmp(key, "padding") == 0) {
        Padding p;
        parse_margin(value, p);
        set_padding(p);
        return true;
    }
    if (std::strcmp(key, "spacing") == 0) {
        // 由 Panel 处理
        return true;
    }
    if (std::strcmp(key, "width") == 0) {
        float w = parse_float(value);
        if (w > 0.0f) set_size(w, size_h_);
        return true;
    }
    if (std::strcmp(key, "height") == 0) {
        float h = parse_float(value);
        if (h > 0.0f) set_size(size_w_, h);
        return true;
    }
    if (std::strcmp(key, "flex-grow") == 0) {
        set_flex_grow(parse_float(value, 0.0f));
        return true;
    }
    if (std::strcmp(key, "margin") == 0) {
        Margin m;
        parse_margin(value, m);
        set_margin(m);
        return true;
    }
    if (std::strcmp(key, "align") == 0) {
        set_anchor(parse_anchor(value));
        return true;
    }
    if (std::strcmp(key, "visible") == 0) {
        set_visible(parse_bool(value));
        return true;
    }
    if (std::strcmp(key, "enabled") == 0) {
        set_enabled(parse_bool(value));
        return true;
    }
    if (std::strcmp(key, "style") == 0) {
        set_style_class(value);
        // 检查是否有匹配的样式预设，有则自动应用
        if (UIStylePresetSet::has(value)) {
            UIStylePresetSet::get(value).apply_to(style_);
        }
        return true;
    }
    if (std::strcmp(key, "bgcolor") == 0) {
        Color c;
        if (parse_color_from_string(value, c)) {
            style_.background = c;
            style_.has_background = true;
        }
        return true;
    }
    if (std::strcmp(key, "color") == 0) {
        Color c;
        if (parse_color_from_string(value, c)) {
            style_.color = c;
        }
        return true;
    }
    if (std::strcmp(key, "font-size") == 0) {
        style_.font_size = parse_float(value, 16.0f);
        return true;
    }
    if (std::strcmp(key, "border-radius") == 0) {
        style_.border_radius = parse_float(value, 0.0f);
        return true;
    }
    if (std::strcmp(key, "border-width") == 0) {
        style_.border_width = parse_float(value, 0.0f);
        return true;
    }
    if (std::strcmp(key, "border-color") == 0) {
        Color c;
        if (parse_color_from_string(value, c)) {
            style_.border_color = c;
        }
        return true;
    }
    if (std::strcmp(key, "opacity") == 0) {
        set_opacity(parse_float(value, 1.0f));
        return true;
    }
    if (std::strcmp(key, "title") == 0) {
        // 标题属性，由 Window 等处理
        return true;
    }

    // 事件属性
    if (std::strcmp(key, "onClick") == 0) {
        set_event_callback("click", value);
        return true;
    }
    if (std::strcmp(key, "onChange") == 0) {
        set_event_callback("change", value);
        return true;
    }
    if (std::strcmp(key, "onFocus") == 0) {
        set_event_callback("focus", value);
        return true;
    }
    if (std::strcmp(key, "onBlur") == 0) {
        set_event_callback("blur", value);
        return true;
    }

    return false; // 未识别的属性
}

// ============================================================================
// 事件回调管理
// ============================================================================
const std::string& Widget::event_callback(const char* event_name) const {
    static const std::string empty;
    auto it = event_callbacks_.find(event_name ? event_name : "");
    return it != event_callbacks_.end() ? it->second : empty;
}

void Widget::set_event_callback(const char* event_name, const char* callback) {
    if (event_name && callback) {
        event_callbacks_[event_name] = callback;
    }
}

} // namespace GryceEngineUtils::ui
