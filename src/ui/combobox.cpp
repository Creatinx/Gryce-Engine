#include "GryceEngineUtils/ui/combobox.h"

#include <algorithm>
#include <cstring>

#include "GryceEngineUtils/ui/button.h"
#include "ui_draw.h"

namespace GryceEngineUtils::ui {

namespace {
constexpr float kItemHeight = 26.0f;
}

ComboBox::ComboBox(const char* id) : Widget(id) {
    style_.has_background = true;
    style_.background = Color(0.10f, 0.11f, 0.14f, 1.0f);
    style_.background_hover = Color(0.14f, 0.15f, 0.19f, 1.0f);
    style_.background_pressed = style_.background_hover;

    popup_ = new Panel("combobox_popup");
    popup_->set_layout(LayoutType::Vertical);
    popup_->set_spacing(0.0f);
    popup_->set_visible(false);
    popup_->set_background(Color(0.10f, 0.11f, 0.14f, 0.98f));
    add_child(popup_);
}

void ComboBox::toggle_popup() {
    if (!enabled_) return;
    open_ = !open_;
    if (open_) relayout_popup();
    if (popup_) popup_->set_visible(open_);
}

void ComboBox::add_item(const char* text) {
    items_.emplace_back(text ? text : "");
    auto* item = new Button(("combo_item_" + std::to_string(items_.size() - 1)).c_str(),
                            text ? text : "");
    item->set_style_class("combo-item");
    item->style().background = Color(0.12f, 0.13f, 0.16f, 1.0f);
    item->style().background_hover = Color(0.24f, 0.30f, 0.40f, 1.0f);
    item->style().background_pressed = item->style().background_hover;
    item->style().padding = Padding{10.0f, 4.0f, 10.0f, 4.0f};
    item->style().text_align = TextAlign::Left;
    popup_->add_child(item);
}

void ComboBox::clear_items() {
    items_.clear();
    selected_ = -1;
    // popup_ 的子项由 popup_ 析构时释放；这里清空子项列表
    while (!popup_->children().empty()) {
        auto* child = popup_->children().back();
        popup_->remove_child(child);
        delete child;
    }
    popup_->mark_layout_dirty();
}

void ComboBox::set_selected_index(int index) {
    if (index < -1 || index >= static_cast<int>(items_.size())) return;
    selected_ = index;
    if (selected_ >= 0) {
        on_selected.emit(selected_);
        on_text_selected.emit(items_[static_cast<size_t>(selected_)].c_str());
    }
}

const std::string& ComboBox::selected_text() const {
    static const std::string kEmpty;
    return selected_ >= 0 ? items_[static_cast<size_t>(selected_)] : kEmpty;
}

void ComboBox::relayout_popup() {
    if (!popup_) return;
    const int n = static_cast<int>(items_.size());
    const float max_w = std::max(bounds_.w, 160.0f);
    const float h = static_cast<float>(n) * kItemHeight;
    popup_->set_bounds(Rect{bounds_.x, bounds_.y + bounds_.h, max_w, h});
    int i = 0;
    for (auto* child : popup_->children()) {
        child->set_bounds(Rect{bounds_.x, bounds_.y + bounds_.h + static_cast<float>(i) * kItemHeight,
                               max_w, kItemHeight});
        ++i;
    }
}

void ComboBox::draw(Renderer* renderer) {
    if (!visible_) return;
    Widget::draw(renderer); // 背景
    auto* r2d = renderer->renderer2d();
    if (!r2d) return;

    float alpha = opacity_ * style_.opacity;
    if (!enabled_) alpha *= 0.4f;
    // 文本
    const std::string shown = selected_ >= 0 ? selected_text() : std::string();
    const Padding text_pad{10.0f, 0.0f, 36.0f, 0.0f};
    detail::draw_label_text(r2d, bounds_, style_, shown, alpha, text_pad);
    // 下拉箭头（三角形）
    const float ax = bounds_.x + bounds_.w - 18.0f;
    const float ay = bounds_.y + bounds_.h * 0.5f;
    std::vector<math::Vector2f> tri = {
        {ax - 5.0f, ay - 2.0f}, {ax + 5.0f, ay - 2.0f}, {ax, ay + 4.0f}};
    r2d->draw_polygon(tri, detail::with_alpha(Color(0.75f, 0.76f, 0.8f, 1.0f), alpha));

    if (open_) relayout_popup();
}

void ComboBox::on_global_click(Widget* clicked) {
    if (!open_) return;
    if (clicked == this) return; // 开合由 UIManager 处理
    Widget* cur = clicked;
    while (cur) {
        if (cur == popup_) {
            // 命中弹出项 → 选中并关闭
            const auto& children = popup_->children();
            for (size_t i = 0; i < children.size(); ++i) {
                if (children[i] == clicked) {
                    set_selected_index(static_cast<int>(i));
                    break;
                }
            }
            open_ = false;
            if (popup_) popup_->set_visible(false);
            return;
        }
        cur = cur->parent();
    }
    open_ = false;
    if (popup_) popup_->set_visible(false);
}

bool ComboBox::hit_test(float x, float y) const {
    if (visible_ && bounds_.contains(x, y)) return true;
    if (open_ && popup_ && popup_->visible() && popup_->bounds().contains(x, y)) {
        return true;
    }
    return false;
}

float ComboBox::preferred_height(float width) const {
    (void)width;
    return std::max(30.0f, style_.font_size * 1.8f);
}

bool ComboBox::set_property(const char* key, const char* value) {
    return Widget::set_property(key, value);
}

} // namespace GryceEngineUtils::ui
