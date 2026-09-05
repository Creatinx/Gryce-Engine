#include "GryceEngineUtils/ui/menubar.h"

#include <algorithm>

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

namespace {
constexpr float kItemHeight = 28.0f;
constexpr float kItemPadX = 18.0f;
}

// ---------------------------------------------------------------------------
// MenuItem
// ---------------------------------------------------------------------------
MenuItem::MenuItem(const char* id, const char* text)
    : Button(id, text) {
    style_.has_background = true;
    style_.background = Color(0.16f, 0.17f, 0.20f, 1.0f);
    style_.background_hover = Color(0.26f, 0.32f, 0.42f, 1.0f);
    style_.background_pressed = style_.background_hover;
    style_.padding = Padding{10.0f, 4.0f, 10.0f, 4.0f};
}

// ---------------------------------------------------------------------------
// MenuBar
// ---------------------------------------------------------------------------
MenuBar::MenuBar(const char* id) : Widget(id) {
    style_.has_background = true;
    style_.background = Color(0.12f, 0.13f, 0.16f, 1.0f);
    style_.background_hover = style_.background;
    style_.background_pressed = style_.background;

    popup_panel_ = new Panel("menubar_popup");
    popup_panel_->set_layout(LayoutType::Vertical);
    popup_panel_->set_spacing(0.0f);
    popup_panel_->set_visible(false);
    popup_panel_->set_background(Color(0.10f, 0.11f, 0.14f, 0.98f));
    add_child(popup_panel_);
}

MenuItem* MenuBar::add_menu(const char* id, const char* text) {
    auto* top = new MenuItem(id, text);
    top->set_top_level(true);
    top_items_.push_back(top);
    add_child(top);
    top->on_pressed.connect([this, top]() { open_menu(top); });
    relayout_items();
    return top;
}

MenuItem* MenuBar::add_item(MenuItem* top_menu, const char* id, const char* text,
                            std::function<void()> callback) {
    auto* item = new MenuItem(id, text);
    if (callback) {
        item->on_activate.connect(std::move(callback));
    }
    popup_panel_->add_child(item);
    if (top_menu) {
        // 记录归属：用于命中时定位（通过 popup 子项顺序即可，这里仅保证存在）
        (void)top_menu;
    }
    relayout_items();
    return item;
}

Divider* MenuBar::add_separator(MenuItem* top_menu) {
    (void)top_menu;
    auto* div = new Divider("menu_sep");
    div->set_vertical(false);
    div->set_size(0.0f, 4.0f);
    popup_panel_->add_child(div);
    relayout_items();
    return div;
}

void MenuBar::relayout_items() {
    if (bounds_.w <= 0.0f) return;
    float x = bounds_.x + 4.0f;
    for (auto* top : top_items_) {
        const float w = detail::text_width(top->text(), top->style().font_size) + kItemPadX * 2.0f;
        top->set_bounds(Rect{x, bounds_.y, w, bounds_.h});
        x += w + 2.0f;
    }

    // 弹出菜单：定位到打开的顶级项下方
    if (open_item_ && popup_panel_) {
        const Rect& top_b = open_item_->bounds();
        const auto& items = popup_panel_->children();
        float max_w = 120.0f;
        for (auto* it : items) {
            if (auto* mi = dynamic_cast<MenuItem*>(it)) {
                max_w = std::max(max_w, detail::text_width(mi->text(), mi->style().font_size) + kItemPadX * 2.0f);
            }
        }
        float y = bounds_.y + bounds_.h;
        for (auto* it : items) {
            if (auto* mi = dynamic_cast<MenuItem*>(it)) {
                mi->set_bounds(Rect{top_b.x, y, max_w, kItemHeight});
                y += kItemHeight;
            } else if (auto* div = dynamic_cast<Divider*>(it)) {
                div->set_bounds(Rect{top_b.x + 6.0f, y + 1.0f, max_w - 12.0f, 2.0f});
                y += 4.0f;
            }
        }
        popup_panel_->set_bounds(Rect{top_b.x, bounds_.y + bounds_.h, max_w, y - (bounds_.y + bounds_.h)});
    }
}

void MenuBar::open_menu(MenuItem* top) {
    if (open_ && open_item_ == top) {
        close_menu();
        return;
    }
    open_item_ = top;
    open_ = true;
    if (popup_panel_) {
        popup_panel_->set_visible(true);
    }
    relayout_items();
}

void MenuBar::close_menu() {
    open_ = false;
    open_item_ = nullptr;
    if (popup_panel_) {
        popup_panel_->set_visible(false);
    }
}

void MenuBar::draw(Renderer* renderer) {
    if (!visible_) return;
    Widget::draw(renderer); // 背景
    relayout_items();
}

void MenuBar::on_global_click(Widget* clicked) {
    if (!clicked) {
        close_menu();
        return;
    }
    // 点击顶级菜单项：状态已由 on_pressed 切换，这里保持不动
    for (auto* top : top_items_) {
        if (clicked == top) return;
    }
    // 点击弹出菜单项：触发并关闭
    if (popup_panel_ && popup_panel_->visible()) {
        Widget* cur = clicked;
        while (cur) {
            if (cur == popup_panel_) {
                if (auto* item = dynamic_cast<MenuItem*>(clicked)) {
                    item->on_activate.emit();
                    close_menu();
                }
                return;
            }
            cur = cur->parent();
        }
    }
    // 点击其他区域：关闭
    close_menu();
}

bool MenuBar::hit_test(float x, float y) const {
    if (visible_ && bounds_.contains(x, y)) return true;
    // 弹出菜单在 MenuBar 下方，父 bounds 之外也要可命中
    if (open_ && popup_panel_ && popup_panel_->visible() &&
        popup_panel_->bounds().contains(x, y)) {
        return true;
    }
    return false;
}

} // namespace GryceEngineUtils::ui
