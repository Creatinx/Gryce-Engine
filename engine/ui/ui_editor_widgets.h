#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "ui/ui_extended_widgets.h"
#include "ui/code_highlight.h"

namespace gryce_engine::components {

// 模型层没有 renderer，量不到真实字宽；菜单/标签/图标按钮这类"宽度自适"的
// 地方按等宽字体 0.58 倍字号估，再补左右留白。等宽字体下误差在 1px 内。
inline float ui_estimate_text_width(const std::string& text, float font_size) {
    return static_cast<float>(text.size()) * font_size * 0.58f;
}

inline std::vector<std::string> ui_split_pipe(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string value;
    while (std::getline(stream, value, '|')) {
        out.push_back(value);
    }
    return out;
}

inline std::string ui_format_number(float value, int precision = 3) {
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.*f",
                  std::clamp(precision, 0, 6), static_cast<double>(value));
    return buffer;
}

inline float ui_snap_number(float value, float step) {
    if (!(step > 0.0f)) return value;
    return std::round(value / step) * step;
}

inline math::Vector3f ui_hsv_to_rgb(float h, float s, float v) {
    h = std::fmod(std::max(0.0f, h), 1.0f);
    s = std::clamp(s, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);
    const float c = v * s;
    const float hp = h * 6.0f;
    const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
    math::Vector3f rgb;
    switch (std::clamp(static_cast<int>(hp), 0, 5)) {
        case 0: rgb = math::Vector3f(c, x, 0.0f); break;
        case 1: rgb = math::Vector3f(x, c, 0.0f); break;
        case 2: rgb = math::Vector3f(0.0f, c, x); break;
        case 3: rgb = math::Vector3f(0.0f, x, c); break;
        case 4: rgb = math::Vector3f(x, 0.0f, c); break;
        default: rgb = math::Vector3f(c, 0.0f, x); break;
    }
    const float m = v - c;
    return rgb + math::Vector3f(m, m, m);
}

inline std::string ui_key_name(int key) {
    if (key >= '0' && key <= '9') return std::string(1, static_cast<char>(key));
    if (key >= 'A' && key <= 'Z') return std::string(1, static_cast<char>(key));
    switch (static_cast<ui::UIKey>(key)) {
        case ui::UIKey::Escape: return "Escape";
        case ui::UIKey::Enter: return "Enter";
        case ui::UIKey::Tab: return "Tab";
        case ui::UIKey::Backspace: return "Backspace";
        case ui::UIKey::Delete: return "Delete";
        case ui::UIKey::Right: return "Right";
        case ui::UIKey::Left: return "Left";
        case ui::UIKey::Down: return "Down";
        case ui::UIKey::Up: return "Up";
        case ui::UIKey::Home: return "Home";
        case ui::UIKey::End: return "End";
    }
    return "Key" + std::to_string(key);
}

// ---------------------------------------------------------------------------
// Text and numeric input
// ---------------------------------------------------------------------------

class UIMultilineTextInput : public UITextInput {
public:
    float line_height = 20.0f;

    UIMultilineTextInput() {
        size = math::Vector2f(360.0f, 160.0f);
        min_size = math::Vector2f(120.0f, 60.0f);
        clip_contents = true;
        max_length = 16384;
        background_role = ui::UIThemeColor::Input;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Accent;
    }
    const char* type() const override { return "UIMultilineTextInput"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UITextInput::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIMultilineTextInput)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UITextInput::serialize(out);
        out["line_height"] = line_height;
    }
    void deserialize(const nlohmann::json& in) override {
        UITextInput::deserialize(in);
        line_height = in.value("line_height", 20.0f);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::KeyDown && focused &&
            event.key == static_cast<int>(ui::UIKey::Enter)) {
            preedit.clear();
            erase_selection();
            const std::string addition = "\n";
            if (text.size() + addition.size() <=
                static_cast<std::size_t>(std::max(0, max_length))) {
                text.insert(cursor, addition);
                cursor += addition.size();
                selection_start = cursor;
            }
            return true;
        }
        return UITextInput::on_ui_event(event);
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        const float radius = ui::ui_resolve_radius(corner_radius, radius_tier);
        draw_list.add_round_rect(rect, radius, themed_color(
            background_role, ui::ui_color(background.x, background.y,
                                          background.z, background.w)),
                                 0u, 1.0f,
                                 ui::ui_with_alpha(
                                     ui::ui_theme_color(ui::UIThemeColor::Border), 0.6f));
        std::size_t begin = 0;
        int line = 0;
        while (begin <= text.size() && line < 4096) {
            const std::size_t end = text.find('\n', begin);
            const std::string value = text.substr(
                begin, end == std::string::npos ? std::string::npos : end - begin);
            const float y = rect.y + 4.0f + line_height * static_cast<float>(line);
            if (y >= rect.bottom()) break;
            draw_list.add_text({rect.x + 6.0f, y, rect.w - 12.0f, line_height},
                               value, font_size,
                               themed_color(foreground_role,
                                   ui::ui_color(text_color.x, text_color.y,
                                                text_color.z, text_color.w)));
            if (!preedit.empty() && cursor >= begin &&
                (end == std::string::npos || cursor <= end)) {
                draw_list.add_text({rect.x + 6.0f, y, rect.w - 12.0f, line_height},
                                   preedit, font_size,
                                   themed_color(foreground_role,
                                       ui::ui_color(text_color.x, text_color.y,
                                                    text_color.z, text_color.w)));
            }
            if (focused && (end == std::string::npos || cursor <= end)) {
                const std::size_t line_cursor =
                    cursor >= begin ? cursor - begin : 0;
                const float x = rect.x + 6.0f +
                    static_cast<float>(line_cursor) * font_size * 0.58f;
                draw_list.add_rect({x, y + 2.0f, 1.5f,
                                    std::max(1.0f, line_height - 4.0f)},
                                   themed_color(accent_role,
                                       ui::ui_color(cursor_color.x, cursor_color.y,
                                                    cursor_color.z, cursor_color.w)));
            }
            if (end == std::string::npos) break;
            begin = end + 1;
            ++line;
        }
    }
};

class UISearchBox : public UITextInput {
public:
    std::string placeholder = "Search...";
    std::string query;
    bool query_changed = false;

    UISearchBox() {
        size = math::Vector2f(240.0f, 28.0f);
        min_size = size;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UISearchBox"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UITextInput::collect_type_views(out);
        out.push_back({std::type_index(typeid(UISearchBox)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UITextInput::serialize(out);
        out["placeholder"] = placeholder;
        out["query"] = query;
    }
    void deserialize(const nlohmann::json& in) override {
        UITextInput::deserialize(in);
        placeholder = in.value("placeholder", placeholder);
        query = in.value("query", text);
        text = query;
    }
    void set_query(std::string value) {
        query = std::move(value);
        text = query;
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if ((event.type == ui::UIEventType::PointerDown ||
             event.type == ui::UIEventType::Click) &&
            event.position.x >= event.layout_rect.right() - 24.0f) {
            set_query({});
            query_changed = true;
            return true;
        }
        const bool handled = UITextInput::on_ui_event(event);
        if (query != text) {
            query = text;
            query_changed = true;
        }
        return handled;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        UITextInput::add_draw_commands(rect, draw_list);
        if (text.empty() && !placeholder.empty()) {
            draw_list.add_text({rect.x + 7.0f, rect.y, rect.w - 32.0f, rect.h},
                               placeholder, font_size,
                               ui::ui_theme_color(ui::UIThemeColor::TextMuted));
        }
        if (!text.empty()) {
            draw_list.add_text({rect.right() - 20.0f, rect.y, 16.0f, rect.h},
                               "x", 14.0f,
                               ui::ui_theme_color(ui::UIThemeColor::TextMuted));
        }
    }
};

class UINumberInput : public UITextInput {
public:
    float value = 0.0f;
    float minimum = -std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::max();
    float step = 0.1f;
    int precision = 3;
    bool value_changed = false;

    UINumberInput() {
        text = "0.000";
        size = math::Vector2f(140.0f, 28.0f);
        min_size = size;
    }
    const char* type() const override { return "UINumberInput"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UITextInput::collect_type_views(out);
        out.push_back({std::type_index(typeid(UINumberInput)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UITextInput::serialize(out);
        out["value"] = value;
        out["minimum"] = minimum;
        out["maximum"] = maximum;
        out["step"] = step;
        out["precision"] = precision;
    }
    void deserialize(const nlohmann::json& in) override {
        UITextInput::deserialize(in);
        value = in.value("value", 0.0f);
        minimum = in.value("minimum", -std::numeric_limits<float>::max());
        maximum = in.value("maximum", std::numeric_limits<float>::max());
        step = in.value("step", 0.1f);
        precision = in.value("precision", 3);
        text = ui_format_number(value, precision);
    }
    void set_value(float next, bool notify = false) {
        value = std::clamp(ui_snap_number(next, step), minimum, maximum);
        text = ui_format_number(value, precision);
        value_changed = notify;
    }
    bool commit() {
        char* end = nullptr;
        const float parsed = std::strtof(text.c_str(), &end);
        if (!end || end == text.c_str() || !std::isfinite(parsed)) {
            text = ui_format_number(value, precision);
            return false;
        }
        const float previous = value;
        value = std::clamp(ui_snap_number(parsed, step), minimum, maximum);
        text = ui_format_number(value, precision);
        value_changed = value != previous;
        return value_changed;
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        const bool handled = UITextInput::on_ui_event(event);
        if (event.type == ui::UIEventType::FocusLost) {
            commit();
            return handled;
        }
        if (event.type != ui::UIEventType::KeyDown || !focused) return handled;
        if (event.key == static_cast<int>(ui::UIKey::Enter)) {
            commit();
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Up) ||
            event.key == static_cast<int>(ui::UIKey::Down)) {
            const float direction =
                event.key == static_cast<int>(ui::UIKey::Up) ? 1.0f : -1.0f;
            set_value(value + direction * step, true);
            return true;
        }
        return handled;
    }
};

class UIDragFloat : public NodeUI {
public:
    std::string label = "Value";
    float value = 0.0f;
    float minimum = -std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::max();
    float step = 0.01f;
    float drag_speed = 0.02f;
    int precision = 3;
    bool value_changed = false;
    math::Vector4f background = math::Vector4f(0.10f, 0.12f, 0.16f, 1.0f);
    math::Vector4f active_background = math::Vector4f(0.16f, 0.24f, 0.34f, 1.0f);

    UIDragFloat() {
        size = math::Vector2f(220.0f, 26.0f);
        min_size = size;
        focusable = true;
        background_role = ui::UIThemeColor::Input;
        background_hover_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UIDragFloat"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIDragFloat)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["label"] = label;
        out["value"] = value;
        out["minimum"] = minimum;
        out["maximum"] = maximum;
        out["step"] = step;
        out["drag_speed"] = drag_speed;
        out["precision"] = precision;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        label = in.value("label", label);
        value = in.value("value", value);
        minimum = in.value("minimum", minimum);
        maximum = in.value("maximum", maximum);
        step = in.value("step", step);
        drag_speed = in.value("drag_speed", drag_speed);
        precision = in.value("precision", precision);
    }
    void set_value(float next, bool notify = false) {
        const float clamped = std::clamp(ui_snap_number(next, step), minimum, maximum);
        value_changed = notify && clamped != value;
        value = clamped;
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::PointerDown) {
            dragging = true;
            drag_start_x = event.position.x;
            drag_start_value = value;
            return true;
        }
        if (event.type == ui::UIEventType::PointerMove && dragging) {
            set_value(drag_start_value +
                      (event.position.x - drag_start_x) * drag_speed, true);
            return true;
        }
        if (event.type == ui::UIEventType::PointerUp) {
            dragging = false;
            return true;
        }
        if (event.type != ui::UIEventType::KeyDown) return false;
        if (event.key == static_cast<int>(ui::UIKey::Left) ||
            event.key == static_cast<int>(ui::UIKey::Down)) {
            set_value(value - step, true);
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Right) ||
            event.key == static_cast<int>(ui::UIKey::Up)) {
            set_value(value + step, true);
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        const auto& bg = dragging ? active_background : background;
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier), themed_color(
            background_role, ui::ui_color(bg.x, bg.y, bg.z, bg.w)));
        const float split = rect.x + rect.w * 0.42f;
        draw_list.add_text({rect.x + 7.0f, rect.y, rect.w * 0.42f - 12.0f, rect.h},
                           label, 14.0f,
                           ui::ui_theme_color(ui::UIThemeColor::TextMuted));
        draw_list.add_text({split + 7.0f, rect.y, rect.right() - split - 12.0f, rect.h},
                           ui_format_number(value, precision),
                           14.0f,
                           ui::ui_theme_color(ui::UIThemeColor::Text));
    }
    bool dragging = false;

private:
    float drag_start_x = 0.0f;
    float drag_start_value = 0.0f;
};

class UIVectorEditor : public NodeUI {
public:
    int components = 3;
    std::array<float, 4> values = {0.0f, 0.0f, 0.0f, 0.0f};
    std::string labels = "X\nY\nZ\nW";
    float minimum = -std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::max();
    float step = 0.01f;
    float drag_speed = 0.02f;
    int precision = 3;
    int active_component = 0;
    bool value_changed = false;
    math::Vector4f background = math::Vector4f(0.08f, 0.10f, 0.14f, 1.0f);

    UIVectorEditor() {
        size = math::Vector2f(260.0f, 26.0f);
        min_size = size;
        focusable = true;
        background_role = ui::UIThemeColor::Input;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Selection;
    }
    const char* type() const override { return "UIVectorEditor"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIVectorEditor)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["components"] = components;
        out["values"] = {values[0], values[1], values[2], values[3]};
        out["labels"] = labels;
        out["minimum"] = minimum;
        out["maximum"] = maximum;
        out["step"] = step;
        out["drag_speed"] = drag_speed;
        out["precision"] = precision;
        out["active_component"] = active_component;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        components = std::clamp(in.value("components", 3), 2, 4);
        const auto v = in.value("values", std::vector<float>{
            values[0], values[1], values[2], values[3]});
        for (std::size_t i = 0; i < v.size() && i < values.size(); ++i) {
            values[i] = v[i];
        }
        labels = in.value("labels", labels);
        minimum = in.value("minimum", minimum);
        maximum = in.value("maximum", maximum);
        step = in.value("step", step);
        drag_speed = in.value("drag_speed", drag_speed);
        precision = in.value("precision", precision);
        active_component = std::clamp(in.value("active_component", 0), 0, components - 1);
    }
    void set_value(int index, float next, bool notify = true) {
        if (index < 0 || index >= components) return;
        const float clamped = std::clamp(ui_snap_number(next, step), minimum, maximum);
        value_changed = value_changed || (notify && clamped != values[index]);
        values[index] = clamped;
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        const float lane_width = event.layout_rect.w / static_cast<float>(components);
        if (event.type == ui::UIEventType::PointerDown) {
            active_component = std::clamp(
                static_cast<int>((event.position.x - event.layout_rect.x) /
                                 std::max(1.0f, lane_width)),
                0, components - 1);
            dragging = true;
            drag_start_x = event.position.x;
            drag_start_value = values[active_component];
            return true;
        }
        if (event.type == ui::UIEventType::PointerMove && dragging) {
            set_value(active_component,
                      drag_start_value +
                          (event.position.x - drag_start_x) * drag_speed);
            return true;
        }
        if (event.type == ui::UIEventType::PointerUp) {
            dragging = false;
            return true;
        }
        if (event.type != ui::UIEventType::KeyDown) return false;
        if (event.key == static_cast<int>(ui::UIKey::Left) ||
            event.key == static_cast<int>(ui::UIKey::Down)) {
            set_value(active_component, values[active_component] - step);
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Right) ||
            event.key == static_cast<int>(ui::UIKey::Up)) {
            set_value(active_component, values[active_component] + step);
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier), themed_color(
            background_role, ui::ui_color(background.x, background.y,
                                          background.z, background.w)));
        const auto names = ui_split_lines(labels);
        const float lane_width = rect.w / static_cast<float>(components);
        for (int i = 0; i < components; ++i) {
            const ui::UIRect lane{rect.x + lane_width * static_cast<float>(i),
                                  rect.y, lane_width, rect.h};
            if (i == active_component) {
                draw_list.add_rect(lane,
                    ui::ui_theme_color(ui::UIThemeColor::Selection));
            }
            const std::string name = i < static_cast<int>(names.size()) ? names[i] :
                                     std::string(1, static_cast<char>('X' + i));
            draw_list.add_text({lane.x + 5.0f, lane.y, 16.0f, lane.h},
                               name, 13.0f,
                               ui::ui_theme_color(ui::UIThemeColor::TextMuted));
            draw_list.add_text({lane.x + 22.0f, lane.y,
                                std::max(0.0f, lane.w - 28.0f), lane.h},
                               ui_format_number(values[i], precision),
                               13.0f,
                               ui::ui_theme_color(ui::UIThemeColor::Text));
        }
    }

private:
    bool dragging = false;
    float drag_start_x = 0.0f;
    float drag_start_value = 0.0f;
};

class UIColorPicker : public NodeUI {
public:
    math::Vector4f color = math::Vector4f(1.0f, 0.5f, 0.1f, 1.0f);
    float hue = 0.08f;
    float saturation = 1.0f;
    float brightness = 1.0f;
    bool color_changed = false;

