#pragma once

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include "ui/ui_widgets.h"

namespace gryce_engine::components {

inline std::vector<std::string> ui_split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}

class UISplitter : public NodeUI {
public:
    bool vertical = true;
    float ratio = 0.25f;
    float min_ratio = 0.08f;
    float max_ratio = 0.92f;
    math::Vector4f color = math::Vector4f(0.22f, 0.25f, 0.30f, 1.0f);

    UISplitter() {
        size = math::Vector2f(6.0f, 600.0f);
        min_size = size;
        focusable = true;
        background_role = ui::UIThemeColor::Border;
    }
    const char* type() const override { return "UISplitter"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UISplitter)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["vertical"] = vertical;
        out["ratio"] = ratio;
        out["min_ratio"] = min_ratio;
        out["max_ratio"] = max_ratio;
        out["color"] = {color.x, color.y, color.z, color.w};
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        vertical = in.value("vertical", true);
        ratio = in.value("ratio", 0.25f);
        min_ratio = in.value("min_ratio", 0.08f);
        max_ratio = in.value("max_ratio", 0.92f);
        const auto c = in.value("color", std::vector<float>{0.22f, 0.25f, 0.30f, 1.0f});
        if (c.size() >= 4) color = math::Vector4f(c[0], c[1], c[2], c[3]);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::PointerDown) {
            dragging = true;
            return true;
        }
        if (event.type == ui::UIEventType::PointerUp) {
            dragging = false;
            return true;
        }
        if (event.type == ui::UIEventType::PointerMove && dragging) {
            const float extent = vertical ? event.parent_rect.w : event.parent_rect.h;
            if (extent > 1.0f) {
                const float p = vertical
                                    ? (event.position.x - event.parent_rect.x) / extent
                                    : (event.position.y - event.parent_rect.y) / extent;
                ratio = std::clamp(p, min_ratio, max_ratio);
            }
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        uint32_t rgba = themed_color(
            background_role,
            ui::ui_color(color.x, color.y, color.z, color.w));
        if (dragging) {
            rgba = themed_color(background_role,
                                ui::ui_theme_color(ui::UIThemeColor::Accent));
        }
        // 分隔条很窄，圆角取"厚度的一半"让它成为一条胶囊，而不是一根方块。
        const float thickness = std::min(rect.w, rect.h);
        draw_list.add_round_rect(rect, ui::ui_pill_radius(thickness), rgba);
    }

private:
    bool dragging = false;
};

class UITabBar : public NodeUI {
public:
    std::string items = "Scene";
    int selected = 0;
    float tab_height = 32.0f;
    math::Vector4f background = math::Vector4f(0.07f, 0.08f, 0.11f, 1.0f);
    math::Vector4f selected_color = math::Vector4f(0.20f, 0.34f, 0.55f, 1.0f);

