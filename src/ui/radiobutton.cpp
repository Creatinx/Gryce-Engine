#include "GryceEngineUtils/ui/radiobutton.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

namespace {

// 组注册表：组名 → (存活标记, RadioButton*)，弱引用避免悬垂
using GroupEntry = std::pair<std::weak_ptr<bool>, RadioButton*>;
std::unordered_map<std::string, std::vector<GroupEntry>>& group_registry() {
    static std::unordered_map<std::string, std::vector<GroupEntry>> registry;
    return registry;
}

} // namespace

RadioButton::RadioButton(const char* id, const char* label)
    : Widget(id), label_(label ? label : "") {}

RadioButton::~RadioButton() {
    // 从组注册表移除（惰性清理：条目带弱引用，下一次遍历时跳过）
}

void RadioButton::set_group(const char* group) {
    group_ = group ? group : "";
    group_registry()[group_].emplace_back(group_lifetime_, this);
}

void RadioButton::set_checked(bool checked) {
    if (checked_ == checked) return;
    if (checked) {
        uncheck_others();
    }
    checked_ = checked;
    on_toggled.emit(checked_);
}

void RadioButton::toggle() {
    if (checked_ || !enabled_) return; // 单选按钮不允许点击取消，禁用时不可操作
    set_checked(true);
}

void RadioButton::uncheck_others() {
    auto& entries = group_registry()[group_];
    for (auto& entry : entries) {
        if (entry.first.expired()) continue;
        RadioButton* other = entry.second;
        if (other && other != this && other->checked_) {
            other->checked_ = false;
            other->on_toggled.emit(false);
        }
    }
    // 清理过期条目，避免注册表无限增长
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [](const GroupEntry& e) { return e.first.expired(); }),
        entries.end());
    if (entries.empty()) {
        group_registry().erase(group_);
    }
}

void RadioButton::set_label(const char* label) {
    label_ = label ? label : "";
}

void RadioButton::draw(Renderer* renderer) {
    if (!visible_) return;
    auto* r2d = renderer->renderer2d();
    if (!r2d) return;

    const Style& s = style_;
    float alpha = opacity_ * s.opacity;
    if (!enabled_) alpha *= 0.4f;
    const float circle = std::min(16.0f, bounds_.h);
    const float cx = bounds_.x + circle * 0.5f;
    const float cy = bounds_.y + bounds_.h * 0.5f;
    const float r = circle * 0.5f;

    const Color ring = detail::widget_background(s, state_hovered_, state_pressed_);
    r2d->draw_circle(cx, cy, r, 24, detail::with_alpha(ring, alpha));
    if (checked_) {
        r2d->draw_circle(cx, cy, std::max(3.0f, r * 0.45f), 16,
                         detail::with_alpha(s.color, alpha));
    }

    // 标签
    const float text_x = bounds_.x + circle + 8.0f;
    const float text_w = std::max(0.0f, bounds_.w - circle - 8.0f);
    const Rect label_bounds{text_x, bounds_.y, text_w, bounds_.h};
    const Padding label_pad{0.0f, 0.0f, 0.0f, 0.0f};
    detail::draw_label_text(r2d, label_bounds, s, label_, alpha, label_pad);

    // 焦点环
    if (state_focused_ && enabled_) {
        detail::draw_focus_ring(r2d, bounds_.x, bounds_.y, bounds_.w, bounds_.h,
                                circle * 0.5f,
                                detail::with_alpha(Color(0.4f, 0.6f, 1.0f, 0.8f), alpha));
    }
}

float RadioButton::preferred_height(float width) const {
    (void)width;
    return std::max(24.0f, style_.font_size * 1.6f);
}

} // namespace GryceEngineUtils::ui