    UIColorPicker() {
        size = math::Vector2f(220.0f, 150.0f);
        min_size = size;
        focusable = true;
    }
    const char* type() const override { return "UIColorPicker"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIColorPicker)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["color"] = {color.x, color.y, color.z, color.w};
        out["hue"] = hue;
        out["saturation"] = saturation;
        out["brightness"] = brightness;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        const auto c = in.value("color", std::vector<float>{
            color.x, color.y, color.z, color.w});
        if (c.size() >= 4) color = math::Vector4f(c[0], c[1], c[2], c[3]);
        hue = in.value("hue", hue);
        saturation = in.value("saturation", saturation);
        brightness = in.value("brightness", brightness);
    }
    void set_rgb(const math::Vector4f& next, bool notify = true) {
        color_changed = notify && (next.x != color.x || next.y != color.y ||
                                   next.z != color.z || next.w != color.w);
        color = next;
        saturation = 0.0f;
        brightness = 0.0f;
    }
    void update_rgb(bool notify) {
        const math::Vector3f rgb = ui_hsv_to_rgb(hue, saturation, brightness);
        const math::Vector4f next(rgb.x, rgb.y, rgb.z, color.w);
        color_changed = notify && (next.x != color.x || next.y != color.y ||
                                   next.z != color.z);
        color = next;
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::KeyDown) {
            const float delta = 0.02f;
            if (event.key == static_cast<int>(ui::UIKey::Left)) hue -= delta;
            else if (event.key == static_cast<int>(ui::UIKey::Right)) hue += delta;
            else if (event.key == static_cast<int>(ui::UIKey::Down)) brightness -= delta;
            else if (event.key == static_cast<int>(ui::UIKey::Up)) brightness += delta;
            else return false;
            hue = std::fmod(std::max(0.0f, hue), 1.0f);
            brightness = std::clamp(brightness, 0.0f, 1.0f);
            update_rgb(true);
            return true;
        }
        if (event.type != ui::UIEventType::PointerDown &&
            event.type != ui::UIEventType::PointerMove) return false;
        const float hue_height = 16.0f;
        const float field_height = std::max(1.0f, event.layout_rect.h - hue_height - 4.0f);
        if (event.position.y >= event.layout_rect.y + field_height) {
            hue = std::clamp((event.position.x - event.layout_rect.x) /
                             std::max(1.0f, event.layout_rect.w), 0.0f, 1.0f);
        } else {
            saturation = std::clamp((event.position.x - event.layout_rect.x) /
                                    std::max(1.0f, event.layout_rect.w), 0.0f, 1.0f);
            brightness = std::clamp(1.0f -
                (event.position.y - event.layout_rect.y) / field_height, 0.0f, 1.0f);
        }
        update_rgb(true);
        return true;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        const float hue_height = 16.0f;
        const float field_height = std::max(1.0f, rect.h - hue_height - 4.0f);
        constexpr int columns = 12;
        constexpr int rows = 7;
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < columns; ++x) {
                const float s = (static_cast<float>(x) + 0.5f) / columns;
                const float v = 1.0f - (static_cast<float>(y) + 0.5f) / rows;
                const math::Vector3f rgb = ui_hsv_to_rgb(hue, s, v);
                draw_list.add_rect({
                    rect.x + rect.w * static_cast<float>(x) / columns,
                    rect.y + field_height * static_cast<float>(y) / rows,
                    rect.w / columns + 1.0f, field_height / rows + 1.0f,
                }, ui::ui_color(rgb.x, rgb.y, rgb.z, 1.0f));
            }
        }
        for (int i = 0; i < 24; ++i) {
            const math::Vector3f rgb = ui_hsv_to_rgb(static_cast<float>(i) / 24.0f,
                                                     1.0f, 1.0f);
            draw_list.add_rect({
                rect.x + rect.w * static_cast<float>(i) / 24.0f,
                rect.y + field_height + 4.0f,
                rect.w / 24.0f + 1.0f, hue_height,
            }, ui::ui_color(rgb.x, rgb.y, rgb.z, 1.0f));
        }
        draw_list.add_rect({rect.x + rect.w * hue - 2.0f,
                            rect.y + field_height + 2.0f, 4.0f, hue_height + 4.0f},
                           0xFFFFFFFFu);
        draw_list.add_rect({rect.x + rect.w * saturation - 3.0f,
                            rect.y + field_height * (1.0f - brightness) - 3.0f,
                            6.0f, 6.0f}, 0xFFFFFFFFu);
    }
};

class UIRangeSlider : public NodeUI {
public:
    float minimum = 0.0f;
    float maximum = 1.0f;
    float low = 0.25f;
    float high = 0.75f;
    float step = 0.01f;
    bool value_changed = false;
    math::Vector4f track_color = math::Vector4f(0.12f, 0.14f, 0.18f, 1.0f);
    math::Vector4f fill_color = math::Vector4f(0.24f, 0.58f, 0.90f, 1.0f);

    UIRangeSlider() {
        size = math::Vector2f(240.0f, 26.0f);
        min_size = size;
        focusable = true;
        accent_role = ui::UIThemeColor::Accent;
    }
    const char* type() const override { return "UIRangeSlider"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIRangeSlider)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["minimum"] = minimum;
        out["maximum"] = maximum;
        out["low"] = low;
        out["high"] = high;
        out["step"] = step;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        minimum = in.value("minimum", minimum);
        maximum = in.value("maximum", maximum);
        low = in.value("low", low);
        high = in.value("high", high);
        step = in.value("step", step);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        const float range = std::max(0.0001f, maximum - minimum);
        if (event.type == ui::UIEventType::PointerDown) {
            const float value = minimum +
                std::clamp((event.position.x - event.layout_rect.x) /
                           std::max(1.0f, event.layout_rect.w), 0.0f, 1.0f) * range;
            active_high = std::fabs(value - high) < std::fabs(value - low);
            update_value(value);
            dragging = true;
            return true;
        }
        if (event.type == ui::UIEventType::PointerMove && dragging) {
            const float value = minimum +
                std::clamp((event.position.x - event.layout_rect.x) /
                           std::max(1.0f, event.layout_rect.w), 0.0f, 1.0f) * range;
            update_value(value);
            return true;
        }
        if (event.type == ui::UIEventType::PointerUp) {
            dragging = false;
            return true;
        }
        if (event.type == ui::UIEventType::KeyDown) {
            if (event.key == static_cast<int>(ui::UIKey::Left) ||
                event.key == static_cast<int>(ui::UIKey::Down)) {
                update_value((active_high ? high : low) - step);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Right) ||
                event.key == static_cast<int>(ui::UIKey::Up)) {
                update_value((active_high ? high : low) + step);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Tab)) {
                active_high = !active_high;
                return true;
            }
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        // Fluent 的双头滑块：4px 细轨道 + 选中区间高亮 + 两枚 20px 圆头。
        // 整块铺底再插两根白条是"进度条加两个标记"，与滑块的手感不符。
        const float thickness = std::min(4.0f, rect.h);
        const float cy = rect.y + rect.h * 0.5f;
        const ui::UIRect track{rect.x, cy - thickness * 0.5f, rect.w, thickness};
        const uint32_t accent = themed_color(accent_role,
            ui::ui_color(fill_color.x, fill_color.y, fill_color.z, fill_color.w));
        draw_list.add_round_rect(track, ui::ui_pill_radius(thickness),
                                 ui::ui_with_alpha(
                                     ui::ui_theme_color(UIThemeColor::Track), 0.60f));
        const float range = std::max(0.0001f, maximum - minimum);
        const float lo = std::clamp((low - minimum) / range, 0.0f, 1.0f);
        const float hi = std::clamp((high - minimum) / range, 0.0f, 1.0f);
        if ((hi - lo) * rect.w > 0.5f) {
            draw_list.add_round_rect({rect.x + rect.w * lo, track.y,
                                      rect.w * (hi - lo), thickness},
                                     ui::ui_pill_radius(thickness), accent);
        }
        const float handle = std::min(20.0f, std::max(10.0f, rect.h - 2.0f)) *
                             (1.0f + 0.2f * hover_amount());
        const float handle_radius = handle * 0.5f;
        for (float t : {lo, hi}) {
            const ui::UIRect knob{rect.x + rect.w * t - handle_radius,
                                  cy - handle_radius, handle, handle};
            draw_list.add_round_rect(knob, handle_radius, accent, 0u, 1.0f,
                                     ui::ui_with_alpha(
                                         ui::ui_theme_color(UIThemeColor::PanelRaised),
                                         0.5f));
        }
        ui::ui_paint_focus_ring(
            draw_list, rect, ui::ui_resolve_radius(corner_radius, radius_tier),
            focus_amount());
    }

private:
    void update_value(float value) {
        const float next = std::clamp(ui_snap_number(value, step), minimum, maximum);
        if (active_high) {
            high = std::max(low, next);
        } else {
            low = std::min(high, next);
        }
        value_changed = true;
    }
    bool dragging = false;
    bool active_high = true;
};

class UIKeyBindingEditor : public NodeUI {
public:
    std::string binding = "None";
    bool capturing = false;
    bool binding_changed = false;
    math::Vector4f background = math::Vector4f(0.10f, 0.12f, 0.16f, 1.0f);

    UIKeyBindingEditor() {
        size = math::Vector2f(140.0f, 28.0f);
        min_size = size;
        focusable = true;
        background_role = ui::UIThemeColor::Input;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Accent;
    }
    const char* type() const override { return "UIKeyBindingEditor"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIKeyBindingEditor)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["binding"] = binding;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        binding = in.value("binding", binding);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::FocusLost) {
            capturing = false;
            return false;
        }
        if (event.type == ui::UIEventType::PointerDown ||
            event.type == ui::UIEventType::Click) {
            capturing = true;
            return true;
        }
        if (event.type != ui::UIEventType::KeyDown || !capturing) return false;
        if (event.key == static_cast<int>(ui::UIKey::Escape)) {
            binding = "None";
        } else {
            std::string next;
            if (event.modifiers & 1) next += "Ctrl+";
            if (event.modifiers & 2) next += "Shift+";
            if (event.modifiers & 4) next += "Alt+";
            next += ui_key_name(event.key);
            binding = std::move(next);
        }
        capturing = false;
        binding_changed = true;
        return true;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier), themed_color(
            background_role, ui::ui_color(background.x, background.y,
                                          background.z, background.w)));
        draw_list.add_text({rect.x + 7.0f, rect.y, rect.w - 14.0f, rect.h},
                           capturing ? "Press a key..." : binding,
                           14.0f, capturing
                               ? ui::ui_theme_color(ui::UIThemeColor::Accent)
                               : themed_color(foreground_role, 0xFFFFFFFFu));
    }
};

// ---------------------------------------------------------------------------
// Selection, menus and toolbars
// ---------------------------------------------------------------------------

class UIRadioButton : public NodeUI {
public:
    std::string text = "Radio";
    bool selected = false;
    int group = 0;
    float font_size = 14.0f;
    math::Vector4f text_color = math::Vector4f(0.92f, 0.94f, 0.97f, 1.0f);

    UIRadioButton() {
        size = math::Vector2f(180.0f, 32.0f);
        min_size = size;
        focusable = true;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Accent;
    }
    const char* type() const override { return "UIRadioButton"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIRadioButton)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["text"] = text;
        out["selected"] = selected;
        out["group"] = group;
        out["font_size"] = font_size;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        text = in.value("text", text);
        selected = in.value("selected", selected);
        group = in.value("group", group);
        font_size = in.value("font_size", font_size);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::Click ||
            (event.type == ui::UIEventType::KeyDown &&
             (event.key == static_cast<int>(ui::UIKey::Enter) || event.key == 32))) {
            selected = true;
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        // Fluent 的单选框是 20px 圆环：未选中为控件填充 + 亮描边，选中时描边
        // 换成强调色并在正中放一枚 10px 圆点。外圈永远比内点暗一档，才不会糊成
        // 一个实心圆。
        const float diameter = std::min(20.0f, rect.h);
        const ui::UIRect ring{rect.x, rect.y + (rect.h - diameter) * 0.5f,
                             diameter, diameter};
        const float ring_radius = diameter * 0.5f;
        const uint32_t accent = ui::ui_theme_color(accent_role);
        draw_list.add_round_rect(ring, ring_radius,
                                 selected
                                     ? ui::ui_theme_mix(
                                           ui::ui_theme_color(ui::UIThemeColor::ControlFill),
                                           accent, 0.12f + 0.1f * hover_amount())
                                     : ui::ui_control_fill(hover_amount(), 0.0f),
                                 0u, 1.0f,
                                 selected
                                     ? accent
                                     : ui::ui_control_stroke(hover_amount(), focus_amount()));
        if (selected) {
            const float dot = diameter * 0.5f;
            draw_list.add_round_rect({ring.x + (diameter - dot) * 0.5f,
                                      ring.y + (diameter - dot) * 0.5f, dot, dot},
                                     dot * 0.5f, accent);
        }
        draw_list.add_text({rect.x + diameter + 8.0f, rect.y,
                           rect.w - diameter - 8.0f, rect.h},
                          text, font_size,
                          themed_color(foreground_role,
                              ui::ui_color(text_color.x, text_color.y,
                                           text_color.z, text_color.w)));
        ui::ui_paint_focus_ring(draw_list, ring, ring_radius, focus_amount());
    }
};

class UIToggleButton : public UIButton {
public:
    bool checked = false;
    math::Vector4f checked_color = math::Vector4f(0.18f, 0.52f, 0.38f, 1.0f);

    const char* type() const override { return "UIToggleButton"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIButton::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIToggleButton)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIButton::serialize(out);
        out["checked"] = checked;
    }
    void deserialize(const nlohmann::json& in) override {
        UIButton::deserialize(in);
        checked = in.value("checked", false);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        const bool handled = UIButton::on_ui_event(event);
        if (event.type == ui::UIEventType::Click ||
            (event.type == ui::UIEventType::KeyDown &&
             (event.key == static_cast<int>(ui::UIKey::Enter) || event.key == 32))) {
            checked = !checked;
        }
        return handled;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (!checked) {
            UIButton::add_draw_commands(rect, draw_list);
            return;
        }
        // 选中态是强调色实底 + 白字（WinUI 的 ToggleButton checked 就是这个
        // 形态）。淡底 + 强调色文字在小尺寸下对比不足，看着像禁用。
        const float radius = ui::ui_resolve_radius(corner_radius, radius_tier);
        const uint32_t fill = ui::ui_theme_mix(
            ui::ui_theme_color(ui::UIThemeColor::Accent),
            ui::ui_theme_color(ui::UIThemeColor::AccentHover), hover_amount());
        draw_list.add_round_rect(rect, radius, fill, 0u, 1.0f,
                                 ui::ui_with_alpha(0xFFFFFFFFu, 0.10f));
        draw_list.add_text({rect.x + 12.0f, rect.y, rect.w - 24.0f, rect.h},
                           text, font_size, 0xFFFFFFFFu, ui::UITextAlign::Center);
        ui::ui_paint_focus_ring(
            draw_list, rect, ui::ui_resolve_radius(corner_radius, radius_tier),
            focus_amount());
    }
};

class UIIconButton : public UIButton {
public:
    // 图标可以是字符（内置字体里的符号）或 24px 贴图（推荐：贴图在任意 DPI
    // 下都清晰，字符图标会受字体字重与字形缺失影响）。两者都有时贴图优先。
    std::string icon = "*";
    std::string icon_path;
    float icon_size = 18.0f;

    UIIconButton() {
        // 图标按钮默认只画图标：基类的 text 默认是 "Button"，不清掉会在
        // 纯图标按钮上多出一段文字，和图标挤在一起。
        text.clear();
        show_text = false;
        variant = UIButtonVariant::Subtle;
    }

    const char* type() const override { return "UIIconButton"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIButton::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIIconButton)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIButton::serialize(out);
        out["icon"] = icon;
        out["icon_path"] = icon_path;
        out["icon_size"] = icon_size;
    }
    void deserialize(const nlohmann::json& in) override {
        UIButton::deserialize(in);
        icon = in.value("icon", icon);
        icon_path = in.value("icon_path", "");
        icon_size = in.value("icon_size", icon_size);
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        UIButton::add_draw_commands(rect, draw_list);
        // "图标 + 文字"作为一个整体在按钮内居中：图标单独贴左边、文字再由基类
        // 居中，会让两者在对不齐的位置上互相压。
        const bool has_text = !text.empty();
        const float text_w = has_text ? ui_estimate_text_width(text, font_size) : 0.0f;
        const float gap = has_text ? 8.0f : 0.0f;
        const float total = icon_size + gap + text_w;
        const float x = rect.x + (rect.w - total) * 0.5f;
        const ui::TextureRef tex = ui::ui_texture_cached(icon_path);
        const uint32_t tint = themed_color(foreground_role, 0xFFFFFFFFu);
        if (tex.valid()) {
            draw_list.add_image({x, rect.y + (rect.h - icon_size) * 0.5f,
                                 icon_size, icon_size},
                                tex.index, tex.generation,
                                0.0f, 0.0f, 1.0f, 1.0f, tint);
        } else if (!icon.empty()) {
            draw_list.add_text({x, rect.y, icon_size, rect.h}, icon, icon_size,
                               tint, ui::UITextAlign::Center);
        }
        if (has_text) {
            draw_list.add_text({x + icon_size + gap, rect.y, text_w, rect.h},
                               text, font_size, tint, ui::UITextAlign::Left);
        }
    }
};
class UIBreadcrumb : public NodeUI {
public:
    std::string items = "Project\nAssets\nScripts";
    int selected = -1;
    int activated = -1;
    float font_size = 14.0f;