    UITabBar() {
        size = math::Vector2f(300.0f, 32.0f);
        min_size = size;
        focusable = true;
        // 内容视图与所在卡片同层：比页面底再深一档会让列表看起来是凹陷的。
        background_role = ui::UIThemeColor::PanelRaised;
        accent_role = ui::UIThemeColor::Selection;
    }
    const char* type() const override { return "UITabBar"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UITabBar)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["items"] = items;
        out["selected"] = selected;
        out["tab_height"] = tab_height;
        out["background"] = {background.x, background.y, background.z, background.w};
        out["selected_color"] = {selected_color.x, selected_color.y,
                                 selected_color.z, selected_color.w};
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        items = in.value("items", "Scene");
        selected = in.value("selected", 0);
        tab_height = in.value("tab_height", 28.0f);
        auto read4 = [&](const char* key, math::Vector4f& c) {
            const auto v = in.value(key, std::vector<float>{c.x, c.y, c.z, c.w});
            if (v.size() >= 4) c = math::Vector4f(v[0], v[1], v[2], v[3]);
        };
        read4("background", background);
        read4("selected_color", selected_color);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type != ui::UIEventType::PointerDown &&
            event.type != ui::UIEventType::Click) return false;
        const auto tab_values = tabs();
        if (tab_values.empty() || event.layout_rect.w <= 0.0f) return false;
        const float tab_w = event.layout_rect.w / static_cast<float>(tab_values.size());
        selected = std::clamp(
            static_cast<int>((event.position.x - event.layout_rect.x) / tab_w),
            0, static_cast<int>(tab_values.size()) - 1);
        return true;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        const auto labels = tabs();
        if (labels.empty()) return;
        // Fluent 的 TabView：标签本身没有底，选中项顶部两角圆、底下一条 2px
        // 强调色指示条。整条用一条底部分隔线收边，不再铺一块实心色块——
        // 实心色块是"分段控件"，不是 Tab。
        const float h = std::min(tab_height, rect.h);
        const float tab_w = rect.w / static_cast<float>(labels.size());
        const float radius = ui::ui_theme_radius(ui::UIRadiusTier::Medium);
        const float corners[4] = {radius, radius, 0.0f, 0.0f};
        const uint32_t accent = themed_color(
            accent_role, ui::ui_color(selected_color.x, selected_color.y,
                                      selected_color.z, selected_color.w));
        const uint32_t divider = ui::ui_with_alpha(
            ui::ui_theme_color(ui::UIThemeColor::ControlStroke), 1.0f);
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const ui::UIRect tab{rect.x + tab_w * static_cast<float>(i), rect.y,
                                 tab_w, h};
            const bool is_selected = static_cast<int>(i) == selected;
            if (is_selected) {
                // 选中标签的底色要比所在卡片再亮一档，否则"选中"只剩指示条
                // 一条细线可读。
                draw_list.add_round_rect(tab, corners,
                    ui::ui_theme_mix(ui::ui_theme_color(ui::UIThemeColor::PanelRaised),
                                     ui::ui_theme_color(ui::UIThemeColor::Text), 0.07f));
                const float bar_w = std::max(12.0f, tab.w - 24.0f);
                draw_list.add_rect({tab.x + (tab.w - bar_w) * 0.5f,
                                    tab.bottom() - 2.0f, bar_w, 2.0f}, accent);
            }
            draw_list.add_text({tab.x + 8.0f, tab.y, tab.w - 16.0f, tab.h},
                               labels[i], 14.0f,
                               is_selected
                                   ? ui::ui_theme_color(ui::UIThemeColor::Text)
                                   : ui::ui_theme_color(ui::UIThemeColor::TextMuted),
                               ui::UITextAlign::Center);
        }
        draw_list.add_rect({rect.x, rect.y + h - 1.0f, rect.w, 1.0f}, divider);
    }

    std::vector<std::string> tabs() const { return ui_split_lines(items); }
};

class UIComboBox : public NodeUI {
public:
    std::string items = "Option A\nOption B";
    int selected = 0;
    bool open = false;
    math::Vector4f background = math::Vector4f(0.16f, 0.19f, 0.24f, 1.0f);
    math::Vector4f popup_background = math::Vector4f(0.09f, 0.11f, 0.15f, 1.0f);

