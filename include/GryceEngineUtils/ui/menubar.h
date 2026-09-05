#pragma once

#include <functional>
#include <vector>

#include "GryceEngineUtils/ui/button.h"
#include "GryceEngineUtils/ui/divider.h"
#include "GryceEngineUtils/ui/panel.h"
#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

class MenuBar;

// MenuItem — 菜单项（顶级菜单或下拉项）
class MenuItem : public Button {
public:
    MenuItem(const char* id, const char* text);

    // 触发回调（下拉项被点击时）
    Signal<void()> on_activate;
    // 是否为顶级菜单项（带下拉箭头）
    void set_top_level(bool top) { top_level_ = top; }
    bool is_top_level() const { return top_level_; }

    const char* type_name() const override { return "MenuItem"; }

private:
    bool top_level_ = false;
    friend class MenuBar;
};

// MenuBar — 菜单栏：顶级菜单横向排布，点击展开下拉菜单
class MenuBar : public Widget {
public:
    explicit MenuBar(const char* id = "menubar");

    // 添加顶级菜单，返回顶级 MenuItem（继续 add_item 挂子项）
    MenuItem* add_menu(const char* id, const char* text);
    // 在指定顶级菜单下添加菜单项，返回子 MenuItem
    MenuItem* add_item(MenuItem* top_menu, const char* id, const char* text,
                       std::function<void()> callback = nullptr);
    // 添加分隔线到指定菜单
    Divider* add_separator(MenuItem* top_menu);

    void draw(Renderer* renderer) override;
    void on_global_click(Widget* clicked) override;
    bool hit_test(float x, float y) const override;
    const char* type_name() const override { return "MenuBar"; }
    float preferred_height(float width) const override { return 30.0f; }

    // 打开/关闭指定顶级菜单
    void open_menu(MenuItem* top);
    void close_menu();
    bool is_open() const { return open_ && popup_panel_ && popup_panel_->visible(); }

private:
    void relayout_items();

    std::vector<MenuItem*> top_items_;
    Panel* popup_panel_ = nullptr;
    MenuItem* open_item_ = nullptr;
    bool open_ = false;
};

} // namespace GryceEngineUtils::ui