    UIBreadcrumb() {
        size = math::Vector2f(320.0f, 24.0f);
        min_size = size;
        focusable = true;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Selection;
    }
    const char* type() const override { return "UIBreadcrumb"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIBreadcrumb)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["items"] = items;
        out["selected"] = selected;
        out["font_size"] = font_size;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        items = in.value("items", items);
        selected = in.value("selected", selected);
        font_size = in.value("font_size", font_size);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type != ui::UIEventType::Click &&
            event.type != ui::UIEventType::KeyDown) return false;
        const auto values = ui_split_lines(items);
        if (values.empty()) return false;
        if (event.type == ui::UIEventType::KeyDown) {
            if (event.key == static_cast<int>(ui::UIKey::Left)) {
                selected = std::max(0, selected - 1);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Right)) {
                selected = std::min(static_cast<int>(values.size()) - 1, selected + 1);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Enter)) {
                activated = selected;
                return true;
            }
            return false;
        }
        const float item_width = event.layout_rect.w /
                                 static_cast<float>(values.size());
        selected = std::clamp(static_cast<int>(
            (event.position.x - event.layout_rect.x) / std::max(1.0f, item_width)),
            0, static_cast<int>(values.size()) - 1);
        activated = selected;
        return true;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        const auto values = ui_split_lines(items);
        if (values.empty()) return;
        const float item_width = rect.w / static_cast<float>(values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (static_cast<int>(i) == selected) {
                draw_list.add_rect({rect.x + item_width * static_cast<float>(i),
                                    rect.y, item_width, rect.h},
                                   ui::ui_theme_color(accent_role));
            }
            draw_list.add_text({rect.x + item_width * static_cast<float>(i) + 6.0f,
                                rect.y, item_width - 12.0f, rect.h},
                               values[i], font_size,
                               themed_color(foreground_role, 0xE8ECF2FFu));
            if (i + 1 < values.size()) {
                draw_list.add_text({rect.x + item_width * static_cast<float>(i + 1) - 8.0f,
                                    rect.y, 16.0f, rect.h},
                                   "/", font_size,
                                   ui::ui_theme_color(ui::UIThemeColor::TextMuted));
            }
        }
    }
};

class UIToolbar : public UIPanel {
public:
    UIToolbar() {
        // 工具栏是按钮的布局容器，本身不铺底色：铺一块会让工具区看起来像
        // 一个深色输入框。
        draws_background = false;
        layout_mode = UILayoutMode::Row;
        padding = 4.0f;
        gap = 6.0f;
        size = math::Vector2f(640.0f, 36.0f);
        min_size = math::Vector2f(80.0f, 28.0f);
        background_role = ui::UIThemeColor::PanelRaised;
    }
    const char* type() const override { return "UIToolbar"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPanel::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIToolbar)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
};

// ---------------------------------------------------------------------------
// 菜单项：Fluent 的 MenuFlyoutItem
// ---------------------------------------------------------------------------
struct UIMenuItem {
    std::string label;
    // 右侧快捷键提示（"Ctrl+S"）。空 = 不显示。
    std::string shortcut;
    // 命令标识，由宿主解释（-1 = 无）。
    int command = -1;
    // 分隔线：忽略其余字段，只占一条细线的高度。
    bool separator = false;
    // 勾选项：左侧显示对勾（视图菜单的"显示网格"这类）。
    bool checked = false;
    bool enabled = true;
    // 有子菜单：右侧显示展开箭头（子项暂由宿主另行挂载）。
    bool submenu = false;
    // 左侧 16px 图标贴图（走 UI 纹理服务）。空 = 不显示。
    std::string icon_path;
};

struct UIMenuDef {
    std::string title;
    std::vector<UIMenuItem> items;
};

// ---------------------------------------------------------------------------
// UIMenuFlyout — 菜单栏/右键菜单共用的浮层
//
// 必须是独立节点（而不是菜单栏自己画）：命中测试按节点矩形走，画在自己
// rect 之外的浮层收不到点击。UIPopup 的 draws_on_top 保证它总在所有普通
// 节点之后绘制，不会被后面的卡片盖住。
// ---------------------------------------------------------------------------
class UIMenuFlyout : public UIPopup {
public:
    std::vector<UIMenuItem> entries;
    int selected = -1;
    int command = -1;
    float row_height = 32.0f;
    float font_size = 14.0f;
    float min_width = 180.0f;
    float separator_height = 9.0f;

    UIMenuFlyout() {
        z_index = 12000;
        visible = false;
        receives_input = false;
        padding = 4.0f;
        radius_tier = ui::UIRadiusTier::Medium;
        rest_elevation = 16.0f;
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Selection;
    }

    const char* type() const override { return "UIMenuFlyout"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPopup::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIMenuFlyout)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIPopup::serialize(out);
        out["row_height"] = row_height;
        out["font_size"] = font_size;
        out["min_width"] = min_width;
        nlohmann::json arr = nlohmann::json::array();
        for (const UIMenuItem& item : entries) {
            arr.push_back({{"label", item.label}, {"shortcut", item.shortcut},
                           {"command", item.command}, {"separator", item.separator},
                           {"checked", item.checked}, {"enabled", item.enabled},
                           {"submenu", item.submenu}, {"icon", item.icon_path}});
        }
        out["entries"] = arr;
    }
    void deserialize(const nlohmann::json& in) override {
        UIPopup::deserialize(in);
        row_height = in.value("row_height", row_height);
        font_size = in.value("font_size", font_size);
        min_width = in.value("min_width", min_width);
        entries.clear();
        if (in.contains("entries") && in["entries"].is_array()) {
            for (const auto& e : in["entries"]) {
                UIMenuItem item;
                item.label = e.value("label", "");
                item.shortcut = e.value("shortcut", "");
                item.command = e.value("command", -1);
                item.separator = e.value("separator", false);
                item.checked = e.value("checked", false);
                item.enabled = e.value("enabled", true);
                item.submenu = e.value("submenu", false);
                item.icon_path = e.value("icon", "");
                entries.push_back(std::move(item));
            }
        }
        recompute_size();
        receives_input = visible;
    }

    // 浮层必须是不透明的：主题里的"抬升表面"带一点透明度（acrylic），叠在
    // 命令栏/内容上会透出下面的文字。先让 UIPanel 画投影和发丝边，再在边框内
    // 铺一层实底，最后画条目。
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (!visible) return;
        UIPopup::add_draw_commands(rect, draw_list);
        const uint32_t solid = ui::ui_with_alpha(
            ui::ui_theme_mix(ui::ui_theme_color(ui::UIThemeColor::Window),
                             ui::ui_theme_color(ui::UIThemeColor::PanelRaised), 0.85f),
            1.0f);
        const float radius = ui::ui_resolve_radius(corner_radius, radius_tier);
        const ui::UIRect body{rect.x + 1.0f, rect.y + 1.0f,
                              std::max(0.0f, rect.w - 2.0f),
                              std::max(0.0f, rect.h - 2.0f)};
        draw_list.add_round_rect(body, std::max(0.0f, radius - 1.0f), solid);
        draw_entries(rect, draw_list);
    }

    void set_entries(std::vector<UIMenuItem> next) {
        entries = std::move(next);
        selected = -1;
        command = -1;
        recompute_size();
    }

    // 尺寸由内容推出来：宽度取最宽一行（左槽 + 文字 + 快捷键 + 右留白），
    // 高度逐行累加（分隔线只占细线的高度）。
    void recompute_size() {
        float width = min_width;
        float height = padding * 2.0f;
        for (const UIMenuItem& item : entries) {
            if (item.separator) {
                height += separator_height;
                continue;
            }
            height += row_height;
            const float shortcut = item.shortcut.empty()
                                       ? 0.0f
                                       : ui_estimate_text_width(item.shortcut, 12.0f) + 28.0f;
            const float content = 40.0f + ui_estimate_text_width(item.label, font_size) +
                                  shortcut + 16.0f;
            width = std::max(width, content);
        }
        size = math::Vector2f(width, height);
        min_size = size;
    }

    void set_open(bool value) {
        visible = value;
        receives_input = value;
        if (value) {
            selected = -1;
            command = -1;
        }
    }

    void place_at(float local_x, float local_y) {
        set_local_position(math::Vector3f(local_x, local_y, 0.0f));
    }

    bool on_ui_event(const ui::UIEvent& event) override {
        if (!visible) return false;
        if (event.type == ui::UIEventType::PointerMove) {
            const int index = index_at_y(event.position.y - event.layout_rect.y);
            selected = index >= 0 && entries[static_cast<std::size_t>(index)].enabled
                           ? index : -1;
            return true;
        }
        if (event.type == ui::UIEventType::Click ||
            event.type == ui::UIEventType::PointerDown) {
            const int index = index_at_y(event.position.y - event.layout_rect.y);
            if (index < 0) return false;
            const UIMenuItem& item = entries[static_cast<std::size_t>(index)];
            if (!item.enabled || item.separator) return false;
            command = item.command;
            // 勾选项点一下就地取反，调用方读 checked 即可知道新状态。
            if (item.checked) {
                entries[static_cast<std::size_t>(index)].checked = false;
            }
            set_open(false);
            return true;
        }
        if (event.type == ui::UIEventType::KeyDown) {
            if (event.key == static_cast<int>(ui::UIKey::Down)) {
                move_selection(1);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Up)) {
                move_selection(-1);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Enter) && selected >= 0) {
                command = entries[static_cast<std::size_t>(selected)].command;
                if (entries[static_cast<std::size_t>(selected)].checked) {
                    entries[static_cast<std::size_t>(selected)].checked = false;
                }
                set_open(false);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Escape)) {
                set_open(false);
                return true;
            }
        }
        return false;
    }

    void draw_entries(const ui::UIRect& rect, ui::UIDrawList& draw_list) const {
        const float radius = ui::ui_theme_radius(ui::UIRadiusTier::Small);
        const uint32_t text_color = themed_color(foreground_role, 0xFFFFFFFFu);
        const uint32_t muted = ui::ui_theme_color(ui::UIThemeColor::TextMuted);
        const uint32_t accent = themed_color(accent_role,
                                             ui::ui_theme_color(ui::UIThemeColor::Accent));
        float y = rect.y + padding;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const UIMenuItem& item = entries[i];
            if (item.separator) {
                draw_list.add_rect({rect.x + padding + 8.0f,
                                    y + separator_height * 0.5f,
                                    std::max(0.0f, rect.w - (padding + 8.0f) * 2.0f), 1.0f},
                                   ui::ui_with_alpha(
                                       ui::ui_theme_color(ui::UIThemeColor::ControlStroke), 0.8f));
                y += separator_height;
                continue;
            }
            const ui::UIRect row{rect.x + padding, y,
                                 std::max(0.0f, rect.w - padding * 2.0f), row_height};
            if (static_cast<int>(i) == selected) {
                draw_list.add_round_rect(row, radius,
                                         ui::ui_theme_color(ui::UIThemeColor::ControlFillHover));
            }
            // 左槽 16px：勾选项画对勾，带图标的画贴图，其余留空。
            const float slot_cx = row.x + 18.0f;
            const float slot_cy = row.y + row.h * 0.5f;
            if (item.checked) {
                draw_list.add_line(slot_cx - 4.0f, slot_cy,
                                   slot_cx - 1.0f, slot_cy + 3.5f, 1.8f, accent);
                draw_list.add_line(slot_cx - 1.0f, slot_cy + 3.5f,
                                   slot_cx + 4.0f, slot_cy - 4.0f, 1.8f, accent);
            } else if (!item.icon_path.empty()) {
                const ui::TextureRef icon = ui::ui_texture_cached(item.icon_path);
                if (icon.valid()) {
                    draw_list.add_image({slot_cx - 8.0f, slot_cy - 8.0f, 16.0f, 16.0f},
                                        icon.index, icon.generation);
                }
            }
            const uint32_t label_color = item.enabled
                ? text_color
                : ui::ui_with_alpha(text_color, 0.4f);
            draw_list.add_text({row.x + 36.0f, row.y,
                                std::max(0.0f, row.w - 48.0f), row.h},
                               item.label, font_size, label_color,
                               ui::UITextAlign::Left);
            if (!item.shortcut.empty()) {
                draw_list.add_text({row.x, row.y, row.w - 12.0f, row.h},
                                   item.shortcut, 12.0f,
                                   ui::ui_with_alpha(muted, item.enabled ? 1.0f : 0.4f),
                                   ui::UITextAlign::Right);
            } else if (item.submenu) {
                // 子菜单箭头：两条短斜线拼出的 ">"。
                const float ax = row.right() - 18.0f;
                draw_list.add_line(ax, slot_cy - 4.0f, ax + 3.5f, slot_cy, 1.6f, muted);
                draw_list.add_line(ax + 3.5f, slot_cy, ax, slot_cy + 4.0f, 1.6f, muted);
            }
            y += row_height;
        }
    }

private:
    int index_at_y(float local_y) const {
        float y = padding;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const float h = entries[i].separator ? separator_height : row_height;
            if (local_y >= y && local_y < y + h) {
                return entries[i].separator ? -1 : static_cast<int>(i);
            }
            y += h;
        }
        return -1;
    }
    void move_selection(int delta) {
        if (entries.empty()) return;
        int index = selected;
        for (int step = 0; step < static_cast<int>(entries.size()); ++step) {
            index += delta;
            if (index < 0) index = static_cast<int>(entries.size()) - 1;
            if (index >= static_cast<int>(entries.size())) index = 0;
            if (!entries[static_cast<std::size_t>(index)].separator &&
                entries[static_cast<std::size_t>(index)].enabled) {
                selected = index;
                return;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// UIMenuBar — 窗口菜单栏
//
// 菜单栏自己只画顶层标签条；展开的菜单是挂在它下面的 UIMenuFlyout 子节点，
// 这样浮层既能收到点击（命中测试按节点矩形），又能靠 draws_on_top 画在所
// 有内容之上。标签宽度按文字估宽自适应，不再等分整条——等分会让 "File"
// 占 100px、文字左右各留 40px 空白，看起来位置全偏。
// ---------------------------------------------------------------------------
class UIMenuBar : public NodeUI {
public:
    // 顶层标题（"\n" 分隔）。menus 为空时用它（展开项也为空）。
    std::string items = "File\nEdit\nView\nBuild";
    // 完整定义：标题 + 每一项（含快捷键/分隔线/勾选）。优先于 items。
    std::vector<UIMenuDef> menus;
    int selected = 0;
    bool open = false;
    int open_index = -1;
    int hover_index = -1;
    int command = -1;
    float font_size = 14.0f;
    UIMenuFlyout* flyout = nullptr;

    UIMenuBar() {
        size = math::Vector2f(300.0f, 32.0f);
        min_size = size;
        focusable = true;
        background_role = ui::UIThemeColor::PanelRaised;
        accent_role = ui::UIThemeColor::Selection;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UIMenuBar"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIMenuBar)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["items"] = items;
        out["selected"] = selected;
        out["font_size"] = font_size;
        nlohmann::json arr = nlohmann::json::array();
        for (const UIMenuDef& menu : menus) {
            nlohmann::json entry;
            entry["title"] = menu.title;
            nlohmann::json list = nlohmann::json::array();
            for (const UIMenuItem& item : menu.items) {
                list.push_back({{"label", item.label}, {"shortcut", item.shortcut},
                                {"command", item.command}, {"separator", item.separator},
                                {"checked", item.checked}, {"enabled", item.enabled},
                                {"submenu", item.submenu}, {"icon", item.icon_path}});
            }
            entry["items"] = list;
            arr.push_back(std::move(entry));
        }
        out["menus"] = arr;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        items = in.value("items", items);
        selected = in.value("selected", selected);
        font_size = in.value("font_size", font_size);
        menus.clear();
        if (in.contains("menus") && in["menus"].is_array()) {
            for (const auto& m : in["menus"]) {
                UIMenuDef menu;
                menu.title = m.value("title", "");
                if (m.contains("items") && m["items"].is_array()) {
                    for (const auto& e : m["items"]) {
                        UIMenuItem item;
                        item.label = e.value("label", "");
                        item.shortcut = e.value("shortcut", "");
                        item.command = e.value("command", -1);
                        item.separator = e.value("separator", false);
                        item.checked = e.value("checked", false);
                        item.enabled = e.value("enabled", true);
                        item.submenu = e.value("submenu", false);
                        item.icon_path = e.value("icon", "");
                        menu.items.push_back(std::move(item));
                    }
                }
                menus.push_back(std::move(menu));
            }
        }
    }

    std::vector<std::string> titles() const {
        if (!menus.empty()) {
            std::vector<std::string> out;
            out.reserve(menus.size());
            for (const UIMenuDef& menu : menus) out.push_back(menu.title);
            return out;
        }
        return ui_split_lines(items);
    }

    // 每个顶层标签的宽度（按文字自适应）。
    std::vector<float> tab_widths(const std::vector<std::string>& labels) const {
        std::vector<float> widths;
        widths.reserve(labels.size());
        for (const std::string& label : labels) {
            widths.push_back(std::max(48.0f,
                                      ui_estimate_text_width(label, font_size) + 26.0f));
        }
        return widths;
    }

    bool on_ui_event(const ui::UIEvent& event) override {
        const std::vector<std::string> labels = titles();
        if (labels.empty()) return false;
        const ui::UIRect bar = event.layout_rect;
        const std::vector<float> widths = tab_widths(labels);
        const int index = tab_at(bar, widths, event.position.x);

        if (event.type == ui::UIEventType::PointerMove) {
            hover_index = index;
            // 菜单已展开时指针划过别的顶层项直接切换（Windows 菜单栏的行为）。
            if (open_index >= 0 && index >= 0 && index != open_index) {
                open_menu(index, bar, widths, labels);
            }
            return index >= 0;
        }
        if (event.type == ui::UIEventType::Click ||
            event.type == ui::UIEventType::PointerDown) {
            if (index < 0) {
                close_menu();
                return false;
            }
            if (open_index == index) {
                close_menu();
            } else {
                open_menu(index, bar, widths, labels);
            }
            return true;
        }
        if (event.type == ui::UIEventType::KeyDown) {
            if (event.key == static_cast<int>(ui::UIKey::Left)) {
                selected = std::max(0, selected - 1);
                if (open_index >= 0) open_menu(selected, bar, widths, labels);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Right)) {
                selected = std::min(static_cast<int>(labels.size()) - 1, selected + 1);
                if (open_index >= 0) open_menu(selected, bar, widths, labels);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Down)) {
                open_menu(selected, bar, widths, labels);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Escape)) {
                close_menu();
                return true;
            }
        }
        return false;
    }

    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        const std::vector<std::string> labels = titles();
        if (labels.empty()) return;
        const std::vector<float> widths = tab_widths(labels);
        const float radius = ui::ui_theme_radius(ui::UIRadiusTier::Small);
        float x = rect.x;
        for (std::size_t i = 0; i < labels.size(); ++i) {
            const ui::UIRect tab{x, rect.y + 2.0f, widths[i],
                                 std::max(0.0f, rect.h - 4.0f)};
            if (open_index == static_cast<int>(i) || hover_index == static_cast<int>(i)) {
                draw_list.add_round_rect(tab, radius,
                                         ui::ui_theme_color(ui::UIThemeColor::ControlFillHover));
            }
            draw_list.add_text(tab, labels[i], font_size,
                               themed_color(foreground_role, 0xFFFFFFFFu),
                               ui::UITextAlign::Center);
            x += widths[i];
        }
    }

    // 展开第 index 个顶层菜单：把条目灌进 flyout 并放到标签正下方。
    void open_menu(int index, const ui::UIRect& bar,
                   const std::vector<float>& widths,
                   const std::vector<std::string>& labels) {
        if (index < 0 || index >= static_cast<int>(labels.size())) return;
        open_index = index;
        selected = index;
        open = true;
        if (!flyout) return;
        std::vector<UIMenuItem> entries;
        if (index < static_cast<int>(menus.size())) {
            entries = menus[static_cast<std::size_t>(index)].items;
        } else {
            // 只有标题字符串时退化成"每行一个命令"，至少是可点的。
            const std::vector<std::string> rows = ui_split_lines(items);
            if (index < static_cast<int>(rows.size())) {
                const std::vector<std::string> names =
                    ui_split_lines(rows[static_cast<std::size_t>(index)]);
                for (std::size_t i = 0; i < names.size(); ++i) {
                    UIMenuItem item;
                    item.label = names[i];
                    item.command = static_cast<int>(i);
                    entries.push_back(std::move(item));
                }
            }
        }
        flyout->set_entries(std::move(entries));
        float anchor_x = bar.x;
        for (int i = 0; i < index; ++i) anchor_x += widths[static_cast<std::size_t>(i)];
        // 右边界夹取：浮层超出菜单栏右边缘时整体左移，避免被窗口裁掉。
        const float limit = bar.right() - flyout->size.x;
        if (anchor_x > limit) anchor_x = std::max(bar.x, limit);
        flyout->place_at(anchor_x - bar.x, bar.h + 2.0f);
        flyout->set_open(true);
    }

    void close_menu() {
        open_index = -1;
        open = false;
        if (flyout) flyout->set_open(false);
    }