    UIComboBox() {
        size = math::Vector2f(180.0f, 32.0f);
        min_size = size;
        focusable = true;
        background_role = ui::UIThemeColor::Input;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UIComboBox"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIComboBox)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["items"] = items;
        out["selected"] = selected;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        items = in.value("items", items);
        selected = in.value("selected", 0);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::Click) {
            open = !open;
            return true;
        }
        if (event.type == ui::UIEventType::PointerDown && open) {
            const auto values = ui_split_lines(items);
            if (event.position.y > event.layout_rect.bottom()) {
                const int index = static_cast<int>(
                    (event.position.y - event.layout_rect.bottom() - 4.0f) /
                    std::max(1.0f, size.y));
                if (index >= 0 && index < static_cast<int>(values.size())) {
                    selected = index;
                    open = false;
                    return true;
                }
            }
        }
        if (event.type == ui::UIEventType::KeyDown &&
            event.key == static_cast<int>(ui::UIKey::Enter)) {
            open = !open;
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        // Fluent 的 DropDownButton：控件填充 + 发丝边 + 右侧 chevron。展开时
        // 箭头翻转朝上，弹层是比控件更亮一档的卡片表面。
        const float radius = ui::ui_resolve_radius(corner_radius, radius_tier);
        const auto values = ui_split_lines(items);
        ui::UISurface surface;
        surface.rect = rect;
        ui::ui_fill_radii(radius, surface.radii);
        surface.fill = ui::ui_control_fill(hover_amount(), press_amount());
        surface.border = ui::ui_control_stroke(hover_amount(), focus_amount());
        surface.border_width = 1.0f;
        ui::ui_paint_surface(draw_list, surface);

        if (!values.empty()) {
            const int index = std::clamp(selected, 0, static_cast<int>(values.size()) - 1);
            draw_list.add_text({rect.x + 11.0f, rect.y, rect.w - 40.0f, rect.h},
                               values[index], 14.0f,
                               ui::ui_theme_color(ui::UIThemeColor::Text));
        }
        ui::ui_paint_chevron(draw_list, rect.right() - 16.0f, rect.y + rect.h * 0.5f,
                             9.0f, ui::ui_theme_color(ui::UIThemeColor::TextMuted),
                             1.4f, open);
        if (open) {
            // 整个下拉是一块浮层：先铺一层圆角底（带投影），行与行之间不再
            // 各自铺底，否则圆角会被中间的行切成方块。
            const float flyout_radius = ui::ui_theme_radius(ui::UIRadiusTier::Medium);
            const ui::UIRect panel{rect.x, rect.bottom() + 4.0f,
                                   rect.w, size.y * static_cast<float>(values.size()) + 8.0f};
            ui::UISurface flyout;
            flyout.rect = panel;
            ui::ui_fill_radii(flyout_radius, flyout.radii);
            flyout.fill = themed_color(
                UIThemeColor::PanelRaised,
                ui::ui_color(popup_background.x, popup_background.y,
                             popup_background.z, popup_background.w));
            flyout.border = ui::ui_with_alpha(
                ui::ui_theme_color(ui::UIThemeColor::ControlStroke), 1.0f);
            flyout.border_width = 1.0f;
            flyout.elevation = 12.0f;
            ui::ui_paint_surface(draw_list, flyout);
            for (std::size_t i = 0; i < values.size(); ++i) {
                const ui::UIRect row{panel.x + 4.0f,
                                     panel.y + 4.0f + size.y * static_cast<float>(i),
                                     panel.w - 8.0f, size.y};
                if (static_cast<int>(i) == selected) {
                    draw_list.add_round_rect(row, ui::ui_theme_radius(ui::UIRadiusTier::Small),
                                             ui::ui_theme_color(ui::UIThemeColor::ControlFillHover));
                }
                draw_list.add_text({row.x + 9.0f, row.y, row.w - 18.0f, row.h},
                                   values[i], 14.0f,
                                   ui::ui_theme_color(ui::UIThemeColor::Text));
            }
        }
        ui::ui_paint_focus_ring(draw_list, rect, radius, focus_amount());
    }
};

class UIProgressBar : public NodeUI {
public:
    float value = 0.0f;
    float minimum = 0.0f;
    float maximum = 1.0f;
    math::Vector4f track_color = math::Vector4f(0.12f, 0.14f, 0.18f, 1.0f);
    math::Vector4f fill_color = math::Vector4f(0.30f, 0.66f, 0.95f, 1.0f);

    UIProgressBar() {
        size = math::Vector2f(180.0f, 18.0f);
        min_size = size;
        accent_role = ui::UIThemeColor::Accent;
    }
    const char* type() const override { return "UIProgressBar"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIProgressBar)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["value"] = value;
        out["minimum"] = minimum;
        out["maximum"] = maximum;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        value = in.value("value", 0.0f);
        minimum = in.value("minimum", 0.0f);
        maximum = in.value("maximum", 1.0f);
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        // Fluent 的进度条是 4px 细条：底色是 40% 的轨道色，已完成段是强调色，
        // 两端全圆。没有外框、没有凹槽——大块填充反而像老式进度条。
        const float thickness = std::min(4.0f, rect.h);
        const ui::UIRect track{rect.x, rect.y + (rect.h - thickness) * 0.5f,
                               rect.w, thickness};
        const float pill = ui::ui_pill_radius(thickness);
        draw_list.add_round_rect(track, pill, ui::ui_with_alpha(
            themed_color(UIThemeColor::Track,
                         ui::ui_color(track_color.x, track_color.y,
                                      track_color.z, track_color.w)),
            0.75f));
        const float range = maximum - minimum;
        const float t = range == 0.0f ? 0.0f : std::clamp((value - minimum) / range, 0.0f, 1.0f);
        if (rect.w * t > 1.0f) {
            draw_list.add_round_rect({track.x, track.y, track.w * t, track.h}, pill,
                                     themed_color(accent_role,
                                         ui::ui_color(fill_color.x, fill_color.y,
                                                      fill_color.z, fill_color.w)));
        }
    }
};

class UIListView : public NodeUI {
public:
    std::string items = "Item";
    int selected = -1;
    int activated = -1;
    float row_height = 32.0f;
    math::Vector4f background = math::Vector4f(0.06f, 0.07f, 0.09f, 1.0f);
    math::Vector4f selected_color = math::Vector4f(0.20f, 0.34f, 0.55f, 1.0f);

    UIListView() {
        size = math::Vector2f(240.0f, 180.0f);
        min_size = math::Vector2f(80.0f, 40.0f);
        focusable = true;
        clip_contents = true;
        // 内容视图与所在卡片同层：比页面底再深一档会让列表看起来是凹陷的。
        background_role = ui::UIThemeColor::PanelRaised;
        accent_role = ui::UIThemeColor::Selection;
    }
    const char* type() const override { return "UIListView"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIListView)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["items"] = items;
        out["selected"] = selected;
        out["row_height"] = row_height;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        items = in.value("items", items);
        selected = in.value("selected", -1);
        row_height = in.value("row_height", 32.0f);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        const int count = static_cast<int>(ui_split_lines(items).size());
        if (event.type == ui::UIEventType::Click ||
            event.type == ui::UIEventType::PointerDown) {
            const int index = static_cast<int>(
                (event.position.y - event.layout_rect.y) / std::max(1.0f, row_height));
            if (index >= 0 && index < count) {
                selected = index;
                if (event.type == ui::UIEventType::Click) activated = selected;
            }
            return true;
        }
        if (event.type == ui::UIEventType::KeyDown) {
            if (event.key == static_cast<int>(ui::UIKey::Down)) {
                selected = std::min(count - 1, selected + 1);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Up)) {
                selected = std::max(0, selected - 1);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Enter)) {
                activated = selected;
                return true;
            }
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        // Fluent 的列表：容器是卡片表面，选中行不是整行铺满强调色，而是
        // "圆角淡强调底 + 左侧 3px 指示条"，鼠标扫过用更淡的一层。
        const float radius = ui::ui_resolve_radius(corner_radius, radius_tier);
        ui::UISurface surface;
        surface.rect = rect;
        ui::ui_fill_radii(radius, surface.radii);
        surface.fill = themed_color(
            background_role, ui::ui_color(background.x, background.y,
                                          background.z, background.w));
        surface.border = ui::ui_with_alpha(
            ui::ui_theme_color(ui::UIThemeColor::ControlStroke), 1.0f);
        surface.border_width = 1.0f;
        ui::ui_paint_surface(draw_list, surface);

        const auto values = ui_split_lines(items);
        const float row_radius = ui::ui_theme_radius(ui::UIRadiusTier::Small);
        const uint32_t accent = themed_color(
            accent_role, ui::ui_color(selected_color.x, selected_color.y,
                                      selected_color.z, selected_color.w));
        for (std::size_t i = 0; i < values.size(); ++i) {
            const ui::UIRect row{rect.x + 4.0f, rect.y + row_height * static_cast<float>(i),
                                 rect.w - 8.0f, row_height};
            if (row.y >= rect.bottom()) break;
            if (static_cast<int>(i) == selected) {
                draw_list.add_round_rect(row, row_radius,
                                         ui::ui_with_alpha(accent, 0.28f));
                draw_list.add_round_rect({row.x + 2.0f, row.y + row.h * 0.25f,
                                          3.0f, row.h * 0.5f},
                                         ui::ui_pill_radius(3.0f), accent);
            }
            draw_list.add_text({row.x + 12.0f, row.y, row.w - 20.0f, row.h},
                               values[i], 14.0f,
                               ui::ui_theme_color(ui::UIThemeColor::Text));
        }
    }
};

class UIPopup : public UIPanel {
public:
    bool visible = true;
    bool modal = false;

    UIPopup() {
        // 浮层是层次最高的一类表面：大圆角 + 明显投影。投影由 ui_paint 在填充
        // 之前画，控件本身不需要关心顺序。
        radius_tier = ui::UIRadiusTier::Large;
        rest_elevation = 14.0f;
    }

    const char* type() const override { return "UIPopup"; }
    bool is_visible_in_tree() const override {
        return visible && UIPanel::is_visible_in_tree();
    }
    // 浮层一律最后画：菜单、对话框、提示条的矩形都伸到父容器之外，按树序
    // 画会被后面的兄弟节点盖住。
    bool draws_on_top() const override { return visible; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPanel::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIPopup)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIPanel::serialize(out);
        out["visible"] = visible;
        out["modal"] = modal;
    }
    void deserialize(const nlohmann::json& in) override {
        UIPanel::deserialize(in);
        visible = in.value("visible", true);
        modal = in.value("modal", false);
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (visible) UIPanel::add_draw_commands(rect, draw_list);
    }
};