private:
    int tab_at(const ui::UIRect& bar, const std::vector<float>& widths, float px) const {
        float x = bar.x;
        for (std::size_t i = 0; i < widths.size(); ++i) {
            if (px >= x && px < x + widths[i]) return static_cast<int>(i);
            x += widths[i];
        }
        return -1;
    }
};

// ---------------------------------------------------------------------------
// UISeparator — 分隔线（CommandBar 的分组、设置页的分节）
// ---------------------------------------------------------------------------
class UISeparator : public NodeUI {
public:
    bool vertical = true;
    float inset = 6.0f;

    UISeparator() {
        size = math::Vector2f(1.0f, 24.0f);
        min_size = size;
        receives_input = false;
    }
    const char* type() const override { return "UISeparator"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UISeparator)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["vertical"] = vertical;
        out["inset"] = inset;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        vertical = in.value("vertical", vertical);
        inset = in.value("inset", inset);
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        const uint32_t color = ui::ui_with_alpha(
            ui::ui_theme_color(ui::UIThemeColor::ControlStroke), 0.9f);
        if (vertical) {
            const float x = rect.x + rect.w * 0.5f;
            draw_list.add_rect({x, rect.y + inset, 1.0f,
                                std::max(0.0f, rect.h - inset * 2.0f)}, color);
        } else {
            const float y = rect.y + rect.h * 0.5f;
            draw_list.add_rect({rect.x + inset, y,
                                std::max(0.0f, rect.w - inset * 2.0f), 1.0f}, color);
        }
    }
};

// ---------------------------------------------------------------------------
// UITitleBar — 窗口标题栏（左图标 + 标题，右侧最小化/最大化/关闭）
//
// 只负责画与被点：拖动窗口、真正的最小化由宿主在拿到 hot_zone 后处理。
// 三个系统按钮是纯 Line 画的（不依赖字体里有没有 ✕/▢ 这类字形）。
// ---------------------------------------------------------------------------
class UITitleBar : public NodeUI {
public:
    std::string title = "Gryce Engine";
    std::string icon_path;
    std::string subtitle;
    float font_size = 12.0f;
    float button_width = 46.0f;
    bool show_minimize = true;
    bool show_maximize = true;
    bool show_close = true;
    // 宿主读取后清零：1 = 关闭，2 = 最大化，3 = 最小化。
    int button_command = 0;
    int hover_button = -1;

    UITitleBar() {
        size = math::Vector2f(1200.0f, 40.0f);
        min_size = math::Vector2f(240.0f, 32.0f);
        background_role = ui::UIThemeColor::Window;
        foreground_role = ui::UIThemeColor::Text;
        receives_input = true;
    }
    const char* type() const override { return "UITitleBar"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UITitleBar)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["title"] = title;
        out["subtitle"] = subtitle;
        out["icon_path"] = icon_path;
        out["font_size"] = font_size;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        title = in.value("title", title);
        subtitle = in.value("subtitle", subtitle);
        icon_path = in.value("icon_path", "");
        font_size = in.value("font_size", font_size);
    }

    int button_count() const {
        return (show_minimize ? 1 : 0) + (show_maximize ? 1 : 0) + (show_close ? 1 : 0);
    }
    // 从右往左：关闭、最大化、最小化（与 Windows 一致）。
    ui::UIRect button_rect(const ui::UIRect& bar, int index) const {
        const int count = button_count();
        const float x = bar.right() - button_width * static_cast<float>(count - index);
        return {x, bar.y, button_width, bar.h};
    }
    int button_at(const ui::UIRect& bar, float px) const {
        const int count = button_count();
        for (int i = 0; i < count; ++i) {
            const ui::UIRect r = button_rect(bar, i);
            if (px >= r.x && px < r.right()) return i;
        }
        return -1;
    }
    // 第 index 个按钮对应的语义：0 关闭 / 1 最大化 / 2 最小化。
    int command_of(int index) const {
        if (show_close && index == 0) return 1;
        if (show_maximize && index == (show_close ? 1 : 0)) return 2;
        return 3;
    }

    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::PointerMove) {
            hover_button = button_at(event.layout_rect, event.position.x);
            return hover_button >= 0;
        }
        if (event.type == ui::UIEventType::Click) {
            const int index = button_at(event.layout_rect, event.position.x);
            if (index >= 0) {
                button_command = command_of(index);
                return true;
            }
        }
        return false;
    }

    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        // 标题栏底色比页面深一档（Mica 的近似）：不铺底会让标题浮在内容上。
        draw_list.add_rect(rect, themed_color(background_role,
                                              ui::ui_theme_color(ui::UIThemeColor::Window)));

        float text_x = rect.x + 14.0f;
        const ui::TextureRef icon = ui::ui_texture_cached(icon_path);
        if (icon.valid()) {
            draw_list.add_image({text_x, rect.y + (rect.h - 16.0f) * 0.5f, 16.0f, 16.0f},
                                icon.index, icon.generation);
            text_x += 24.0f;
        }
        const uint32_t text_color = themed_color(foreground_role,
                                                ui::ui_theme_color(ui::UIThemeColor::Text));
        if (!title.empty()) {
            draw_list.add_text({text_x, rect.y, 400.0f, rect.h}, title, font_size,
                               text_color, ui::UITextAlign::Left);
            if (!subtitle.empty()) {
                text_x += ui_estimate_text_width(title, font_size) + 10.0f;
            }
        }
        if (!subtitle.empty()) {
            draw_list.add_text({text_x, rect.y, 400.0f, rect.h}, subtitle, font_size,
                               ui::ui_theme_color(ui::UIThemeColor::TextMuted),
                               ui::UITextAlign::Left);
        }

        // 系统按钮：hover 是整块淡底（关闭是系统的红），图标是 1.6px 圆头线。
        const int count = button_count();
        for (int i = 0; i < count; ++i) {
            const ui::UIRect button = button_rect(rect, i);
            const bool is_close = command_of(i) == 1;
            const uint32_t hover_fill = is_close
                ? 0xC42B1CFFu
                : ui::ui_theme_color(ui::UIThemeColor::ControlFillHover);
            if (hover_button == i) {
                draw_list.add_rect(button, hover_fill);
            }
            const uint32_t glyph = (is_close && hover_button == i)
                ? 0xFFFFFFFFu
                : ui::ui_theme_color(ui::UIThemeColor::Text);
            const float cx = button.x + button.w * 0.5f;
            const float cy = button.y + button.h * 0.5f;
            if (is_close) {
                draw_list.add_line(cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f, 1.6f, glyph);
                draw_list.add_line(cx + 4.5f, cy - 4.5f, cx - 4.5f, cy + 4.5f, 1.6f, glyph);
            } else if (command_of(i) == 2) {
                const float h = 4.0f;
                draw_list.add_line(cx - h, cy - h, cx + h, cy - h, 1.6f, glyph);
                draw_list.add_line(cx + h, cy - h, cx + h, cy + h, 1.6f, glyph);
                draw_list.add_line(cx + h, cy + h, cx - h, cy + h, 1.6f, glyph);
                draw_list.add_line(cx - h, cy + h, cx - h, cy - h, 1.6f, glyph);
            } else {
                draw_list.add_line(cx - 5.0f, cy, cx + 5.0f, cy, 1.6f, glyph);
            }
        }
    }
};

// ---------------------------------------------------------------------------
// UICommandBar — 命令栏（图标按钮 + 分隔线 + 右对齐的"更多"）
//
// 只是布局容器：子控件用 Row 排布，自带 4px 内边距与 4px 间隔。命令按钮
// 建议用 UIIconButton（variant = Subtle、icon_path 指向 24px 图标）。
// ---------------------------------------------------------------------------
class UICommandBar : public UIPanel {
public:
    // WinUI 3 的命令栏是一条与页面同宽的条带：无圆角、无四周描边，只在底部
    // 留一条发丝分隔线。跟着 UIPanel 画 PanelRaised 圆角块会变成"一枚胶囊把
    // 所有按钮包起来"，和 Fluent 的观感差很远。
    bool show_strip = true;

    UICommandBar() {
        layout_mode = UILayoutMode::Row;
        padding = 4.0f;
        gap = 2.0f;
        size = math::Vector2f(600.0f, 44.0f);
        min_size = size;
        background_role = ui::UIThemeColor::PanelRaised;
        radius_tier = ui::UIRadiusTier::Medium;
        draws_background = false;
    }
    const char* type() const override { return "UICommandBar"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPanel::collect_type_views(out);
        out.push_back({std::type_index(typeid(UICommandBar)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (!show_strip) return;
        // 只画底边分隔线，不铺底色：命令栏贴在窗口底色上，任何填充都会变成
        // 一整条比页面亮的横带，反而更像"另一张卡片"。
        draw_list.add_rect({rect.x, rect.bottom() - 1.0f, rect.w, 1.0f},
                           ui::ui_with_alpha(
                               ui::ui_theme_color(ui::UIThemeColor::ControlStroke), 0.8f));
    }
};

// ---------------------------------------------------------------------------
// UIInfoBar — 行内提示条（成功/警告/错误 + 可关闭）
// ---------------------------------------------------------------------------
enum class UIInfoBarSeverity : uint8_t {
    Informational = 0,
    Success,
    Warning,
    Error,
};

class UIInfoBar : public NodeUI {
public:
    std::string title = "Title";
    std::string message;
    UIInfoBarSeverity severity = UIInfoBarSeverity::Informational;
    bool closable = true;
    bool dismissed = false;
    // 关闭按钮点击计数（宿主读它决定是否真的移除）。
    int close_requested = 0;
    int hover_close = 0;

    UIInfoBar() {
        size = math::Vector2f(520.0f, 56.0f);
        min_size = size;
        radius_tier = ui::UIRadiusTier::Medium;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UIInfoBar"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIInfoBar)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["title"] = title;
        out["message"] = message;
        out["severity"] = static_cast<int>(severity);
        out["closable"] = closable;
        out["dismissed"] = dismissed;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        title = in.value("title", title);
        message = in.value("message", message);
        severity = static_cast<UIInfoBarSeverity>(in.value("severity", 0));
        closable = in.value("closable", true);
        dismissed = in.value("dismissed", false);
    }
    bool is_visible_in_tree() const override {
        return !dismissed && NodeUI::is_visible_in_tree();
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (!closable) return false;
        const ui::UIRect button = close_rect(event.layout_rect);
        const bool inside = event.position.x >= button.x &&
                            event.position.x < button.right() &&
                            event.position.y >= button.y &&
                            event.position.y < button.bottom();
        if (event.type == ui::UIEventType::PointerMove) {
            hover_close = inside ? 1 : 0;
            return inside;
        }
        if ((event.type == ui::UIEventType::Click ||
             event.type == ui::UIEventType::PointerDown) && inside) {
            ++close_requested;
            dismissed = true;
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        uint32_t accent = ui::ui_theme_color(ui::UIThemeColor::Accent);
        switch (severity) {
            case UIInfoBarSeverity::Success:
                accent = ui::ui_theme_color(ui::UIThemeColor::Success); break;
            case UIInfoBarSeverity::Warning:
                accent = ui::ui_theme_color(ui::UIThemeColor::Warning); break;
            case UIInfoBarSeverity::Error:
                accent = ui::ui_theme_color(ui::UIThemeColor::Error); break;
            default: break;
        }
        const float radius = ui::ui_resolve_radius(corner_radius, radius_tier);
        float radii[4];
        ui::ui_fill_radii(radius, radii);
        // Fluent InfoBar：淡染的底 + 发丝边 + 顶部 1px 强调条。
        ui::UISurface surface;
        surface.rect = rect;
        for (int i = 0; i < 4; ++i) surface.radii[i] = radii[i];
        surface.fill = ui::ui_theme_mix(
            ui::ui_theme_color(ui::UIThemeColor::PanelRaised), accent, 0.10f);
        surface.border = ui::ui_with_alpha(
            ui::ui_theme_color(ui::UIThemeColor::ControlStroke), 1.0f);
        surface.border_width = 1.0f;
        ui::ui_paint_surface(draw_list, surface);
        const float top_radii[4] = {radius, radius, 0.0f, 0.0f};
        draw_list.add_round_rect({rect.x, rect.y, rect.w, 2.0f}, top_radii, accent);

        // 左侧严重度指示：实心圆 + 中心图标笔画（对勾/感叹号/信息点）。
        const float icon_cx = rect.x + 22.0f;
        const float icon_cy = rect.y + 22.0f;
        draw_list.add_round_rect({icon_cx - 9.0f, icon_cy - 9.0f, 18.0f, 18.0f},
                                 9.0f, accent);
        const uint32_t ink = ui::ui_color(1.0f, 1.0f, 1.0f, 0.95f);
        switch (severity) {
            case UIInfoBarSeverity::Success:
                draw_list.add_line(icon_cx - 4.0f, icon_cy, icon_cx - 1.0f, icon_cy + 3.5f,
                                   1.8f, ink);
                draw_list.add_line(icon_cx - 1.0f, icon_cy + 3.5f, icon_cx + 4.5f,
                                   icon_cy - 4.0f, 1.8f, ink);
                break;
            case UIInfoBarSeverity::Warning:
            case UIInfoBarSeverity::Error:
                draw_list.add_line(icon_cx, icon_cy - 5.0f, icon_cx, icon_cy + 1.0f,
                                   1.9f, ink);
                draw_list.add_line(icon_cx, icon_cy + 4.5f, icon_cx, icon_cy + 5.0f,
                                   1.9f, ink);
                break;
            default:
                draw_list.add_line(icon_cx, icon_cy - 4.5f, icon_cx, icon_cy - 1.5f,
                                   1.9f, ink);
                draw_list.add_line(icon_cx, icon_cy + 2.0f, icon_cx, icon_cy + 4.5f,
                                   1.9f, ink);
                break;
        }

        const float text_x = rect.x + 42.0f;
        const float text_w = std::max(0.0f, rect.w - (text_x - rect.x) -
                                               (closable ? 44.0f : 14.0f));
        if (message.empty()) {
            draw_list.add_text({text_x, rect.y, text_w, rect.h}, title, 14.0f,
                               themed_color(foreground_role, 0xFFFFFFFFu),
                               ui::UITextAlign::Left);
        } else {
            draw_list.add_text({text_x, rect.y + 8.0f, text_w, 20.0f}, title, 14.0f,
                               themed_color(foreground_role, 0xFFFFFFFFu),
                               ui::UITextAlign::Left);
            draw_list.add_text({text_x, rect.y + 26.0f, text_w, 20.0f}, message, 12.0f,
                               ui::ui_theme_color(ui::UIThemeColor::TextMuted),
                               ui::UITextAlign::Left);
        }

        if (closable) {
            const ui::UIRect button = close_rect(rect);
            if (hover_close) {
                draw_list.add_round_rect(button, 4.0f,
                    ui::ui_theme_color(ui::UIThemeColor::ControlFillHover));
            }
            const float cx = button.x + button.w * 0.5f;
            const float cy = button.y + button.h * 0.5f;
            const uint32_t glyph = ui::ui_theme_color(ui::UIThemeColor::Text);
            draw_list.add_line(cx - 4.0f, cy - 4.0f, cx + 4.0f, cy + 4.0f, 1.6f, glyph);
            draw_list.add_line(cx + 4.0f, cy - 4.0f, cx - 4.0f, cy + 4.0f, 1.6f, glyph);
        }
    }

    ui::UIRect close_rect(const ui::UIRect& rect) const {
        return {rect.right() - 38.0f, rect.y + (rect.h - 24.0f) * 0.5f, 24.0f, 24.0f};
    }
};

// ---------------------------------------------------------------------------
// UINavigationView — 左侧导航（图标 + 文字，选中项淡底 + 左侧指示条）
// ---------------------------------------------------------------------------
struct UINavItem {
    std::string label;
    std::string icon_path;
    // 底部固定项（设置/账户），会与主列表之间留一条分隔。
    bool footer = false;
};

class UINavigationView : public NodeUI {
public:
    std::string header = "Navigation";
    std::vector<UINavItem> items;
    int selected = 0;
    int activated = -1;
    float row_height = 40.0f;
    float font_size = 14.0f;
    int hover_index = -1;

    UINavigationView() {
        size = math::Vector2f(240.0f, 480.0f);
        min_size = math::Vector2f(160.0f, 120.0f);
        background_role = ui::UIThemeColor::Panel;
        accent_role = ui::UIThemeColor::Accent;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UINavigationView"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UINavigationView)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["header"] = header;
        out["selected"] = selected;
        out["row_height"] = row_height;
        nlohmann::json arr = nlohmann::json::array();
        for (const UINavItem& item : items) {
            arr.push_back({{"label", item.label}, {"icon", item.icon_path},
                           {"footer", item.footer}});
        }
        out["items"] = arr;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        header = in.value("header", header);
        selected = in.value("selected", selected);
        row_height = in.value("row_height", row_height);
        items.clear();
        if (in.contains("items") && in["items"].is_array()) {
            for (const auto& e : in["items"]) {
                UINavItem item;
                item.label = e.value("label", "");
                item.icon_path = e.value("icon", "");
                item.footer = e.value("footer", false);
                items.push_back(std::move(item));
            }
        }
    }

    // 每项的纵向起点：主列表在 header 下方顺排，footer 项从容器下沿往上排。
    float row_top(int index, const ui::UIRect& rect) const {
        int footer_count = 0;
        for (const UINavItem& item : items) footer_count += item.footer ? 1 : 0;
        const float header_h = header.empty() ? 8.0f : 48.0f;
        float y = rect.y + header_h;
        int footer_seen = 0;
        const float footer_base = rect.bottom() - 8.0f -
                                  row_height * static_cast<float>(footer_count);
        for (int i = 0; i < index && i < static_cast<int>(items.size()); ++i) {
            if (items[static_cast<std::size_t>(i)].footer) {
                ++footer_seen;
            } else {
                y += row_height;
            }
        }
        if (index >= 0 && index < static_cast<int>(items.size()) &&
            items[static_cast<std::size_t>(index)].footer) {
            return footer_base + row_height * static_cast<float>(footer_seen);
        }
        return y;
    }

    int item_at(const ui::UIRect& rect, float px, float py) const {
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const float y = row_top(i, rect);
            if (px >= rect.x && px < rect.right() && py >= y && py < y + row_height) {
                return i;
            }
        }
        return -1;
    }

    bool on_ui_event(const ui::UIEvent& event) override {
        const int index = item_at(event.layout_rect, event.position.x, event.position.y);
        if (event.type == ui::UIEventType::PointerMove) {
            hover_index = index;
            return index >= 0;
        }
        if (event.type == ui::UIEventType::Click) {
            if (index >= 0) {
                selected = index;
                activated = index;
                return true;
            }
        }
        return false;
    }

    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier),
                                 themed_color(background_role,
                                              ui::ui_theme_color(ui::UIThemeColor::Panel)));
        if (!header.empty()) {
            draw_list.add_text({rect.x + 16.0f, rect.y + 8.0f,
                                std::max(0.0f, rect.w - 32.0f), 32.0f},
                               header, 16.0f,
                               themed_color(foreground_role, 0xFFFFFFFFu),
                               ui::UITextAlign::Left);
        }
        const float radius = ui::ui_theme_radius(ui::UIRadiusTier::Small);
        const uint32_t accent = themed_color(accent_role,
                                             ui::ui_theme_color(ui::UIThemeColor::Accent));
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const UINavItem& item = items[static_cast<std::size_t>(i)];
            const float y = row_top(i, rect);
            const ui::UIRect row{rect.x + 6.0f, y, std::max(0.0f, rect.w - 12.0f),
                                 row_height - 4.0f};
            const bool is_selected = i == selected;
            if (is_selected) {
                draw_list.add_round_rect(row, radius,
                    ui::ui_theme_color(ui::UIThemeColor::ControlFillHover));
                // 左侧 3px 指示条：Fluent 的 NavigationView 选中标记。
                draw_list.add_round_rect({row.x + 2.0f, row.y + row.h * 0.28f, 3.0f,
                                          row.h * 0.44f},
                                         ui::ui_pill_radius(3.0f), accent);
            } else if (i == hover_index) {
                draw_list.add_round_rect(row, radius,
                    ui::ui_with_alpha(
                        ui::ui_theme_color(ui::UIThemeColor::ControlFillHover), 0.6f));
            }
            const float icon_x = row.x + 14.0f;
            const ui::TextureRef icon = ui::ui_texture_cached(item.icon_path);
            const uint32_t ink = is_selected
                ? themed_color(foreground_role, 0xFFFFFFFFu)
                : ui::ui_theme_color(ui::UIThemeColor::TextMuted);
            if (icon.valid()) {
                draw_list.add_image({icon_x, row.y + (row.h - 16.0f) * 0.5f, 16.0f, 16.0f},
                                    icon.index, icon.generation,
                                    0.0f, 0.0f, 1.0f, 1.0f,
                                    is_selected ? 0xFFFFFFFFu : ink);
            }
            draw_list.add_text({row.x + 40.0f, row.y,
                                std::max(0.0f, row.w - 52.0f), row.h},
                               item.label, font_size, ink, ui::UITextAlign::Left);
        }
    }
};