class UIModal : public UIPopup {
public:
    UIModal() {
        modal = true;
        z_index = 10000;
        size = math::Vector2f(480.0f, 300.0f);
        min_size = size;
    }
    const char* type() const override { return "UIModal"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPopup::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIModal)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
};

class UITooltip : public UIPopup {
public:
    std::string text = "Tooltip";
    float font_size = 13.0f;

    UITooltip() {
        z_index = 20000;
        receives_input = false;
        size = math::Vector2f(160.0f, 28.0f);
        min_size = size;
        // 提示条是"贴着指针"的轻量浮层，圆角收小一档，投影也淡一些。
        radius_tier = ui::UIRadiusTier::Small;
        rest_elevation = 8.0f;
    }
    const char* type() const override { return "UITooltip"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPopup::collect_type_views(out);
        out.push_back({std::type_index(typeid(UITooltip)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIPopup::serialize(out);
        out["text"] = text;
        out["font_size"] = font_size;
    }
    void deserialize(const nlohmann::json& in) override {
        UIPopup::deserialize(in);
        text = in.value("text", text);
        font_size = in.value("font_size", font_size);
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (!visible) return;
        UIPopup::add_draw_commands(rect, draw_list);
        draw_list.add_text({rect.x + 6.0f, rect.y + 4.0f, rect.w - 12.0f, rect.h - 8.0f},
                           text, font_size, 0xFFFFFFFFu);
    }
};

class UIDragSource : public UIPanel {
public:
    std::string payload = "item";
    std::string label = "Drag";

    const char* type() const override { return "UIDragSource"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPanel::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIDragSource)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    std::string drag_payload() const override { return payload; }
    void serialize(nlohmann::json& out) const override {
        UIPanel::serialize(out);
        out["payload"] = payload;
        out["label"] = label;
    }
    void deserialize(const nlohmann::json& in) override {
        UIPanel::deserialize(in);
        payload = in.value("payload", "item");
        label = in.value("label", "Drag");
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        UIPanel::add_draw_commands(rect, draw_list);
        draw_list.add_text({rect.x + 6.0f, rect.y, rect.w - 12.0f, rect.h},
                           label, 14.0f, 0xFFFFFFFFu);
    }
};

class UIDropZone : public UIPanel {
public:
    std::string accepted_prefix;
    std::string last_payload;
    bool highlighted = false;
    math::Vector4f active_color = math::Vector4f(0.18f, 0.52f, 0.82f, 0.55f);

    const char* type() const override { return "UIDropZone"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPanel::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIDropZone)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::DragOver) {
            highlighted = accepted_prefix.empty() ||
                          event.payload.rfind(accepted_prefix, 0) == 0;
            return highlighted;
        }
        if (event.type == ui::UIEventType::Drop) {
            highlighted = false;
            if (accepted_prefix.empty() ||
                event.payload.rfind(accepted_prefix, 0) == 0) {
                last_payload = event.payload;
                return true;
            }
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        UIPanel::add_draw_commands(rect, draw_list);
        if (highlighted) {
            draw_list.add_rect(rect, ui::ui_color(active_color.x, active_color.y,
                                                  active_color.z, active_color.w));
        }
        if (!last_payload.empty()) {
            draw_list.add_text({rect.x + 6.0f, rect.y, rect.w - 12.0f, rect.h},
                               last_payload, 13.0f, 0xFFFFFFFFu);
        }
    }
};

enum class UIDockSide : uint8_t {
    Center = 0,
    Top,
    Bottom,
    Left,
    Right,
};

class UIDockPanel : public UIPanel {
public:
    UIDockSide side = UIDockSide::Center;
    std::string title = "Panel";
    float title_height = 24.0f;

    UIDockPanel() {
        clip_contents = true;
        padding = 8.0f;
        // 内容视图与所在卡片同层：比页面底再深一档会让列表看起来是凹陷的。
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UIDockPanel"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPanel::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIDockPanel)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIPanel::serialize(out);
        out["side"] = static_cast<int>(side);
        out["title"] = title;
        out["title_height"] = title_height;
    }
    void deserialize(const nlohmann::json& in) override {
        UIPanel::deserialize(in);
        side = static_cast<UIDockSide>(
            in.value("side", static_cast<int>(UIDockSide::Center)));
        title = in.value("title", "Panel");
        title_height = in.value("title_height", 24.0f);
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        UIPanel::add_draw_commands(rect, draw_list);
        const ui::UIRect title_rect{rect.x, rect.y, rect.w, std::min(title_height, rect.h)};
        draw_list.add_rect(title_rect, ui::ui_theme_color(ui::UIThemeColor::PanelRaised));
        draw_list.add_text({title_rect.x + 8.0f, title_rect.y,
                            title_rect.w - 16.0f, title_rect.h},
                           title, 14.0f, themed_color(
                               foreground_role, 0xFFFFFFFFu));
    }
    std::string drag_payload() const override { return "dock:" + title; }
};

class UIDockHost : public NodeUI {
public:
    float top_height = 36.0f;
    float bottom_height = 140.0f;
    float left_width = 260.0f;
    float right_width = 280.0f;
    float gap = 2.0f;

    const char* type() const override { return "UIDockHost"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIDockHost)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["top_height"] = top_height;
        out["bottom_height"] = bottom_height;
        out["left_width"] = left_width;
        out["right_width"] = right_width;
        out["gap"] = gap;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        top_height = in.value("top_height", 36.0f);
        bottom_height = in.value("bottom_height", 140.0f);
        left_width = in.value("left_width", 260.0f);
        right_width = in.value("right_width", 280.0f);
        gap = in.value("gap", 2.0f);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type != ui::UIEventType::Drop ||
            event.payload.rfind("dock:", 0) != 0) return false;
        const std::string title = event.payload.substr(5);
        components::UIDockPanel* target_panel = nullptr;
        for (Component* child : component_children()) {
            auto* panel = dynamic_cast<components::UIDockPanel*>(child);
            if (panel && panel->title == title) {
                target_panel = panel;
                break;
            }
        }
        if (!target_panel || event.layout_rect.empty()) return false;
        const float nx = (event.position.x - event.layout_rect.x) / event.layout_rect.w;
        const float ny = (event.position.y - event.layout_rect.y) / event.layout_rect.h;
        if (nx < 0.2f) target_panel->side = UIDockSide::Left;
        else if (nx > 0.8f) target_panel->side = UIDockSide::Right;
        else if (ny < 0.2f) target_panel->side = UIDockSide::Top;
        else if (ny > 0.8f) target_panel->side = UIDockSide::Bottom;
        else target_panel->side = UIDockSide::Center;
        return true;
    }
};