class UIMenu : public UIPopup {
public:
    std::string items = "Open\nSave\nSave As...\nExit";
    int selected = 0;
    int command = -1;
    float row_height = 32.0f;
    float font_size = 14.0f;
    math::Vector4f row_hover_color = math::Vector4f(0.16f, 0.27f, 0.42f, 1.0f);

    UIMenu() {
        z_index = 12000;
        visible = false;
        receives_input = false;
        size = math::Vector2f(220.0f, 120.0f);
        min_size = math::Vector2f(120.0f, 32.0f);
        padding = 4.0f;
        focusable = true;
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Selection;
    }
    const char* type() const override { return "UIMenu"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPopup::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIMenu)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIPopup::serialize(out);
        out["items"] = items;
        out["selected"] = selected;
        out["row_height"] = row_height;
        out["font_size"] = font_size;
    }
    void deserialize(const nlohmann::json& in) override {
        UIPopup::deserialize(in);
        items = in.value("items", items);
        selected = in.value("selected", selected);
        row_height = in.value("row_height", row_height);
        font_size = in.value("font_size", font_size);
        receives_input = visible;
    }
    void set_open(bool value) {
        visible = value;
        receives_input = value;
        if (value) {
            command = -1;
            const int count = static_cast<int>(ui_split_lines(items).size());
            size.y = std::max(row_height, row_height * static_cast<float>(count) +
                                             padding * 2.0f);
        }
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (!visible) return false;
        const auto values = ui_split_lines(items);
        if (values.empty()) return false;
        if (event.type == ui::UIEventType::PointerMove) {
            selected = std::clamp(static_cast<int>(
                (event.position.y - event.layout_rect.y - padding) /
                std::max(1.0f, row_height)), 0, static_cast<int>(values.size()) - 1);
            return true;
        }
        if (event.type == ui::UIEventType::Click ||
            event.type == ui::UIEventType::PointerDown) {
            command = selected;
            set_open(false);
            return true;
        }
        if (event.type != ui::UIEventType::KeyDown) return false;
        if (event.key == static_cast<int>(ui::UIKey::Down)) {
            selected = std::min(static_cast<int>(values.size()) - 1, selected + 1);
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Up)) {
            selected = std::max(0, selected - 1);
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Enter)) {
            command = selected;
            set_open(false);
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Escape)) {
            set_open(false);
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (!visible) return;
        // 浮层本体是卡片表面（圆角 + 发丝边 + 投影），里面每一项是 4px 圆角的
        // 淡底行，不再整行铺强调色。
        UIPanel::add_draw_commands(rect, draw_list);
        const auto values = ui_split_lines(items);
        const float radius = ui::ui_theme_radius(ui::UIRadiusTier::Small);
        for (std::size_t i = 0; i < values.size(); ++i) {
            const ui::UIRect row{rect.x + padding, rect.y + padding +
                                      row_height * static_cast<float>(i),
                                 std::max(0.0f, rect.w - padding * 2.0f), row_height};
            if (static_cast<int>(i) == selected) {
                draw_list.add_round_rect(row, radius,
                                         ui::ui_theme_color(ui::UIThemeColor::ControlFillHover));
            }
            draw_list.add_text({row.x + 10.0f, row.y, row.w - 20.0f, row.h},
                               values[i], font_size,
                               themed_color(foreground_role, 0xFFFFFFFFu));
        }
    }
};

class UIContextMenu : public UIMenu {
public:
    UIContextMenu() {
        z_index = 13000;
        items = "Copy\nPaste\nDuplicate\nDelete";
    }
    const char* type() const override { return "UIContextMenu"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIMenu::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIContextMenu)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
};

// ---------------------------------------------------------------------------
// Data and asset views
// ---------------------------------------------------------------------------

struct UITreeItem {
    std::string label;
    int depth = 0;
    bool expanded = true;
    bool has_children = false;
    bool enabled = true;
};

class UITreeView : public NodeUI {
public:
    std::vector<UITreeItem> items;
    int selected = -1;
    int activated = -1;
    float row_height = 24.0f;
    float indent = 18.0f;
    float scroll_y = 0.0f;
    float font_size = 14.0f;
    bool selection_changed = false;
    bool toggled = false;
    math::Vector4f background = math::Vector4f(0.055f, 0.065f, 0.085f, 1.0f);
    math::Vector4f selected_color = math::Vector4f(0.18f, 0.34f, 0.55f, 1.0f);

    UITreeView() {
        size = math::Vector2f(280.0f, 260.0f);
        min_size = math::Vector2f(100.0f, 60.0f);
        clip_contents = true;
        focusable = true;
        // 内容视图与所在卡片同层：比页面底再深一档会让列表看起来是凹陷的。
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Selection;
    }
    const char* type() const override { return "UITreeView"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UITreeView)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& item : items) {
            rows.push_back({
                {"label", item.label},
                {"depth", item.depth},
                {"expanded", item.expanded},
                {"has_children", item.has_children},
                {"enabled", item.enabled},
            });
        }
        out["items"] = std::move(rows);
        out["selected"] = selected;
        out["row_height"] = row_height;
        out["indent"] = indent;
        out["scroll_y"] = scroll_y;
        out["font_size"] = font_size;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        items.clear();
        if (in.contains("items") && in["items"].is_array()) {
            for (const auto& row : in["items"]) {
                UITreeItem item;
                item.label = row.value("label", "");
                item.depth = std::max(0, row.value("depth", 0));
                item.expanded = row.value("expanded", true);
                item.has_children = row.value("has_children", false);
                item.enabled = row.value("enabled", true);
                items.push_back(std::move(item));
            }
        }
        selected = in.value("selected", -1);
        row_height = in.value("row_height", row_height);
        indent = in.value("indent", indent);
        scroll_y = in.value("scroll_y", scroll_y);
        font_size = in.value("font_size", font_size);
    }
    std::vector<int> visible_indices() const {
        std::vector<int> visible;
        int hidden_below = -1;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const auto& item = items[static_cast<std::size_t>(i)];
            if (hidden_below >= 0 && item.depth > hidden_below) continue;
            hidden_below = -1;
            visible.push_back(i);
            if (item.has_children && !item.expanded) hidden_below = item.depth;
        }
        return visible;
    }
    void set_scroll(float value) {
        const auto visible = visible_indices();
        const float max_scroll = std::max(
            0.0f, row_height * static_cast<float>(visible.size()) - size.y);
        scroll_y = std::clamp(value, 0.0f, max_scroll);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        const auto visible = visible_indices();
        if (visible.empty()) return false;
        if (event.type == ui::UIEventType::Wheel) {
            set_scroll(scroll_y - event.wheel.y * row_height * 1.5f);
            return true;
        }
        if (event.type == ui::UIEventType::Click ||
            event.type == ui::UIEventType::PointerDown) {
            const int row = static_cast<int>(
                (event.position.y - event.layout_rect.y + scroll_y) /
                std::max(1.0f, row_height));
            if (row < 0 || row >= static_cast<int>(visible.size())) return true;
            const int index = visible[static_cast<std::size_t>(row)];
            auto& item = items[static_cast<std::size_t>(index)];
            if (!item.enabled) return true;
            const float arrow_x = event.layout_rect.x +
                indent * static_cast<float>(item.depth) + 4.0f;
            if (item.has_children && event.position.x >= arrow_x &&
                event.position.x <= arrow_x + 16.0f) {
                item.expanded = !item.expanded;
                toggled = true;
                return true;
            }
            if (selected != index) {
                selected = index;
                selection_changed = true;
            }
            if (event.type == ui::UIEventType::Click) activated = index;
            return true;
        }
        if (event.type != ui::UIEventType::KeyDown) return false;
        int visible_pos = -1;
        for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
            if (visible[static_cast<std::size_t>(i)] == selected) {
                visible_pos = i;
                break;
            }
        }
        if (event.key == static_cast<int>(ui::UIKey::Down)) {
            visible_pos = std::min(static_cast<int>(visible.size()) - 1,
                                   visible_pos + 1);
            selected = visible[static_cast<std::size_t>(std::max(0, visible_pos))];
            selection_changed = true;
            reveal_selected();
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Up)) {
            visible_pos = std::max(0, visible_pos - 1);
            selected = visible[static_cast<std::size_t>(visible_pos)];
            selection_changed = true;
            reveal_selected();
            return true;
        }
        if (selected < 0 || selected >= static_cast<int>(items.size())) return false;
        auto& item = items[static_cast<std::size_t>(selected)];
        if (event.key == static_cast<int>(ui::UIKey::Right) &&
            item.has_children && !item.expanded) {
            item.expanded = true;
            toggled = true;
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Left) &&
            item.has_children && item.expanded) {
            item.expanded = false;
            toggled = true;
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Enter)) {
            activated = selected;
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier), themed_color(
            background_role, ui::ui_color(background.x, background.y,
                                          background.z, background.w)));
        const auto visible = visible_indices();
        for (int row = 0; row < static_cast<int>(visible.size()); ++row) {
            const int index = visible[static_cast<std::size_t>(row)];
            const auto& item = items[static_cast<std::size_t>(index)];
            const ui::UIRect row_rect{
                rect.x, rect.y + row_height * static_cast<float>(row) - scroll_y,
                rect.w, row_height,
            };
            if (row_rect.bottom() <= rect.y || row_rect.y >= rect.bottom()) continue;
            if (index == selected) {
                ui::ui_paint_row_selection(draw_list, row_rect,
                                           ui::ui_theme_color(accent_role));
            }
            const float x = row_rect.x + indent * static_cast<float>(item.depth);
            if (item.has_children) {
                draw_list.add_text({x + 4.0f, row_rect.y, 16.0f, row_rect.h},
                                   item.expanded ? "v" : ">",
                                   13.0f, ui::ui_theme_color(ui::UIThemeColor::TextMuted));
            }
            draw_list.add_text({x + 20.0f, row_rect.y,
                                std::max(0.0f, row_rect.w - x - 24.0f), row_rect.h},
                               item.label, font_size,
                               item.enabled
                                   ? themed_color(foreground_role, 0xE9EDF5FFu)
                                   : ui::ui_theme_color(ui::UIThemeColor::TextMuted));
        }
    }

private:
    void reveal_selected() {
        const auto visible = visible_indices();
        int row = 0;
        for (int index : visible) {
            if (index == selected) break;
            ++row;
        }
        const float top = row_height * static_cast<float>(row);
        if (top < scroll_y) scroll_y = top;
        if (top + row_height > scroll_y + size.y) {
            scroll_y = top + row_height - size.y;
        }
        set_scroll(scroll_y);
    }
};

class UITableView : public NodeUI {
public:
    std::string columns = "Name\nType\nSize";
    std::string rows = "Main.gesc|Scene|4 KB\nplayer.js|Script|8 KB";
    char separator = '|';
    int selected = -1;
    int activated = -1;
    float row_height = 24.0f;
    float header_height = 26.0f;
    float scroll_y = 0.0f;
    float font_size = 13.0f;
    bool selection_changed = false;
    math::Vector4f background = math::Vector4f(0.055f, 0.065f, 0.085f, 1.0f);
    math::Vector4f header_color = math::Vector4f(0.10f, 0.12f, 0.16f, 1.0f);
    math::Vector4f selected_color = math::Vector4f(0.18f, 0.34f, 0.55f, 1.0f);