class UIFloatingPanel : public UIPopup {
public:
    std::string title = "Floating";
    float title_height = 26.0f;
    bool draggable = true;

    UIFloatingPanel() {
        z_index = 15000;
        size = math::Vector2f(360.0f, 240.0f);
        min_size = math::Vector2f(120.0f, 80.0f);
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UIFloatingPanel"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPopup::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIFloatingPanel)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIPopup::serialize(out);
        out["title"] = title;
        out["title_height"] = title_height;
        out["draggable"] = draggable;
    }
    void deserialize(const nlohmann::json& in) override {
        UIPopup::deserialize(in);
        title = in.value("title", "Floating");
        title_height = in.value("title_height", 26.0f);
        draggable = in.value("draggable", true);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (!draggable) return false;
        const float title_bottom = event.layout_rect.y + title_height;
        if (event.type == ui::UIEventType::PointerDown &&
            event.position.y <= title_bottom) {
            dragging = true;
            drag_offset.x = event.position.x - event.layout_rect.x;
            drag_offset.y = event.position.y - event.layout_rect.y;
            return true;
        }
        if (event.type == ui::UIEventType::PointerMove && dragging) {
            set_local_position(math::Vector3f(
                event.position.x - event.parent_rect.x - drag_offset.x,
                event.position.y - event.parent_rect.y - drag_offset.y, 0.0f));
            return true;
        }
        if (event.type == ui::UIEventType::PointerUp) {
            dragging = false;
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        UIPopup::add_draw_commands(rect, draw_list);
        const ui::UIRect title_rect{rect.x, rect.y, rect.w, std::min(title_height, rect.h)};
        draw_list.add_rect(title_rect, ui::ui_theme_color(ui::UIThemeColor::AccentPressed));
        draw_list.add_text({title_rect.x + 8.0f, title_rect.y,
                            title_rect.w - 16.0f, title_rect.h},
                           title, 14.0f, themed_color(
                               foreground_role, 0xFFFFFFFFu));
    }

private:
    bool dragging = false;
    math::Vector2f drag_offset;
};

} // namespace gryce_engine::components