    UITableView() {
        size = math::Vector2f(520.0f, 240.0f);
        min_size = math::Vector2f(180.0f, 70.0f);
        clip_contents = true;
        focusable = true;
        // 内容视图与所在卡片同层：比页面底再深一档会让列表看起来是凹陷的。
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Selection;
    }
    const char* type() const override { return "UITableView"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UITableView)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["columns"] = columns;
        out["rows"] = rows;
        out["separator"] = static_cast<int>(separator);
        out["selected"] = selected;
        out["row_height"] = row_height;
        out["header_height"] = header_height;
        out["scroll_y"] = scroll_y;
        out["font_size"] = font_size;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        columns = in.value("columns", columns);
        rows = in.value("rows", rows);
        separator = static_cast<char>(in.value(
            "separator", static_cast<int>(separator)));
        selected = in.value("selected", selected);
        row_height = in.value("row_height", row_height);
        header_height = in.value("header_height", header_height);
        scroll_y = in.value("scroll_y", scroll_y);
        font_size = in.value("font_size", font_size);
    }
    std::vector<std::vector<std::string>> row_cells() const {
        std::vector<std::vector<std::string>> out;
        for (const auto& line : ui_split_lines(rows)) {
            std::vector<std::string> cells;
            std::istringstream stream(line);
            std::string cell;
            while (std::getline(stream, cell, separator)) cells.push_back(cell);
            out.push_back(std::move(cells));
        }
        return out;
    }
    void set_scroll(float value) {
        const float content = row_height * static_cast<float>(row_cells().size());
        const float max_scroll = std::max(0.0f, content - (size.y - header_height));
        scroll_y = std::clamp(value, 0.0f, max_scroll);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        const int count = static_cast<int>(row_cells().size());
        if (event.type == ui::UIEventType::Wheel) {
            set_scroll(scroll_y - event.wheel.y * row_height * 1.5f);
            return true;
        }
        if (event.type == ui::UIEventType::Click ||
            event.type == ui::UIEventType::PointerDown) {
            if (event.position.y < event.layout_rect.y + header_height) return true;
            const int index = static_cast<int>(
                (event.position.y - event.layout_rect.y - header_height + scroll_y) /
                std::max(1.0f, row_height));
            if (index >= 0 && index < count) {
                if (selected != index) {
                    selected = index;
                    selection_changed = true;
                }
                if (event.type == ui::UIEventType::Click) activated = selected;
            }
            return true;
        }
        if (event.type != ui::UIEventType::KeyDown) return false;
        if (event.key == static_cast<int>(ui::UIKey::Down)) {
            selected = std::min(count - 1, selected + 1);
            selection_changed = true;
            reveal_selected();
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Up)) {
            selected = std::max(0, selected - 1);
            selection_changed = true;
            reveal_selected();
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Enter)) {
            activated = selected;
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier), themed_color(
            background_role, ui::ui_color(background.x, background.y,
                                          background.z, background.w)));
        const auto headers = ui_split_lines(columns);
        const auto cells = row_cells();
        int column_count = static_cast<int>(headers.size());
        for (const auto& row : cells) {
            column_count = std::max(column_count, static_cast<int>(row.size()));
        }
        column_count = std::max(1, column_count);
        const float column_width = rect.w / static_cast<float>(column_count);
        draw_list.add_rect({rect.x, rect.y, rect.w, header_height},
                           ui::ui_theme_color(ui::UIThemeColor::PanelRaised));
        for (int column = 0; column < column_count; ++column) {
            const std::string text = column < static_cast<int>(headers.size())
                                         ? headers[static_cast<std::size_t>(column)]
                                         : std::string{};
            draw_list.add_text({rect.x + column_width * static_cast<float>(column) + 6.0f,
                                rect.y, column_width - 12.0f, header_height},
                               text, font_size,
                               ui::ui_theme_color(ui::UIThemeColor::TextMuted));
        }
        for (int row = 0; row < static_cast<int>(cells.size()); ++row) {
            const ui::UIRect row_rect{
                rect.x, rect.y + header_height +
                    row_height * static_cast<float>(row) - scroll_y,
                rect.w, row_height,
            };
            if (row_rect.bottom() <= rect.y + header_height ||
                row_rect.y >= rect.bottom()) continue;
            if (row == selected) {
                ui::ui_paint_row_selection(draw_list, row_rect,
                                           ui::ui_theme_color(accent_role));
            }
            for (int column = 0; column < static_cast<int>(cells[row].size()); ++column) {
                draw_list.add_text({
                    rect.x + column_width * static_cast<float>(column) + 6.0f,
                    row_rect.y, column_width - 12.0f, row_rect.h,
                }, cells[row][static_cast<std::size_t>(column)],
                   font_size, themed_color(foreground_role, 0xFFFFFFFFu));
            }
        }
    }

private:
    void reveal_selected() {
        if (selected < 0) return;
        const float top = row_height * static_cast<float>(selected);
        const float view_height = std::max(1.0f, size.y - header_height);
        if (top < scroll_y) scroll_y = top;
        if (top + row_height > scroll_y + view_height) {
            scroll_y = top + row_height - view_height;
        }
        set_scroll(scroll_y);
    }
};

class UIPropertyGrid : public NodeUI {
public:
    std::string items =
        "Name|Cube\nPosition|0, 0, 0\nRotation|0, 0, 0\nScale|1, 1, 1";
    int selected = -1;
    int activated = -1;
    float row_height = 26.0f;
    float label_width_ratio = 0.42f;
    float scroll_y = 0.0f;
    bool selection_changed = false;
    math::Vector4f background = math::Vector4f(0.065f, 0.075f, 0.10f, 1.0f);
    math::Vector4f selected_color = math::Vector4f(0.16f, 0.28f, 0.44f, 1.0f);

    UIPropertyGrid() {
        size = math::Vector2f(320.0f, 300.0f);
        min_size = math::Vector2f(140.0f, 60.0f);
        clip_contents = true;
        focusable = true;
        // 内容视图与所在卡片同层：比页面底再深一档会让列表看起来是凹陷的。
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Selection;
    }
    const char* type() const override { return "UIPropertyGrid"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIPropertyGrid)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["items"] = items;
        out["selected"] = selected;
        out["row_height"] = row_height;
        out["label_width_ratio"] = label_width_ratio;
        out["scroll_y"] = scroll_y;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        items = in.value("items", items);
        selected = in.value("selected", selected);
        row_height = in.value("row_height", row_height);
        label_width_ratio = in.value("label_width_ratio", label_width_ratio);
        scroll_y = in.value("scroll_y", scroll_y);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        const int count = static_cast<int>(ui_split_lines(items).size());
        if (event.type == ui::UIEventType::Wheel) {
            const float max_scroll = std::max(0.0f,
                row_height * static_cast<float>(count) - size.y);
            scroll_y = std::clamp(scroll_y - event.wheel.y * row_height * 1.5f,
                                  0.0f, max_scroll);
            return true;
        }
        if (event.type == ui::UIEventType::Click ||
            event.type == ui::UIEventType::PointerDown) {
            const int index = static_cast<int>(
                (event.position.y - event.layout_rect.y + scroll_y) /
                std::max(1.0f, row_height));
            if (index >= 0 && index < count) {
                if (selected != index) {
                    selected = index;
                    selection_changed = true;
                }
                if (event.type == ui::UIEventType::Click) activated = index;
            }
            return true;
        }
        if (event.type == ui::UIEventType::KeyDown) {
            if (event.key == static_cast<int>(ui::UIKey::Down)) {
                selected = std::min(count - 1, selected + 1);
                selection_changed = true;
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Up)) {
                selected = std::max(0, selected - 1);
                selection_changed = true;
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
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier), themed_color(
            background_role, ui::ui_color(background.x, background.y,
                                          background.z, background.w)));
        const auto rows = ui_split_lines(items);
        const float label_width = rect.w * std::clamp(label_width_ratio, 0.15f, 0.8f);
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const ui::UIRect row{rect.x,
                                 rect.y + row_height * static_cast<float>(i) - scroll_y,
                                 rect.w, row_height};
            if (row.bottom() <= rect.y || row.y >= rect.bottom()) continue;
            if (i == selected) {
                ui::ui_paint_row_selection(draw_list, row,
                                           ui::ui_theme_color(accent_role));
            }
            const auto parts = ui_split_pipe(rows[static_cast<std::size_t>(i)]);
            const std::string name = parts.empty() ? "" : parts[0];
            const std::string value = parts.size() > 1 ? parts[1] : "";
            draw_list.add_text({row.x + 6.0f, row.y, label_width - 12.0f, row.h},
                               name, 13.0f,
                               ui::ui_theme_color(ui::UIThemeColor::TextMuted));
            draw_list.add_text({row.x + label_width + 6.0f, row.y,
                                std::max(0.0f, row.w - label_width - 12.0f), row.h},
                               value, 13.0f,
                               themed_color(foreground_role, 0xFFFFFFFFu));
        }
    }
};

enum class UIPropertyKind : uint8_t {
    Boolean = 0,
    Integer,
    Float,
    Vector2,
    Vector3,
    Vector4,
    String,
    Enum,
};

struct UIPropertyRow {
    std::string label;
    std::string value;
    std::string property_id;
    uint64_t owner_id = 0;
    UIPropertyKind kind = UIPropertyKind::Float;
    float minimum = 0.0f;
    float maximum = 0.0f;
    bool read_only = false;
};

class UIPropertyEditor : public NodeUI {
public:
    std::vector<UIPropertyRow> rows;
    int selected = -1;
    int committed = -1;
    int activated = -1;
    float row_height = 26.0f;
    float label_width_ratio = 0.42f;
    float scroll_y = 0.0f;
    bool editing = false;
    std::string edit_buffer;
    math::Vector4f background = math::Vector4f(0.065f, 0.075f, 0.10f, 1.0f);
    math::Vector4f selected_color = math::Vector4f(0.16f, 0.28f, 0.44f, 1.0f);

    UIPropertyEditor() {
        size = math::Vector2f(320.0f, 300.0f);
        min_size = math::Vector2f(160.0f, 80.0f);
        clip_contents = true;
        focusable = true;
        // 内容视图与所在卡片同层：比页面底再深一档会让列表看起来是凹陷的。
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Selection;
    }
    const char* type() const override { return "UIPropertyEditor"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIPropertyEditor)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        nlohmann::json values = nlohmann::json::array();
        for (const auto& row : rows) {
            values.push_back({
                {"label", row.label},
                {"value", row.value},
                {"property_id", row.property_id},
                {"owner_id", row.owner_id},
                {"kind", static_cast<int>(row.kind)},
                {"minimum", row.minimum},
                {"maximum", row.maximum},
                {"read_only", row.read_only},
            });
        }
        out["rows"] = std::move(values);
        out["selected"] = selected;
        out["row_height"] = row_height;
        out["label_width_ratio"] = label_width_ratio;
        out["scroll_y"] = scroll_y;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        rows.clear();
        for (const auto& source : in.value("rows", nlohmann::json::array())) {
            UIPropertyRow row;
            row.label = source.value("label", "");
            row.value = source.value("value", "");
            row.property_id = source.value("property_id", "");
            row.owner_id = source.value("owner_id", uint64_t{0});
            row.kind = static_cast<UIPropertyKind>(
                std::clamp(source.value("kind", 0), 0, 7));
            row.minimum = source.value("minimum", 0.0f);
            row.maximum = source.value("maximum", 0.0f);
            row.read_only = source.value("read_only", false);
            rows.push_back(std::move(row));
        }
        selected = in.value("selected", -1);
        row_height = in.value("row_height", row_height);
        label_width_ratio = in.value("label_width_ratio", label_width_ratio);
        scroll_y = in.value("scroll_y", scroll_y);
        editing = false;
        edit_buffer.clear();
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::FocusLost) {
            editing = false;
            edit_buffer.clear();
            return false;
        }
        if (event.type == ui::UIEventType::Wheel) {
            const float max_scroll = std::max(
                0.0f, row_height * static_cast<float>(rows.size()) - size.y);
            scroll_y = std::clamp(
                scroll_y - event.wheel.y * row_height * 1.5f, 0.0f, max_scroll);
            return true;
        }
        if (event.type == ui::UIEventType::Click ||
            event.type == ui::UIEventType::PointerDown) {
            const int index = static_cast<int>(
                (event.position.y - event.layout_rect.y + scroll_y) /
                std::max(1.0f, row_height));
            if (index >= 0 && index < static_cast<int>(rows.size())) {
                selected = index;
                editing = false;
                edit_buffer.clear();
                if (event.type == ui::UIEventType::Click) activated = index;
            }
            return true;
        }
        if (event.type == ui::UIEventType::Character && editing) {
            if (event.codepoint >= 32 && event.codepoint != 127 &&
                edit_buffer.size() < 256) {
                ui_detail::append_utf8(edit_buffer, event.codepoint);
            }
            return true;
        }
        if (event.type != ui::UIEventType::KeyDown) return false;
        if (editing) {
            if (event.key == static_cast<int>(ui::UIKey::Enter)) {
                if (selected >= 0 && selected < static_cast<int>(rows.size())) {
                    rows[static_cast<std::size_t>(selected)].value = edit_buffer;
                    committed = selected;
                }
                editing = false;
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Escape)) {
                editing = false;
                edit_buffer.clear();
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Backspace)) {
                if (!edit_buffer.empty()) {
                    const std::size_t previous =
                        ui_detail::previous_codepoint(edit_buffer, edit_buffer.size());
                    edit_buffer.erase(previous);
                }
                return true;
            }
            return false;
        }
        if (event.key == static_cast<int>(ui::UIKey::Down)) {
            selected = std::min(static_cast<int>(rows.size()) - 1, selected + 1);
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Up)) {
            selected = std::max(0, selected - 1);
            return true;
        }
        if (selected < 0 || selected >= static_cast<int>(rows.size())) return false;
        auto& row = rows[static_cast<std::size_t>(selected)];
        if (row.read_only) return false;
        if (row.kind == UIPropertyKind::Boolean &&
            (event.key == static_cast<int>(ui::UIKey::Enter) || event.key == 32)) {
            row.value = row.value == "true" ? "false" : "true";
            committed = selected;
            return true;
        }
        if (event.key == static_cast<int>(ui::UIKey::Enter)) {
            editing = true;
            edit_buffer = row.value;
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier), themed_color(
            background_role, ui::ui_color(background.x, background.y,
                                          background.z, background.w)));
        const float label_width = rect.w * std::clamp(label_width_ratio, 0.15f, 0.8f);
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            const ui::UIRect row_rect{
                rect.x, rect.y + row_height * static_cast<float>(i) - scroll_y,
                rect.w, row_height,
            };
            if (row_rect.bottom() <= rect.y || row_rect.y >= rect.bottom()) continue;
            if (i == selected) {
                ui::ui_paint_row_selection(draw_list, row_rect,
                                           ui::ui_theme_color(accent_role));
            }
            const auto& row = rows[static_cast<std::size_t>(i)];
            draw_list.add_text({row_rect.x + 6.0f, row_rect.y,
                                label_width - 12.0f, row_rect.h},
                               row.label, 13.0f,
                               row.read_only
                                   ? ui::ui_theme_color(ui::UIThemeColor::TextMuted)
                                   : ui::ui_theme_color(ui::UIThemeColor::TextMuted));
            const std::string& value = editing && i == selected
                                           ? edit_buffer : row.value;
            draw_list.add_text({row_rect.x + label_width + 6.0f, row_rect.y,
                                std::max(0.0f, row_rect.w - label_width - 12.0f),
                                row_rect.h},
                               value, 13.0f,
                               themed_color(foreground_role, 0xFFFFFFFFu));
        }
    }
};

class UIAssetGrid : public NodeUI {
public:
    std::string items = "cube.obj\nmain.gesc\nplayer.js\nrock.png";
    int selected = -1;
    int activated = -1;
    int columns = 4;
    math::Vector2f tile_size = math::Vector2f(88.0f, 84.0f);
    float scroll_y = 0.0f;
    float font_size = 12.0f;
    bool selection_changed = false;
    math::Vector4f background = math::Vector4f(0.05f, 0.06f, 0.08f, 1.0f);
    math::Vector4f tile_color = math::Vector4f(0.085f, 0.10f, 0.13f, 1.0f);
    math::Vector4f selected_color = math::Vector4f(0.20f, 0.37f, 0.58f, 1.0f);

    UIAssetGrid() {
        size = math::Vector2f(400.0f, 260.0f);
        min_size = math::Vector2f(120.0f, 90.0f);
        clip_contents = true;
        focusable = true;
        // 内容视图与所在卡片同层：比页面底再深一档会让列表看起来是凹陷的。
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Selection;
    }
    const char* type() const override { return "UIAssetGrid"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIAssetGrid)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["items"] = items;
        out["selected"] = selected;
        out["columns"] = columns;
        out["tile_size"] = {tile_size.x, tile_size.y};
        out["scroll_y"] = scroll_y;
        out["font_size"] = font_size;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        items = in.value("items", items);
        selected = in.value("selected", selected);
        columns = std::max(1, in.value("columns", columns));
        const auto tile = in.value("tile_size", std::vector<float>{
            tile_size.x, tile_size.y});
        if (tile.size() >= 2) tile_size = math::Vector2f(tile[0], tile[1]);
        scroll_y = in.value("scroll_y", scroll_y);
        font_size = in.value("font_size", font_size);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        const int count = static_cast<int>(ui_split_lines(items).size());
        if (event.type == ui::UIEventType::Wheel) {
            const int row_count = (count + columns - 1) / std::max(1, columns);
            const float max_scroll = std::max(
                0.0f, tile_size.y * static_cast<float>(row_count) - size.y);
            scroll_y = std::clamp(scroll_y - event.wheel.y * tile_size.y,
                                  0.0f, max_scroll);
            return true;
        }
        if (event.type == ui::UIEventType::Click ||
            event.type == ui::UIEventType::PointerDown) {
            const float local_x = event.position.x - event.layout_rect.x;
            const float local_y = event.position.y - event.layout_rect.y + scroll_y;
            const int column = static_cast<int>(local_x /
                std::max(1.0f, tile_size.x));
            const int row = static_cast<int>(local_y /
                std::max(1.0f, tile_size.y));
            const int index = row * columns + column;
            if (column >= 0 && column < columns && index >= 0 && index < count) {
                if (selected != index) {
                    selected = index;
                    selection_changed = true;
                }
                if (event.type == ui::UIEventType::Click) activated = index;
            }
            return true;
        }
        if (event.type == ui::UIEventType::KeyDown) {
            if (event.key == static_cast<int>(ui::UIKey::Left)) {
                selected = std::max(0, selected - 1);
                selection_changed = true;
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Right)) {
                selected = std::min(count - 1, selected + 1);
                selection_changed = true;
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Up)) {
                selected = std::max(0, selected - columns);
                selection_changed = true;
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Down)) {
                selected = std::min(count - 1, selected + columns);
                selection_changed = true;
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
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier), themed_color(
            background_role, ui::ui_color(background.x, background.y,
                                          background.z, background.w)));
        const auto values = ui_split_lines(items);
        for (int i = 0; i < static_cast<int>(values.size()); ++i) {
            const int row = i / columns;
            const int column = i % columns;
            const ui::UIRect tile{
                rect.x + tile_size.x * static_cast<float>(column) + 2.0f,
                rect.y + tile_size.y * static_cast<float>(row) - scroll_y + 2.0f,
                tile_size.x - 4.0f, tile_size.y - 4.0f,
            };
            if (tile.bottom() <= rect.y || tile.y >= rect.bottom()) continue;
            draw_list.add_rect(tile, i == selected
                ? ui::ui_theme_color(accent_role)
                : ui::ui_theme_color(ui::UIThemeColor::PanelRaised));
            draw_list.add_rect({tile.x + 12.0f, tile.y + 8.0f,
                                tile.w - 24.0f, 44.0f},
                               ui::ui_theme_color(ui::UIThemeColor::Input));
            draw_list.add_text({tile.x + tile.w * 0.5f - 10.0f,
                                tile.y + 16.0f, 20.0f, 28.0f},
                               "[]", 18.0f, 0xAEBBCCFFu);
            draw_list.add_text({tile.x + 5.0f, tile.y + 56.0f,
                                tile.w - 10.0f,
                                std::max(1.0f, tile.h - 58.0f)},
                               values[static_cast<std::size_t>(i)],
                               font_size,
                               themed_color(foreground_role, 0xFFFFFFFFu));
        }
    }
};

class UIStatusBar : public NodeUI {
public:
    std::string text = "Ready";
    std::string right_text = "GryceEngine";
    float font_size = 13.0f;
    math::Vector4f background = math::Vector4f(0.055f, 0.065f, 0.085f, 1.0f);

    UIStatusBar() {
        size = math::Vector2f(800.0f, 24.0f);
        min_size = math::Vector2f(120.0f, 18.0f);
        // 内容视图与所在卡片同层：比页面底再深一档会让列表看起来是凹陷的。
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::TextMuted;
    }
    const char* type() const override { return "UIStatusBar"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIStatusBar)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["text"] = text;
        out["right_text"] = right_text;
        out["font_size"] = font_size;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        text = in.value("text", text);
        right_text = in.value("right_text", right_text);
        font_size = in.value("font_size", font_size);
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier), themed_color(
            background_role, ui::ui_color(background.x, background.y,
                                          background.z, background.w)));
        draw_list.add_text({rect.x + 8.0f, rect.y, rect.w * 0.6f, rect.h},
                           text, font_size,
                           themed_color(foreground_role, 0xC7D1DFFFu));
        draw_list.add_text({rect.x + rect.w * 0.62f, rect.y,
                            rect.w * 0.38f - 8.0f, rect.h},
                           right_text, font_size,
                           ui::ui_theme_color(ui::UIThemeColor::TextMuted));
    }
};

// ---------------------------------------------------------------------------
// Dialogs and notifications
// ---------------------------------------------------------------------------

class UIMessageDialog : public UIModal {
public:
    std::string title = "Message";
    std::string message = "Operation completed.";
    std::string button_text = "OK";
    bool accepted = false;
    bool closed = false;

    UIMessageDialog() {
        visible = false;
        receives_input = false;
        size = math::Vector2f(440.0f, 190.0f);
        min_size = size;
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UIMessageDialog"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIModal::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIMessageDialog)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIModal::serialize(out);
        out["title"] = title;
        out["message"] = message;
        out["button_text"] = button_text;
    }
    void deserialize(const nlohmann::json& in) override {
        UIModal::deserialize(in);
        title = in.value("title", title);
        message = in.value("message", message);
        button_text = in.value("button_text", button_text);
        receives_input = visible;
    }
    void set_open(bool value) {
        visible = value;
        receives_input = value;
        if (value) {
            closed = false;
            accepted = false;
        }
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (!visible) return false;
        if (event.type == ui::UIEventType::Click) {
            accepted = true;
            closed = true;
            set_open(false);
            return true;
        }
        if (event.type == ui::UIEventType::KeyDown &&
            (event.key == static_cast<int>(ui::UIKey::Enter) ||
             event.key == static_cast<int>(ui::UIKey::Escape))) {
            accepted = event.key == static_cast<int>(ui::UIKey::Enter);
            closed = true;
            set_open(false);
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (!visible) return;
        UIPanel::add_draw_commands(rect, draw_list);
        draw_list.add_text({rect.x + 16.0f, rect.y + 10.0f,
                            rect.w - 32.0f, 28.0f},
                           title, 18.0f,
                           themed_color(foreground_role, 0xFFFFFFFFu));
        draw_list.add_text({rect.x + 16.0f, rect.y + 48.0f,
                            rect.w - 32.0f, rect.h - 100.0f},
                           message, 14.0f,
                           ui::ui_theme_color(ui::UIThemeColor::TextMuted));
        // 对话框按钮与页面上的按钮走同一套形态：4px 圆角、文字居中、主操作
        // 用强调色实底。手绘直角矩形会和界面其余部分明显割裂。
        const float button_radius = ui::ui_theme_radius(ui::UIRadiusTier::Small);
        const ui::UIRect button{rect.right() - 112.0f, rect.bottom() - 44.0f,
                                96.0f, 32.0f};
        draw_list.add_round_rect(button, button_radius,
                                 ui::ui_theme_mix(
                                     ui::ui_theme_color(ui::UIThemeColor::Accent),
                                     ui::ui_theme_color(ui::UIThemeColor::AccentHover),
                                     hover_amount()),
                                 0u, 1.0f, ui::ui_with_alpha(0xFFFFFFFFu, 0.10f));
        draw_list.add_text(button, button_text, 14.0f, 0xFFFFFFFFu,
                           ui::UITextAlign::Center);
    }
};

class UIConfirmDialog : public UIModal {
public:
    std::string title = "Confirm";
    std::string message = "Apply this operation?";
    std::string accept_text = "Confirm";
    std::string cancel_text = "Cancel";
    int result = 0;
    bool closed = false;

    UIConfirmDialog() {
        visible = false;
        receives_input = false;
        size = math::Vector2f(460.0f, 190.0f);
        min_size = size;
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UIConfirmDialog"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIModal::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIConfirmDialog)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIModal::serialize(out);
        out["title"] = title;
        out["message"] = message;
        out["accept_text"] = accept_text;
        out["cancel_text"] = cancel_text;
    }
    void deserialize(const nlohmann::json& in) override {
        UIModal::deserialize(in);
        title = in.value("title", title);
        message = in.value("message", message);
        accept_text = in.value("accept_text", accept_text);
        cancel_text = in.value("cancel_text", cancel_text);
        receives_input = visible;
    }
    void set_open(bool value) {
        visible = value;
        receives_input = value;
        if (value) {
            closed = false;
            result = 0;
        }
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (!visible) return false;
        if (event.type == ui::UIEventType::Click) {
            const float middle = event.layout_rect.x + event.layout_rect.w * 0.5f;
            result = event.position.x >= middle ? 1 : -1;
            closed = true;
            set_open(false);
            return true;
        }
        if (event.type == ui::UIEventType::KeyDown) {
            if (event.key == static_cast<int>(ui::UIKey::Enter)) {
                result = 1;
            } else if (event.key == static_cast<int>(ui::UIKey::Escape)) {
                result = -1;
            } else {
                return false;
            }
            closed = true;
            set_open(false);
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (!visible) return;
        UIPanel::add_draw_commands(rect, draw_list);
        draw_list.add_text({rect.x + 16.0f, rect.y + 10.0f,
                            rect.w - 32.0f, 28.0f},
                           title, 18.0f,
                           themed_color(foreground_role, 0xFFFFFFFFu));
        draw_list.add_text({rect.x + 16.0f, rect.y + 48.0f,
                            rect.w - 32.0f, rect.h - 100.0f},
                           message, 14.0f,
                           ui::ui_theme_color(ui::UIThemeColor::TextMuted));
        // 次操作是"控件填充 + 发丝边"，主操作是强调色实底；两者都居中、
        // 同为 32 高，和窗口里的按钮一致。
        const float button_radius = ui::ui_theme_radius(ui::UIRadiusTier::Small);
        const float button_y = rect.bottom() - 44.0f;
        const ui::UIRect cancel{rect.x + 16.0f, button_y, 104.0f, 32.0f};
        const ui::UIRect accept{rect.right() - 120.0f, button_y, 104.0f, 32.0f};
        draw_list.add_round_rect(cancel, button_radius,
                                 ui::ui_control_fill(hover_amount(), 0.0f),
                                 0u, 1.0f,
                                 ui::ui_control_stroke(hover_amount(), 0.0f));
        draw_list.add_round_rect(accept, button_radius,
                                 ui::ui_theme_color(ui::UIThemeColor::Accent),
                                 0u, 1.0f, ui::ui_with_alpha(0xFFFFFFFFu, 0.10f));
        draw_list.add_text(cancel, cancel_text, 14.0f,
                           ui::ui_theme_color(ui::UIThemeColor::Text),
                           ui::UITextAlign::Center);
        draw_list.add_text(accept, accept_text, 14.0f, 0xFFFFFFFFu,
                           ui::UITextAlign::Center);
    }
};

enum class UIFileDialogMode : uint8_t {
    OpenFile = 0,
    SaveFile,
    PickFolder,
};

class UIFileDialog : public UIModal {
public:
    std::string title = "Open File";
    std::string current_path = "res:/";
    std::string entries = "scenes\nscripts\nmodels";
    int selected = -1;
    int result = 0;
    bool closed = false;
    UIFileDialogMode mode = UIFileDialogMode::OpenFile;
    float row_height = 26.0f;

    UIFileDialog() {
        visible = false;
        receives_input = false;
        size = math::Vector2f(620.0f, 420.0f);
        min_size = size;
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Selection;
    }
    const char* type() const override { return "UIFileDialog"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIModal::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIFileDialog)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIModal::serialize(out);
        out["title"] = title;
        out["current_path"] = current_path;
        out["entries"] = entries;
        out["mode"] = static_cast<int>(mode);
    }
    void deserialize(const nlohmann::json& in) override {
        UIModal::deserialize(in);
        title = in.value("title", title);
        current_path = in.value("current_path", current_path);
        entries = in.value("entries", entries);
        mode = static_cast<UIFileDialogMode>(
            in.value("mode", static_cast<int>(mode)));
        receives_input = visible;
    }
    void set_open(bool value) {
        visible = value;
        receives_input = value;
        if (value) {
            closed = false;
            result = 0;
        }
    }
    std::string selected_entry() const {
        const auto values = ui_split_lines(entries);
        if (selected < 0 || selected >= static_cast<int>(values.size())) return {};
        return values[static_cast<std::size_t>(selected)];
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (!visible) return false;
        const auto values = ui_split_lines(entries);
        if (event.type == ui::UIEventType::Wheel) {
            return true;
        }
        if (event.type == ui::UIEventType::Click ||
            event.type == ui::UIEventType::PointerDown) {
            const float list_top = event.layout_rect.y + 72.0f;
            const float list_bottom = event.layout_rect.bottom() - 54.0f;
            if (event.position.y >= list_top && event.position.y < list_bottom) {
                const int index = static_cast<int>(
                    (event.position.y - list_top) / std::max(1.0f, row_height));
                if (index >= 0 && index < static_cast<int>(values.size())) {
                    selected = index;
                }
                return true;
            }
            if (event.position.y >= list_bottom) {
                const float middle = event.layout_rect.x + event.layout_rect.w * 0.5f;
                result = event.position.x >= middle ? 1 : -1;
                closed = true;
                set_open(false);
                return true;
            }
        }
        if (event.type == ui::UIEventType::KeyDown) {
            if (event.key == static_cast<int>(ui::UIKey::Down)) {
                selected = std::min(static_cast<int>(values.size()) - 1, selected + 1);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Up)) {
                selected = std::max(0, selected - 1);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Enter)) {
                result = 1;
            } else if (event.key == static_cast<int>(ui::UIKey::Escape)) {
                result = -1;
            } else {
                return false;
            }
            closed = true;
            set_open(false);
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (!visible) return;
        UIPanel::add_draw_commands(rect, draw_list);
        draw_list.add_text({rect.x + 16.0f, rect.y + 10.0f,
                            rect.w - 32.0f, 28.0f},
                           title, 18.0f,
                           themed_color(foreground_role, 0xFFFFFFFFu));
        draw_list.add_rect({rect.x + 16.0f, rect.y + 44.0f,
                            rect.w - 32.0f, 24.0f},
                           ui::ui_theme_color(ui::UIThemeColor::Input));
        draw_list.add_text({rect.x + 22.0f, rect.y + 44.0f,
                            rect.w - 44.0f, 24.0f},
                           current_path, 13.0f,
                           ui::ui_theme_color(ui::UIThemeColor::TextMuted));
        const auto values = ui_split_lines(entries);
        const float list_top = rect.y + 72.0f;
        const float list_bottom = rect.bottom() - 54.0f;
        draw_list.push_clip({rect.x + 16.0f, list_top,
                             rect.w - 32.0f, list_bottom - list_top});
        for (int i = 0; i < static_cast<int>(values.size()); ++i) {
            const ui::UIRect row{rect.x + 16.0f,
                                 list_top + row_height * static_cast<float>(i),
                                 rect.w - 32.0f, row_height};
            if (row.y >= list_bottom) break;
            if (i == selected) {
                ui::ui_paint_row_selection(draw_list, row,
                                           ui::ui_theme_color(accent_role));
            }
            draw_list.add_text({row.x + 8.0f, row.y, row.w - 16.0f, row.h},
                               values[static_cast<std::size_t>(i)],
                               14.0f,
                               themed_color(foreground_role, 0xFFFFFFFFu));
        }
        draw_list.pop_clip();
        const float button_radius = ui::ui_theme_radius(ui::UIRadiusTier::Small);
        const float button_y = rect.bottom() - 44.0f;
        const ui::UIRect cancel{rect.x + 16.0f, button_y, 104.0f, 32.0f};
        const ui::UIRect accept{rect.right() - 120.0f, button_y, 104.0f, 32.0f};
        draw_list.add_round_rect(cancel, button_radius,
                                 ui::ui_control_fill(hover_amount(), 0.0f),
                                 0u, 1.0f,
                                 ui::ui_control_stroke(hover_amount(), 0.0f));
        draw_list.add_round_rect(accept, button_radius,
                                 ui::ui_theme_color(ui::UIThemeColor::Accent),
                                 0u, 1.0f, ui::ui_with_alpha(0xFFFFFFFFu, 0.10f));
        draw_list.add_text(cancel, "Cancel", 14.0f,
                           ui::ui_theme_color(ui::UIThemeColor::Text),
                           ui::UITextAlign::Center);
        draw_list.add_text(accept,
                           mode == UIFileDialogMode::PickFolder ? "Select" :
                           mode == UIFileDialogMode::SaveFile ? "Save" : "Open",
                           14.0f, 0xFFFFFFFFu, ui::UITextAlign::Center);
    }
};

class UIFolderPicker : public UIFileDialog {
public:
    UIFolderPicker() {
        title = "Select Folder";
        mode = UIFileDialogMode::PickFolder;
    }
    const char* type() const override { return "UIFolderPicker"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIFileDialog::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIFolderPicker)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
};

enum class UIToastSeverity : uint8_t {
    Info = 0,
    Success,
    Warning,
    Error,
};

class UIToast : public UIPopup {
public:
    std::string text = "Notification";
    float duration = 3.0f;
    float age = 0.0f;
    bool closed = false;
    UIToastSeverity severity = UIToastSeverity::Info;

    UIToast() {
        visible = false;
        receives_input = false;
        z_index = 18000;
        size = math::Vector2f(320.0f, 54.0f);
        min_size = math::Vector2f(180.0f, 40.0f);
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UIToast"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIPopup::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIToast)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIPopup::serialize(out);
        out["text"] = text;
        out["duration"] = duration;
        out["severity"] = static_cast<int>(severity);
    }
    void deserialize(const nlohmann::json& in) override {
        UIPopup::deserialize(in);
        text = in.value("text", text);
        duration = in.value("duration", duration);
        severity = static_cast<UIToastSeverity>(
            in.value("severity", static_cast<int>(severity)));
    }
    void show(std::string message, float seconds = 3.0f,
              UIToastSeverity level = UIToastSeverity::Info) {
        text = std::move(message);
        duration = std::max(0.1f, seconds);
        age = 0.0f;
        severity = level;
        closed = false;
        visible = true;
        receives_input = true;
    }
    void dismiss() {
        visible = false;
        receives_input = false;
        closed = true;
    }
    void update(float dt) {
        if (!visible) return;
        age += std::max(0.0f, dt);
        if (age >= duration) dismiss();
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (!visible) return false;
        if (event.type == ui::UIEventType::Click) {
            dismiss();
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (!visible) return;
        uint32_t accent = ui::ui_theme_color(ui::UIThemeColor::Accent);
        if (severity == UIToastSeverity::Success) {
            accent = ui::ui_theme_color(ui::UIThemeColor::Success);
        }
        if (severity == UIToastSeverity::Warning) {
            accent = ui::ui_theme_color(ui::UIThemeColor::Warning);
        }
        if (severity == UIToastSeverity::Error) {
            accent = ui::ui_theme_color(ui::UIThemeColor::Error);
        }
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier), themed_color(background_role, 0x151C28F2u));
        draw_list.add_rect({rect.x, rect.y, 4.0f, rect.h}, accent);
        draw_list.add_text({rect.x + 14.0f, rect.y + 8.0f,
                            rect.w - 28.0f, rect.h - 16.0f},
                           text, 14.0f,
                           themed_color(foreground_role, 0xFFFFFFFFu));
    }
};

class UINotificationCenter : public NodeUI {
public:
    std::string messages =
        "Info: GryceIDE ready\nWarning: project not saved";
    int unread = 2;
    int max_lines = 8;
    math::Vector4f background = math::Vector4f(0.06f, 0.07f, 0.095f, 1.0f);

    UINotificationCenter() {
        size = math::Vector2f(360.0f, 220.0f);
        min_size = math::Vector2f(180.0f, 80.0f);
        clip_contents = true;
        // 内容视图与所在卡片同层：比页面底再深一档会让列表看起来是凹陷的。
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UINotificationCenter"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UINotificationCenter)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["messages"] = messages;
        out["unread"] = unread;
        out["max_lines"] = max_lines;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        messages = in.value("messages", messages);
        unread = in.value("unread", unread);
        max_lines = in.value("max_lines", max_lines);
    }
    void push(const std::string& message) {
        if (messages.empty()) messages = message;
        else messages += "\n" + message;
        ++unread;
        trim();
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::Click ||
            event.type == ui::UIEventType::PointerDown) {
            unread = 0;
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        draw_list.add_round_rect(rect, ui::ui_resolve_radius(corner_radius, radius_tier), themed_color(
            background_role, ui::ui_color(background.x, background.y,
                                          background.z, background.w)));
        const auto lines = ui_split_lines(messages);
        for (int i = 0; i < std::min(static_cast<int>(lines.size()), max_lines); ++i) {
            draw_list.add_text({rect.x + 8.0f, rect.y + 24.0f * static_cast<float>(i),
                                rect.w - 16.0f, 24.0f},
                               lines[static_cast<std::size_t>(i)],
                               13.0f,
                               themed_color(foreground_role, 0xE1E7F0FFu));
        }
    }

private:
    void trim() {
        auto lines = ui_split_lines(messages);
        if (static_cast<int>(lines.size()) > max_lines) {
            lines.erase(lines.begin(), lines.end() -
                        static_cast<std::ptrdiff_t>(max_lines));
        }
        std::ostringstream stream;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i) stream << '\n';
            stream << lines[i];
        }
        messages = stream.str();
    }
};

// ---------------------------------------------------------------------------
// IDE document controls
// ---------------------------------------------------------------------------

class UICodeEditor : public UIMultilineTextInput {
public:
    std::string file_path;
    std::string language = "javascript";
    bool show_line_numbers = true;
    bool highlight = true;
    bool highlight_current_line = true;
    bool dirty = false;
    float gutter_width = 46.0f;
    int current_line = 1;
    std::string find_query;

    struct HistoryEntry {
        std::string text;
        std::size_t cursor = 0;
    };
    std::vector<HistoryEntry> undo_stack_;
    std::vector<HistoryEntry> redo_stack_;
    std::size_t history_limit_ = 512;

    UICodeEditor() {
        size = math::Vector2f(720.0f, 420.0f);
        min_size = math::Vector2f(240.0f, 120.0f);
        max_length = 4 * 1024 * 1024;
        background_role = ui::UIThemeColor::Input;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Accent;
    }
    const char* type() const override { return "UICodeEditor"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIMultilineTextInput::collect_type_views(out);
        out.push_back({std::type_index(typeid(UICodeEditor)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIMultilineTextInput::serialize(out);
        out["file_path"] = file_path;
        out["language"] = language;
        out["show_line_numbers"] = show_line_numbers;
        out["highlight"] = highlight;
        out["find_query"] = find_query;
        out["gutter_width"] = gutter_width;
    }
    void deserialize(const nlohmann::json& in) override {
        UIMultilineTextInput::deserialize(in);
        file_path = in.value("file_path", file_path);
        language = in.value("language", language);
        show_line_numbers = in.value("show_line_numbers", true);
        highlight = in.value("highlight", true);
        find_query = in.value("find_query", find_query);
        gutter_width = in.value("gutter_width", gutter_width);
        dirty = false;
        update_current_line();
    }
    void set_code(std::string value, bool mark_dirty = false) {
        text = std::move(value);
        cursor = text.size();
        selection_start = cursor;
        dirty = mark_dirty;
        update_current_line();
    }
    bool load_file(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::ostringstream stream;
        stream << in.rdbuf();
        file_path = path;
        set_code(stream.str(), false);
        return true;
    }
    bool save_file(const std::string& path = {}) {
        const std::string target = path.empty() ? file_path : path;
        if (target.empty()) return false;
        std::error_code error;
        const std::filesystem::path file_path_obj(target);
        if (!file_path_obj.parent_path().empty()) {
            std::filesystem::create_directories(file_path_obj.parent_path(), error);
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return false;
        file_path = target;
        dirty = false;
        return true;
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        // 撤销 / 重做：Ctrl+Z、Ctrl+Y、Ctrl+Shift+Z。
        // 键值是 GLFW 码（Z=90, Y=89）；修饰位按本引擎 UI 约定：
        // Ctrl = bit0 (0x1)、Shift = bit1 (0x2)（见 UITextInput::on_ui_event）。
        if (event.type == ui::UIEventType::KeyDown && (event.modifiers & 0x0001) != 0) {
            if (event.key == 90) {  // Z
                if ((event.modifiers & 0x0002) != 0) redo(); else undo();
                return true;
            }
            if (event.key == 89) {  // Y
                redo();
                return true;
            }
        }
        const std::string before = text;
        const std::size_t cursor_before = cursor;
        const bool handled = UIMultilineTextInput::on_ui_event(event);
        if (before != text) {
            push_undo(before, cursor_before);
            dirty = true;
        }
        update_current_line();
        return handled;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        const float surface_radius = ui::ui_resolve_radius(corner_radius, radius_tier);
        draw_list.add_round_rect(rect, surface_radius, themed_color(background_role,
            ui::ui_color(background.x, background.y, background.z, background.w)),
            0u, 1.0f,
            ui::ui_with_alpha(ui::ui_theme_color(ui::UIThemeColor::Border), 0.6f));

        const float gutter = show_line_numbers ? gutter_width : 0.0f;
        const float text_x = rect.x + gutter + 6.0f;
        const float text_w = std::max(1.0f, rect.w - gutter - 12.0f);
        const float lh = line_height;
        const auto lines = ui_split_lines(text);
        const int line_count = std::max(1, static_cast<int>(lines.size()));
        const float char_w = font_size * 0.58f;

        // 当前行背景
        if (highlight_current_line) {
            const float cy = rect.y + 4.0f + lh * static_cast<float>(current_line - 1);
            if (cy + lh > rect.y && cy < rect.bottom()) {
                draw_list.add_round_rect(
                    {rect.x + gutter + 2.0f, cy, std::max(1.0f, rect.w - gutter - 4.0f), lh},
                    2.0f,
                    ui::ui_color(1.0f, 1.0f, 1.0f, 0.035f));
            }
        }

        // 查找匹配高亮（等宽近似定位，与光标同口径）
        if (!find_query.empty()) {
            const float match_w = char_w * static_cast<float>(find_query.size());
            for (int li = 0; li < line_count; ++li) {
                const std::string& line = lines[static_cast<std::size_t>(li)];
                std::size_t pos = line.find(find_query);
                if (pos == std::string::npos) continue;
                const float cy = rect.y + 4.0f + lh * static_cast<float>(li);
                if (cy + lh <= rect.y || cy >= rect.bottom()) continue;
                while (pos != std::string::npos) {
                    draw_list.add_round_rect(
                        {text_x + char_w * static_cast<float>(pos), cy + 1.0f,
                         match_w, std::max(1.0f, lh - 2.0f)},
                        2.0f,
                        ui::ui_color(0.96f, 0.78f, 0.18f, 0.32f));
                    pos = line.find(find_query,
                                    pos + std::max<std::size_t>(1, find_query.size()));
                }
            }
        }

        // 行号槽
        if (show_line_numbers) {
            // 行号槽只圆左侧两角，右侧贴着正文区保持直角。
            const float gutter_radii[4] = {surface_radius, 0.0f, 0.0f, surface_radius};
            draw_list.add_round_rect({rect.x, rect.y, gutter, rect.h}, gutter_radii,
                ui::ui_theme_color(ui::UIThemeColor::Window));
            for (int i = 0; i < line_count; ++i) {
                const float y = rect.y + 4.0f + lh * static_cast<float>(i);
                if (y >= rect.bottom()) break;
                draw_list.add_text({rect.x + 4.0f, y, gutter - 8.0f, lh},
                    std::to_string(i + 1), 12.0f,
                    (i + 1 == current_line)
                        ? ui::ui_theme_color(ui::UIThemeColor::Accent)
                        : ui::ui_theme_color(ui::UIThemeColor::TextMuted));
            }
        }

        // 正文（语法高亮：每行一组连续彩色 run）
        for (int i = 0; i < line_count; ++i) {
            const float y = rect.y + 4.0f + lh * static_cast<float>(i);
            if (y >= rect.bottom()) break;
            if (y + lh <= rect.y) continue;
            const std::string& line = lines[static_cast<std::size_t>(i)];
            if (line.empty()) continue;
            std::vector<ui::UITextRun> runs;
            if (highlight) {
                const auto tokens = ui::tokenize_code_line(line, language);
                runs.reserve(tokens.size());
                for (const ui::CodeToken& tk : tokens) {
                    ui::UITextRun run;
                    run.text = line.substr(tk.start, tk.length);
                    run.color = syntax_color(tk.role);
                    runs.push_back(std::move(run));
                }
            } else {
                ui::UITextRun run;
                run.text = line;
                run.color = themed_color(foreground_role,
                    ui::ui_color(text_color.x, text_color.y, text_color.z, text_color.w));
                runs.push_back(std::move(run));
            }
            draw_list.add_text_colored({text_x, y, text_w, lh}, std::move(runs), font_size);
        }

        // 光标
        if (focused) {
            const int li = std::clamp(current_line - 1, 0, line_count - 1);
            std::size_t line_begin = 0;
            for (int k = 0; k < li && k < line_count; ++k) {
                line_begin += lines[static_cast<std::size_t>(k)].size() + 1;
            }
            const std::size_t col = cursor >= line_begin ? cursor - line_begin : 0;
            const float cy = rect.y + 4.0f + lh * static_cast<float>(li);
            if (cy < rect.bottom()) {
                draw_list.add_rect(
                    {text_x + char_w * static_cast<float>(col), cy + 2.0f, 1.5f,
                     std::max(1.0f, lh - 4.0f)},
                    themed_color(accent_role,
                        ui::ui_color(cursor_color.x, cursor_color.y, cursor_color.z, cursor_color.w)));
            }
        }
    }

    // ---- 撤销 / 重做 / 查找 ----
    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }
    int undo_depth() const { return static_cast<int>(undo_stack_.size()); }
    int redo_depth() const { return static_cast<int>(redo_stack_.size()); }
    void set_find_query(std::string query) { find_query = std::move(query); }
    // 宿主侧设置焦点 / 光标（focused / cursor 在基类里是 protected）。
    void set_editor_focus(bool value) {
        focused = value;
        if (value) {
            cursor = std::min(cursor, text.size());
            selection_start = cursor;
        }
    }
    void set_editor_cursor(std::size_t pos) {
        cursor = std::min(pos, text.size());
        selection_start = cursor;
        update_current_line();
    }
    int match_count() const {
        if (find_query.empty()) return 0;
        int count = 0;
        std::size_t pos = 0;
        while ((pos = text.find(find_query, pos)) != std::string::npos) {
            ++count;
            pos += std::max<std::size_t>(1, find_query.size());
        }
        return count;
    }
    void push_undo(const std::string& prev_text, std::size_t prev_cursor) {
        undo_stack_.push_back({prev_text, prev_cursor});
        if (undo_stack_.size() > history_limit_) {
            undo_stack_.erase(undo_stack_.begin());
        }
        redo_stack_.clear();
    }
    void undo() {
        if (undo_stack_.empty()) return;
        redo_stack_.push_back({text, cursor});
        const HistoryEntry entry = undo_stack_.back();
        undo_stack_.pop_back();
        text = entry.text;
        cursor = std::min(entry.cursor, text.size());
        selection_start = cursor;
        update_current_line();
        dirty = true;
    }
    void redo() {
        if (redo_stack_.empty()) return;
        undo_stack_.push_back({text, cursor});
        const HistoryEntry entry = redo_stack_.back();
        redo_stack_.pop_back();
        text = entry.text;
        cursor = std::min(entry.cursor, text.size());
        selection_start = cursor;
        update_current_line();
        dirty = true;
    }

private:
    // 背景是浅色还是深色：语法配色是成套的，深底用高亮饱和色、浅底必须换成
    // 深色变体。同一套亮色画在白底上对比度不足，笔画看着就是"发虚"。
    bool light_background() const {
        const uint32_t bg = ui::ui_theme_color(ui::UIThemeColor::Window);
        const float r = static_cast<float>((bg >> 24) & 0xffu);
        const float g = static_cast<float>((bg >> 16) & 0xffu);
        const float b = static_cast<float>((bg >> 8) & 0xffu);
        return (0.2126f * r + 0.7152f * g + 0.0722f * b) > 128.0f;
    }

    uint32_t syntax_color(ui::CodeTokenRole role) const {
        const bool light = light_background();
        switch (role) {
            case ui::CodeTokenRole::Keyword:
                return light ? ui::ui_color(0.52f, 0.14f, 0.66f)
                             : ui::ui_color(0.78f, 0.56f, 0.96f);
            case ui::CodeTokenRole::Type:
                return light ? ui::ui_color(0.08f, 0.42f, 0.60f)
                             : ui::ui_color(0.45f, 0.80f, 0.96f);
            case ui::CodeTokenRole::String:
                return light ? ui::ui_color(0.10f, 0.45f, 0.18f)
                             : ui::ui_color(0.56f, 0.86f, 0.58f);
            case ui::CodeTokenRole::Comment:
                return light ? ui::ui_color(0.40f, 0.45f, 0.40f)
                             : ui::ui_color(0.52f, 0.60f, 0.54f);
            case ui::CodeTokenRole::Number:
                return light ? ui::ui_color(0.68f, 0.40f, 0.02f)
                             : ui::ui_color(0.96f, 0.76f, 0.46f);
            case ui::CodeTokenRole::Function:
                return light ? ui::ui_color(0.52f, 0.34f, 0.04f)
                             : ui::ui_color(0.96f, 0.86f, 0.56f);
            case ui::CodeTokenRole::Operator:
                return light ? ui::ui_color(0.42f, 0.26f, 0.28f)
                             : ui::ui_color(0.86f, 0.70f, 0.70f);
            case ui::CodeTokenRole::Punctuation:
                return light ? ui::ui_color(0.26f, 0.30f, 0.36f)
                             : ui::ui_color(0.76f, 0.79f, 0.84f);
            case ui::CodeTokenRole::Preprocessor:
                return light ? ui::ui_color(0.58f, 0.16f, 0.32f)
                             : ui::ui_color(0.90f, 0.60f, 0.72f);
            case ui::CodeTokenRole::Constant:
                return light ? ui::ui_color(0.68f, 0.26f, 0.06f)
                             : ui::ui_color(0.96f, 0.62f, 0.46f);
            case ui::CodeTokenRole::Plain:
            default:                              return themed_color(foreground_role,
                        ui::ui_color(text_color.x, text_color.y, text_color.z, text_color.w));
        }
    }

    void update_current_line() {
        current_line = 1;
        const std::size_t end = std::min(cursor, text.size());
        for (std::size_t i = 0; i < end; ++i) {
            if (text[i] == '\n') ++current_line;
        }
    }
};

class UIConsoleView : public UIListView {
public:
    std::string filter;
    int severity_filter = -1;
    int max_lines = 2000;
    bool auto_scroll = true;

    UIConsoleView() {
        size = math::Vector2f(720.0f, 180.0f);
        min_size = math::Vector2f(220.0f, 70.0f);
        row_height = 20.0f;
        // 内容视图与所在卡片同层：比页面底再深一档会让列表看起来是凹陷的。
        background_role = ui::UIThemeColor::PanelRaised;
        foreground_role = ui::UIThemeColor::Text;
    }
    const char* type() const override { return "UIConsoleView"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        UIListView::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIConsoleView)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        UIListView::serialize(out);
        out["filter"] = filter;
        out["severity_filter"] = severity_filter;
        out["max_lines"] = max_lines;
        out["auto_scroll"] = auto_scroll;
    }
    void deserialize(const nlohmann::json& in) override {
        UIListView::deserialize(in);
        filter = in.value("filter", filter);
        severity_filter = in.value("severity_filter", severity_filter);
        max_lines = std::max(1, in.value("max_lines", max_lines));
        auto_scroll = in.value("auto_scroll", auto_scroll);
    }
    void append(std::string message) {
        auto lines = ui_split_lines(items);
        lines.push_back(std::move(message));
        if (static_cast<int>(lines.size()) > max_lines) {
            lines.erase(lines.begin(), lines.end() -
                        static_cast<std::ptrdiff_t>(max_lines));
        }
        std::ostringstream out;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (i) out << '\n';
            out << lines[i];
        }
        items = out.str();
        if (auto_scroll) selected = static_cast<int>(lines.size()) - 1;
        activated = -1;
    }
    void clear() {
        items.clear();
        selected = -1;
        activated = -1;
    }
};

} // namespace gryce_engine::components
