#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

#include "components/node_ui.h"
#include "ui/ui_clipboard.h"
#include "ui/ui_drawlist.h"
#include "ui/ui_input.h"
#include "ui/ui_paint.h"
#include "ui/ui_texture.h"

namespace gryce_engine::components {

namespace ui_detail {

inline std::size_t previous_codepoint(const std::string& text, std::size_t cursor) {
    if (cursor == 0 || text.empty()) return 0;
    --cursor;
    while (cursor > 0 && (static_cast<unsigned char>(text[cursor]) & 0xC0u) == 0x80u) {
        --cursor;
    }
    return cursor;
}

inline std::size_t next_codepoint(const std::string& text, std::size_t cursor) {
    if (cursor >= text.size()) return text.size();
    ++cursor;
    while (cursor < text.size() &&
           (static_cast<unsigned char>(text[cursor]) & 0xC0u) == 0x80u) {
        ++cursor;
    }
    return cursor;
}

inline void append_utf8(std::string& text, uint32_t cp) {
    if (cp <= 0x7Fu) {
        text.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        text.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
        text.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        text.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
        text.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        text.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        text.push_back(static_cast<char>(0xF0u | (cp >> 18u)));
        text.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu)));
        text.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        text.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

inline std::string masked_text(const std::string& text) {
    std::string masked;
    for (std::size_t i = 0; i < text.size();) {
        masked.push_back('*');
        i = next_codepoint(text, i);
    }
    return masked;
}

} // namespace ui_detail

// GryceUI 基础控件。全部控件只依赖 NodeUI 布局、输入事件和 UIDrawList。
class UIPanel : public NodeUI {
public:
    math::Vector4f color = math::Vector4f(0.12f, 0.14f, 0.18f, 1.0f);
    // 纯布局容器（行/列分组）不该画底色：叠在卡片上会多出一整块表面，
    // 层次全被吃掉。布局容器显式关掉它。
    bool draws_background = true;

    UIPanel() {
        background_role = ui::UIThemeColor::Panel;
    }

    const char* type() const override { return "UIPanel"; }

    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIPanel)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }

    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["color"] = {color.x, color.y, color.z, color.w};
        out["draws_background"] = draws_background;
    }

    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        draws_background = in.value("draws_background", true);
        const auto c = in.value("color", std::vector<float>{0.12f, 0.14f, 0.18f, 1.0f});
        if (c.size() >= 4) color = math::Vector4f(c[0], c[1], c[2], c[3]);
    }

    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (!draws_background) return;
        const uint32_t fill = themed_color(
            background_role, ui::ui_color(color.x, color.y, color.z, color.w));
        ui::UISurface surface;
        surface.rect = rect;
        ui::ui_fill_radii(ui::ui_resolve_radius(corner_radius, radius_tier),
                          surface.radii);
        surface.fill = fill;
        surface.elevation = rest_elevation;
        // 抬起的表面（卡片、浮层）描一圈发丝边：Fluent 的层次主要靠"亮一档
        // 的底 + 1px 白边"表达，纯投影在深色主题上几乎看不见。
        if (rest_elevation > 0.0f || background_role == ui::UIThemeColor::PanelRaised) {
            surface.border = ui::ui_with_alpha(
                ui::ui_theme_color(ui::UIThemeColor::ControlStroke), 1.0f);
            surface.border_width = 1.0f;
        }
        ui::ui_paint_surface(draw_list, surface);
    }
};

class UILabel : public NodeUI {
public:
    std::string text;
    float font_size = 16.0f;
    math::Vector4f color = math::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);

    UILabel() {
        min_size = math::Vector2f(0.0f, 20.0f);
        foreground_role = ui::UIThemeColor::Text;
    }

    const char* type() const override { return "UILabel"; }

    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UILabel)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }

    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["text"] = text;
        out["font_size"] = font_size;
        out["color"] = {color.x, color.y, color.z, color.w};
    }

    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        text = in.value("text", "");
        font_size = in.value("font_size", 16.0f);
        const auto c = in.value("color", std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f});
        if (c.size() >= 4) color = math::Vector4f(c[0], c[1], c[2], c[3]);
    }

    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        if (!text.empty()) {
            draw_list.add_text(rect, text, font_size,
                               themed_color(foreground_role,
                                   ui::ui_color(color.x, color.y, color.z, color.w)));
        }
    }
};

class UIImage : public NodeUI {
public:
    // 声明式用法：只写 texture_path，绘制时经 UI 纹理服务取句柄（同一路径
    // 只解码/上传一次）。也可以直接塞 texture（例如运行时生成的渲染目标）。
    std::string texture_path;
    ui::TextureRef texture;
    ui::UIFitMode fit = ui::UIFitMode::Contain;
    // 贴图没就绪时用的占位色；关掉就完全不画背景。
    math::Vector4f placeholder_color = math::Vector4f(0.2f, 0.22f, 0.26f, 1.0f);
    bool show_placeholder = true;
    math::Vector4f tint = math::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);
    // 兼容旧字段：直接给 index/generation 等同于填 texture。
    uint32_t texture_index = 0xFFFFFFFFu;
    uint32_t texture_generation = 0;

    UIImage() {
        background_role = ui::UIThemeColor::PanelRaised;
        // 图片通常是卡片里的内容块，圆角跟卡片一档。不做节点裁剪：Cover/Native
        // 已经用 UV 把超出部分裁掉了，再套一层 clip 只会多一对 push/pop。
        radius_tier = ui::UIRadiusTier::Medium;
    }

    const char* type() const override { return "UIImage"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIImage)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["texture_path"] = texture_path;
        out["fit"] = static_cast<int>(fit);
        out["show_placeholder"] = show_placeholder;
        out["placeholder_color"] = {placeholder_color.x, placeholder_color.y,
                                    placeholder_color.z, placeholder_color.w};
        out["tint"] = {tint.x, tint.y, tint.z, tint.w};
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        texture_path = in.value("texture_path", "");
        fit = static_cast<ui::UIFitMode>(in.value("fit", 1));
        show_placeholder = in.value("show_placeholder", true);
        const auto p = in.value("placeholder_color",
                                std::vector<float>{0.2f, 0.22f, 0.26f, 1.0f});
        if (p.size() >= 4) placeholder_color = math::Vector4f(p[0], p[1], p[2], p[3]);
        const auto t = in.value("tint", std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f});
        if (t.size() >= 4) tint = math::Vector4f(t[0], t[1], t[2], t[3]);
    }

    // 当前实际可用的纹理：显式 texture 优先，其次按路径查服务。
    ui::TextureRef resolved_texture() const {
        if (texture.valid()) return texture;
        if (texture_index != 0xFFFFFFFFu) {
            ui::TextureRef ref;
            ref.index = texture_index;
            ref.generation = texture_generation;
            // 只给了句柄、没给尺寸时按 1:1 处理（fit 会退化成铺满盒子），
            // 不能因此判成"没有纹理"。
            ref.width = texture.width > 0 ? texture.width : 1;
            ref.height = texture.height > 0 ? texture.height : 1;
            return ref;
        }
        return ui::ui_texture_cached(texture_path);
    }

    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        const float radius = ui::ui_resolve_radius(corner_radius, radius_tier);
        const ui::TextureRef ref = resolved_texture();
        const uint32_t tint_color = ui::ui_color(tint.x, tint.y, tint.z, tint.w);

        if (!ref.valid()) {
            if (!show_placeholder) return;
            // 占位：控件填充底 + 一个对角折线，暗示"这里是一张图"。
            draw_list.add_round_rect(rect, radius, themed_color(
                background_role, ui::ui_color(placeholder_color.x, placeholder_color.y,
                                              placeholder_color.z, placeholder_color.w)));
            const uint32_t mark = ui::ui_with_alpha(
                ui::ui_theme_color(ui::UIThemeColor::TextMuted), 0.5f);
            draw_list.add_line(rect.x + rect.w * 0.28f, rect.y + rect.h * 0.30f,
                               rect.x + rect.w * 0.72f, rect.y + rect.h * 0.70f,
                               2.0f, mark);
            return;
        }

        const ui::UIFittedImage fitted = ui::ui_fit_image(
            rect, ref.width > 0 ? ref.width : 1,
            ref.height > 0 ? ref.height : 1, fit);
        // Contain/Native 会在盒子里留白；留白要不要铺底由宿主决定（多数场景
        // 需要的是贴图本身，多铺一层底会把透明贴图的留白染成实心）。
        draw_list.add_image(fitted.rect, ref.index, ref.generation,
                            fitted.u0, fitted.v0, fitted.u1, fitted.v1, tint_color);
    }
};

class UIViewportPanel : public NodeUI {
public:
    ui::TextureRef texture;
    math::Vector4f letterbox_color = math::Vector4f(0.025f, 0.03f, 0.04f, 1.0f);
    math::Vector4f tint = math::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);
    bool preserve_aspect = true;

    UIViewportPanel() {
        size = math::Vector2f(640.0f, 360.0f);
        min_size = math::Vector2f(160.0f, 90.0f);
        clip_contents = true;
        background_role = ui::UIThemeColor::Window;
    }

    const char* type() const override { return "UIViewportPanel"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIViewportPanel)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["letterbox_color"] = {letterbox_color.x, letterbox_color.y,
                                  letterbox_color.z, letterbox_color.w};
        out["tint"] = {tint.x, tint.y, tint.z, tint.w};
        out["preserve_aspect"] = preserve_aspect;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        preserve_aspect = in.value("preserve_aspect", true);
        auto read4 = [&](const char* key, math::Vector4f& color) {
            const auto v = in.value(key, std::vector<float>{
                color.x, color.y, color.z, color.w});
            if (v.size() >= 4) color = math::Vector4f(v[0], v[1], v[2], v[3]);
        };
        read4("letterbox_color", letterbox_color);
        read4("tint", tint);
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        draw_list.add_rect(rect, themed_color(
            background_role, ui::ui_color(letterbox_color.x, letterbox_color.y,
                                          letterbox_color.z, letterbox_color.w)));
        if (!texture.valid()) return;
        ui::UIRect target = rect;
        if (preserve_aspect && texture.width > 0 && texture.height > 0) {
            const float source_aspect =
                static_cast<float>(texture.width) / static_cast<float>(texture.height);
            const float target_aspect = rect.w / std::max(1.0f, rect.h);
            if (source_aspect > target_aspect) {
                target.h = rect.w / source_aspect;
                target.y += (rect.h - target.h) * 0.5f;
            } else {
                target.w = rect.h * source_aspect;
                target.x += (rect.w - target.w) * 0.5f;
            }
        }
        draw_list.add_image(target, texture.index, texture.generation,
                            0.0f, 0.0f, 1.0f, 1.0f,
                            ui::ui_color(tint.x, tint.y, tint.z, tint.w));
    }
};

// Fluent 的三种按钮变体。Standard 是"控件填充 + 发丝边"，Accent 是主操作，
// Subtle 平时透明、只有指针进来才浮出底色（工具栏、命令栏用）。
enum class UIButtonVariant : uint8_t {
    Standard = 0,
    Accent,
    Subtle,
};

class UIButton : public NodeUI {
public:
    std::string text = "Button";
    math::Vector4f background = math::Vector4f(0.18f, 0.32f, 0.56f, 1.0f);
    math::Vector4f hover_background = math::Vector4f(0.24f, 0.42f, 0.70f, 1.0f);
    math::Vector4f pressed_background = math::Vector4f(0.12f, 0.24f, 0.44f, 1.0f);
    math::Vector4f text_color = math::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);
    float font_size = 14.0f;
    bool clicked = false;
    UIButtonVariant variant = UIButtonVariant::Standard;
    // 只画底板不画文字：图标按钮自己排"图标 + 文字"，交给基类居中会重叠。
    bool show_text = true;

    UIButton() {
        size = math::Vector2f(120.0f, 32.0f);
        min_size = size;
        focusable = true;
        background_role = ui::UIThemeColor::PanelRaised;
        background_hover_role = ui::UIThemeColor::AccentHover;
        background_pressed_role = ui::UIThemeColor::AccentPressed;
        foreground_role = ui::UIThemeColor::Text;
        // Fluent 的按压反馈：缩到 98%，指针进入时抬起一点点。
        press_scale = 0.02f;
        hover_elevation = 3.0f;
        radius_tier = ui::UIRadiusTier::Small;
    }

    const char* type() const override { return "UIButton"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIButton)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["text"] = text;
        out["background"] = {background.x, background.y, background.z, background.w};
        out["hover_background"] = {hover_background.x, hover_background.y,
                                   hover_background.z, hover_background.w};
        out["pressed_background"] = {pressed_background.x, pressed_background.y,
                                     pressed_background.z, pressed_background.w};
        out["text_color"] = {text_color.x, text_color.y, text_color.z, text_color.w};
        out["font_size"] = font_size;
        out["variant"] = static_cast<int>(variant);
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        text = in.value("text", "Button");
        font_size = in.value("font_size", 14.0f);
        variant = static_cast<UIButtonVariant>(
            std::clamp(in.value("variant", 0), 0, 2));
        auto read4 = [&](const char* key, math::Vector4f& value) {
            const auto v = in.value(key, std::vector<float>{
                value.x, value.y, value.z, value.w});
            if (v.size() >= 4) value = math::Vector4f(v[0], v[1], v[2], v[3]);
        };
        read4("background", background);
        read4("hover_background", hover_background);
        read4("pressed_background", pressed_background);
        read4("text_color", text_color);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::KeyDown &&
            (event.key == static_cast<int>(ui::UIKey::Enter) || event.key == 32)) {
            clicked = true;
            return true;
        }
        switch (event.type) {
            case ui::UIEventType::PointerDown: return true;
            case ui::UIEventType::PointerUp: return true;
            case ui::UIEventType::Click: clicked = true; return true;
            default: return false;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect_in, ui::UIDrawList& draw_list) const override {
        const ui::UIRect rect = ui::ui_press_rect(rect_in, press_scale, press_amount());
        float radii[4];
        ui::ui_fill_radii(ui::ui_resolve_radius(corner_radius, radius_tier), radii);

        // 三种变体只差"底色从哪儿来"：Standard 用主题的三档控件填充，Accent 用
        // 强调色三档，Subtle 平时全透明。文字一律居中——左对齐会让按钮看起来
        // 像一块带文字的色块，而不是控件。
        uint32_t fill = 0u;
        uint32_t border = 0u;
        uint32_t label = themed_color(foreground_role,
                                      ui::ui_color(text_color.x, text_color.y,
                                                   text_color.z, text_color.w));
        switch (variant) {
            case UIButtonVariant::Accent: {
                fill = ui::ui_theme_mix(ui::ui_theme_color(ui::UIThemeColor::Accent),
                                        ui::ui_theme_color(ui::UIThemeColor::AccentHover),
                                        hover_amount());
                fill = ui::ui_theme_mix(fill,
                                        ui::ui_theme_color(ui::UIThemeColor::AccentPressed),
                                        press_amount());
                border = ui::ui_with_alpha(0xFFFFFFFFu, 0.10f + 0.06f * hover_amount());
                break;
            }
            case UIButtonVariant::Subtle: {
                const uint32_t hover_fill = ui::ui_theme_mix(
                    ui::ui_theme_color(ui::UIThemeColor::ControlFillPressed),
                    ui::ui_theme_color(ui::UIThemeColor::ControlFillHover),
                    hover_amount());
                fill = ui::ui_with_alpha(hover_fill,
                                         hover_amount() * (1.0f - 0.3f * press_amount()));
                border = ui::ui_with_alpha(ui::ui_theme_color(ui::UIThemeColor::ControlStroke),
                                           0.5f * hover_amount());
                break;
            }
            case UIButtonVariant::Standard:
            default: {
                fill = ui::ui_control_fill(hover_amount(), press_amount());
                border = ui::ui_control_stroke(hover_amount(), focus_amount());
                break;
            }
        }

        ui::UISurface surface;
        surface.rect = rect;
        for (int i = 0; i < 4; ++i) surface.radii[i] = radii[i];
        surface.fill = fill;
        surface.border = border;
        surface.border_width = 1.0f;
        surface.elevation = rest_elevation + hover_elevation * hover_amount();
        ui::ui_paint_surface(draw_list, surface);

        // Fluent 的按下会把文字降一档到次要色，按压感不只是背景变暗。
        if (variant != UIButtonVariant::Accent && press_amount() > 0.0f) {
            label = ui::ui_theme_mix(label, ui::ui_theme_color(ui::UIThemeColor::TextMuted),
                                     press_amount() * 0.55f);
        }
        if (show_text && !text.empty()) {
            draw_list.add_text({rect.x + 12.0f, rect.y, rect.w - 24.0f, rect.h},
                               text, font_size, label, ui::UITextAlign::Center);
        }
        ui::ui_paint_focus_ring(draw_list, rect, radii[0], focus_amount());
    }

};

class UICheckBox : public NodeUI {
public:
    std::string text = "CheckBox";
    bool checked = false;
    math::Vector4f text_color = math::Vector4f(1.0f, 1.0f, 1.0f, 1.0f);
    math::Vector4f box_color = math::Vector4f(0.22f, 0.24f, 0.30f, 1.0f);
    math::Vector4f check_color = math::Vector4f(0.35f, 0.78f, 0.55f, 1.0f);
    float font_size = 15.0f;

    UICheckBox() {
        size = math::Vector2f(180.0f, 32.0f);
        min_size = size;
        focusable = true;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Accent;
        radius_tier = ui::UIRadiusTier::Small;
    }
    const char* type() const override { return "UICheckBox"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UICheckBox)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["text"] = text;
        out["checked"] = checked;
        out["text_color"] = {text_color.x, text_color.y, text_color.z, text_color.w};
        out["box_color"] = {box_color.x, box_color.y, box_color.z, box_color.w};
        out["check_color"] = {check_color.x, check_color.y, check_color.z, check_color.w};
        out["font_size"] = font_size;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        text = in.value("text", "CheckBox");
        checked = in.value("checked", false);
        font_size = in.value("font_size", 14.0f);
        auto read4 = [&](const char* key, math::Vector4f& value) {
            const auto v = in.value(key, std::vector<float>{
                value.x, value.y, value.z, value.w});
            if (v.size() >= 4) value = math::Vector4f(v[0], v[1], v[2], v[3]);
        };
        read4("text_color", text_color);
        read4("box_color", box_color);
        read4("check_color", check_color);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::KeyDown &&
            (event.key == static_cast<int>(ui::UIKey::Enter) || event.key == 32)) {
            checked = !checked;
            return true;
        }
        if (event.type == ui::UIEventType::Click) {
            checked = !checked;
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        // Fluent 的复选框是 20x20 的方框 + 8px 间距 + 14px 文字。选中时整块
        // 填强调色并画一笔白色勾；未选中是控件填充 + 发丝边。中间那个内缩方块
        // 是"不确定态"的语义，不能拿来表示"已选中"。
        const float box = std::min(20.0f, rect.h);
        const ui::UIRect box_rect{rect.x, rect.y + (rect.h - box) * 0.5f, box, box};
        const float radius = ui::ui_resolve_radius(corner_radius, radius_tier);
        const uint32_t accent = themed_color(accent_role,
                                             ui::ui_color(check_color.x, check_color.y,
                                                          check_color.z, check_color.w));
        const uint32_t idle = themed_color(UIThemeColor::Input,
                                           ui::ui_color(box_color.x, box_color.y,
                                                        box_color.z, box_color.w));
        const uint32_t fill = checked
            ? ui::ui_theme_mix(accent,
                               ui::ui_theme_color(ui::UIThemeColor::AccentHover),
                               hover_amount())
            : ui::ui_theme_mix(idle, ui::ui_control_fill(hover_amount(), 0.0f),
                               hover_amount());
        const uint32_t stroke = checked
            ? ui::ui_with_alpha(0xFFFFFFFFu, 0.10f)
            : ui::ui_control_stroke(hover_amount(), focus_amount());
        draw_list.add_round_rect(box_rect, radius, fill, 0u, 1.0f, stroke);
        if (checked) {
            ui::ui_paint_check(draw_list, box_rect,
                               ui::ui_with_alpha(0xFFFFFFFFu, 1.0f), 2.0f);
        }
        if (!text.empty()) {
            draw_list.add_text({rect.x + box + 8.0f, rect.y, rect.w - box - 8.0f, rect.h},
                               text, font_size,
                               themed_color(foreground_role,
                                   ui::ui_color(text_color.x, text_color.y,
                                                text_color.z, text_color.w)));
        }
        ui::ui_paint_focus_ring(draw_list, box_rect, radius, focus_amount());
    }
};

class UISlider : public NodeUI {
public:
    float value = 0.0f;
    float minimum = 0.0f;
    float maximum = 1.0f;
    math::Vector4f track_color = math::Vector4f(0.18f, 0.20f, 0.24f, 1.0f);
    math::Vector4f fill_color = math::Vector4f(0.25f, 0.55f, 0.90f, 1.0f);

    UISlider() {
        size = math::Vector2f(200.0f, 24.0f);
        min_size = size;
        focusable = true;
        accent_role = ui::UIThemeColor::Accent;
        radius_tier = ui::UIRadiusTier::Small;
    }
    const char* type() const override { return "UISlider"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UISlider)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["value"] = value;
        out["minimum"] = minimum;
        out["maximum"] = maximum;
        out["track_color"] = {track_color.x, track_color.y, track_color.z, track_color.w};
        out["fill_color"] = {fill_color.x, fill_color.y, fill_color.z, fill_color.w};
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        value = in.value("value", 0.0f);
        minimum = in.value("minimum", 0.0f);
        maximum = in.value("maximum", 1.0f);
        auto read4 = [&](const char* key, math::Vector4f& color) {
            const auto v = in.value(key, std::vector<float>{
                color.x, color.y, color.z, color.w});
            if (v.size() >= 4) color = math::Vector4f(v[0], v[1], v[2], v[3]);
        };
        read4("track_color", track_color);
        read4("fill_color", fill_color);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::KeyDown) {
            const float step = std::max(0.001f, (maximum - minimum) * 0.02f);
            if (event.key == static_cast<int>(ui::UIKey::Left) ||
                event.key == static_cast<int>(ui::UIKey::Down)) {
                value = std::clamp(value - step, minimum, maximum);
                return true;
            }
            if (event.key == static_cast<int>(ui::UIKey::Right) ||
                event.key == static_cast<int>(ui::UIKey::Up)) {
                value = std::clamp(value + step, minimum, maximum);
                return true;
            }
        }
        if (event.type != ui::UIEventType::PointerDown &&
            event.type != ui::UIEventType::PointerMove) {
            return false;
        }
        const float width = std::max(1.0f, event.layout_rect.w);
        const float t = std::clamp((event.position.x - event.layout_rect.x) / width,
                                   0.0f, 1.0f);
        value = minimum + (maximum - minimum) * t;
        return true;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        // Fluent 的滑块：4px 胶囊轨道 + 20px 圆头。指针进入时圆头放大到 1.2 倍
        // 并在外缘透出一圈更深的强调色，提示"可以拖"。
        const float thickness = std::min(4.0f, rect.h);
        const ui::UIRect track{rect.x, rect.y + (rect.h - thickness) * 0.5f,
                               rect.w, thickness};
        const uint32_t accent = themed_color(accent_role,
            ui::ui_color(fill_color.x, fill_color.y, fill_color.z, fill_color.w));
        draw_list.add_round_rect(track, ui::ui_pill_radius(thickness),
                                 ui::ui_with_alpha(
                                     ui::ui_theme_color(UIThemeColor::Track), 0.60f));
        const float range = maximum - minimum;
        const float t = range == 0.0f ? 0.0f : std::clamp((value - minimum) / range,
                                                          0.0f, 1.0f);
        if (track.w * t > 0.5f) {
            draw_list.add_round_rect({track.x, track.y, track.w * t, track.h},
                                     ui::ui_pill_radius(thickness), accent);
        }
        const float handle = std::min(20.0f, rect.h - 2.0f) *
                             (1.0f + 0.2f * hover_amount());
        const float handle_radius = handle * 0.5f;
        const float cx = rect.x + rect.w * t;
        const float cy = rect.y + rect.h * 0.5f;
        const ui::UIRect handle_rect{cx - handle_radius, cy - handle_radius,
                                     handle, handle};
        draw_list.add_round_rect(handle_rect, handle_radius, accent);
        // Fluent 的圆头有 1px 外环（深色主题上是浅一环，浅色上是深一环）。
        draw_list.add_round_rect(handle_rect, handle_radius, 0u, 0u, 1.0f,
                                 ui::ui_with_alpha(
                                     ui::ui_theme_color(UIThemeColor::PanelRaised), 0.5f));
        ui::ui_paint_focus_ring(
            draw_list, rect, ui::ui_resolve_radius(corner_radius, radius_tier),
            focus_amount());
    }
};

class UITextInput : public NodeUI {
public:
    std::string text;
    float font_size = 14.0f;
    bool password = false;
    int max_length = 256;
    math::Vector4f background = math::Vector4f(0.10f, 0.12f, 0.15f, 1.0f);
    math::Vector4f text_color = math::Vector4f(0.94f, 0.95f, 0.97f, 1.0f);
    math::Vector4f cursor_color = math::Vector4f(0.35f, 0.72f, 1.0f, 1.0f);

    UITextInput() {
        size = math::Vector2f(220.0f, 32.0f);
        min_size = size;
        focusable = true;
        background_role = ui::UIThemeColor::Input;
        foreground_role = ui::UIThemeColor::Text;
        accent_role = ui::UIThemeColor::Accent;
        radius_tier = ui::UIRadiusTier::Small;
    }
    const char* type() const override { return "UITextInput"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UITextInput)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["text"] = text;
        out["font_size"] = font_size;
        out["password"] = password;
        out["max_length"] = max_length;
        out["background"] = {background.x, background.y, background.z, background.w};
        out["text_color"] = {text_color.x, text_color.y, text_color.z, text_color.w};
        out["cursor_color"] = {cursor_color.x, cursor_color.y, cursor_color.z, cursor_color.w};
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        text = in.value("text", "");
        font_size = in.value("font_size", 14.0f);
        password = in.value("password", false);
        max_length = in.value("max_length", 256);
        auto read4 = [&](const char* key, math::Vector4f& color) {
            const auto v = in.value(key, std::vector<float>{
                color.x, color.y, color.z, color.w});
            if (v.size() >= 4) color = math::Vector4f(v[0], v[1], v[2], v[3]);
        };
        read4("background", background);
        read4("text_color", text_color);
        read4("cursor_color", cursor_color);
        cursor = std::min(cursor, text.size());
        selection_start = cursor;
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::FocusGained) {
            focused = true;
            cursor = text.size();
            selection_start = cursor;
            return false;
        }
        if (event.type == ui::UIEventType::FocusLost) {
            focused = false;
            preedit.clear();
            return false;
        }
        if (!focused) return false;
        if (event.type == ui::UIEventType::Preedit) {
            preedit = event.preedit;
            return true;
        }
        if (event.type == ui::UIEventType::Character) {
            preedit.clear();
            if (event.codepoint >= 32 && event.codepoint != 127 &&
                text.size() < static_cast<std::size_t>(std::max(0, max_length))) {
                erase_selection();
                std::string addition;
                ui_detail::append_utf8(addition, event.codepoint);
                if (text.size() + addition.size() <=
                    static_cast<std::size_t>(std::max(0, max_length))) {
                    text.insert(cursor, addition);
                    cursor += addition.size();
                    selection_start = cursor;
                }
            }
            return true;
        }
        if (event.type != ui::UIEventType::KeyDown) return false;
        const bool ctrl = (event.modifiers & 1) != 0;
        const bool shift = (event.modifiers & 2) != 0;
        if (ctrl && event.key == static_cast<int>(ui::UIKey::A)) {
            selection_start = 0;
            cursor = text.size();
            return true;
        }
        if (ctrl && event.key == static_cast<int>(ui::UIKey::C)) {
            const std::size_t begin = std::min(cursor, selection_start);
            const std::size_t end = std::max(cursor, selection_start);
            if (end > begin) ui::set_clipboard_text(text.substr(begin, end - begin));
            return true;
        }
        if (ctrl && event.key == static_cast<int>(ui::UIKey::X)) {
            const std::size_t begin = std::min(cursor, selection_start);
            const std::size_t end = std::max(cursor, selection_start);
            if (end > begin) {
                ui::set_clipboard_text(text.substr(begin, end - begin));
                text.erase(begin, end - begin);
                cursor = selection_start = begin;
            }
            return true;
        }
        if (ctrl && event.key == static_cast<int>(ui::UIKey::V)) {
            const std::string pasted = ui::get_clipboard_text();
            if (!pasted.empty()) {
                erase_selection();
                const std::size_t available =
                    static_cast<std::size_t>(std::max(0, max_length)) > text.size()
                        ? static_cast<std::size_t>(std::max(0, max_length)) - text.size()
                        : 0;
                const std::string clipped = pasted.substr(0, available);
                text.insert(cursor, clipped);
                cursor += clipped.size();
                selection_start = cursor;
            }
            return true;
        }
        switch (static_cast<ui::UIKey>(event.key)) {
            case ui::UIKey::Backspace:
                if (cursor != selection_start) {
                    erase_selection();
                } else if (cursor > 0) {
                    const std::size_t previous = ui_detail::previous_codepoint(text, cursor);
                    text.erase(previous, cursor - previous);
                    cursor = previous;
                    selection_start = cursor;
                }
                return true;
            case ui::UIKey::Delete:
                if (cursor != selection_start) {
                    erase_selection();
                } else if (cursor < text.size()) {
                    const std::size_t next = ui_detail::next_codepoint(text, cursor);
                    text.erase(cursor, next - cursor);
                }
                return true;
            case ui::UIKey::Left:
                cursor = ui_detail::previous_codepoint(text, cursor);
                if (!shift) selection_start = cursor;
                return true;
            case ui::UIKey::Right:
                cursor = ui_detail::next_codepoint(text, cursor);
                if (!shift) selection_start = cursor;
                return true;
            case ui::UIKey::Home:
                cursor = 0;
                if (!shift) selection_start = cursor;
                return true;
            case ui::UIKey::End:
                cursor = text.size();
                if (!shift) selection_start = cursor;
                return true;
            default: return false;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        // Fluent 的 TextBox：顶部两角圆、底部两角方，靠一条底边指示边界。
        // 未聚焦是 1px 控件描边，聚焦后底边换成 2px 强调色——这是 Fluent 2 区分
        // "输入中"最核心的一条视觉，比四周套一圈焦点框更像原生控件。
        const float radius = ui::ui_resolve_radius(corner_radius, radius_tier);
        const uint32_t accent = themed_color(
            accent_role == UIThemeColor::Custom ? UIThemeColor::Accent : accent_role,
            ui::ui_color(cursor_color.x, cursor_color.y, cursor_color.z, cursor_color.w));
        const float corners[4] = {radius, radius, 0.0f, 0.0f};
        const float focus = std::max(focus_amount(), hover_amount() * 0.25f);
        draw_list.add_round_rect(rect, corners,
                                 themed_color(background_role,
                                     ui::ui_color(background.x, background.y,
                                                  background.z, background.w)),
                                 0u, 1.0f,
                                 ui::ui_control_stroke(hover_amount(), focus_amount()));
        const float bottom_width = 1.0f + focus;
        draw_list.add_rect({rect.x, rect.bottom() - bottom_width,
                            rect.w, bottom_width},
                           ui::ui_theme_mix(
                               ui::ui_with_alpha(
                                   ui::ui_theme_color(UIThemeColor::ControlStroke), 1.0f),
                               accent, focus));

        const float pad = 11.0f;
        const std::string display = password ? ui_detail::masked_text(text) : text;
        if (!display.empty()) {
            draw_list.add_text({rect.x + pad, rect.y, rect.w - pad * 2.0f, rect.h},
                               display, font_size,
                               themed_color(foreground_role,
                                   ui::ui_color(text_color.x, text_color.y,
                                                text_color.z, text_color.w)));
        }
        if (!preedit.empty()) {
            draw_list.add_text({rect.x + pad, rect.y, rect.w - pad * 2.0f, rect.h},
                               preedit, font_size,
                               themed_color(foreground_role,
                                   ui::ui_color(text_color.x, text_color.y,
                                                text_color.z, text_color.w)));
        }
        if (focused) {
            const float approximate = static_cast<float>(
                password ? display.size() : cursor) * font_size * 0.58f;
            draw_list.add_rect({rect.x + pad + approximate,
                                rect.y + 7.0f, 1.5f, std::max(1.0f, rect.h - 14.0f)},
                               accent);
        }
    }

protected:
    void erase_selection() {
        if (cursor == selection_start) return;
        const std::size_t begin = std::min(cursor, selection_start);
        const std::size_t end = std::max(cursor, selection_start);
        text.erase(begin, end - begin);
        cursor = begin;
        selection_start = begin;
    }

    std::size_t cursor = 0;
    std::size_t selection_start = 0;
    bool focused = false;
    std::string preedit;
};

// Fluent 的开关：40x20 的胶囊轨道 + 12px 圆头，而不是"整块染色的按钮"。
// 关闭态轨道是深色控件填充 + 亮描边，打开态轨道变强调色、圆头变白并滑到右端。
class UIToggleSwitch : public NodeUI {
public:
    std::string text = "Toggle";
    bool on = false;
    float font_size = 14.0f;
    // 圆头位置/轨道配色的过渡量：0 = 关，1 = 开。
    ui::UIFloatTween switch_tween;

    UIToggleSwitch() {
        size = math::Vector2f(180.0f, 32.0f);
        min_size = math::Vector2f(40.0f, 20.0f);
        focusable = true;
        accent_role = ui::UIThemeColor::Accent;
        foreground_role = ui::UIThemeColor::Text;
        radius_tier = ui::UIRadiusTier::Small;
        press_scale = 0.0f;
        switch_tween.reset(0.0f);
    }

    const char* type() const override { return "UIToggleSwitch"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIToggleSwitch)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["text"] = text;
        out["on"] = on;
        out["font_size"] = font_size;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        text = in.value("text", "Toggle");
        font_size = in.value("font_size", 14.0f);
        on = in.value("on", false);
        switch_tween.reset(on ? 1.0f : 0.0f);
    }
    void set_on(bool value) {
        if (on == value) return;
        on = value;
        const float seconds = std::max(0.001f, ui::ui_theme_metric(
            ui::UIThemeMetric::MotionNormal));
        switch_tween.start(switch_tween.value, value ? 1.0f : 0.0f, seconds,
                           ui::UIEasing::OutCubic);
    }
    bool advance_animations(float dt) override {
        const bool animated = switch_tween.advance(dt);
        return NodeUI::advance_animations(dt) || animated;
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type == ui::UIEventType::KeyDown &&
            (event.key == static_cast<int>(ui::UIKey::Enter) || event.key == 32)) {
            set_on(!on);
            return true;
        }
        if (event.type == ui::UIEventType::Click) {
            set_on(!on);
            return true;
        }
        return false;
    }
    void add_draw_commands(const ui::UIRect& rect, ui::UIDrawList& draw_list) const override {
        const float track_h = std::min(20.0f, rect.h);
        const float track_w = std::min(40.0f, rect.w);
        const ui::UIRect track{rect.x, rect.y + (rect.h - track_h) * 0.5f,
                               track_w, track_h};
        const float track_radius = ui::ui_pill_radius(track_h);
        const float t = std::clamp(switch_tween.value, 0.0f, 1.0f);
        const float hover = hover_amount();
        const uint32_t accent = themed_color(accent_role, ui::ui_theme_color(ui::UIThemeColor::Accent));

        // 关闭态：深色底 + 亮描边（Fluent 的 ToggleSwitchKnobFillOff 是一枚
        // 灰色圆头，看起来"停在左边"）；打开态：轨道整条强调色。
        const uint32_t off_track = ui::ui_with_alpha(
            ui::ui_theme_color(ui::UIThemeColor::ControlFillPressed),
            1.0f + 1.2f * hover);
        const uint32_t on_track = ui::ui_theme_mix(
            accent, ui::ui_theme_color(ui::UIThemeColor::AccentHover), hover * 0.7f);
        const uint32_t track_fill = ui::ui_theme_mix(off_track, on_track, t);
        const uint32_t track_stroke = ui::ui_theme_mix(
            ui::ui_with_alpha(ui::ui_theme_color(ui::UIThemeColor::ControlStroke),
                              1.0f + 0.6f * hover),
            accent, t);
        draw_list.add_round_rect(track, track_radius, track_fill, 0u, 1.0f,
                                 track_stroke);

        // 圆头：关闭时靠左、打开时靠右，两端留 4px。按下时缩 12% 做按压反馈。
        const float knob = std::min(12.0f, track_h - 8.0f) *
                           (1.0f + 0.12f * hover) * (1.0f - 0.12f * press_amount());
        const float travel = track.w - 8.0f - knob;
        const ui::UIRect knob_rect{track.x + 4.0f + travel * t,
                                   track.y + (track.h - knob) * 0.5f,
                                   knob, knob};
        const uint32_t knob_fill = ui::ui_theme_mix(
            ui::ui_theme_color(ui::UIThemeColor::TextMuted), 0xFFFFFFFFu, t);
        draw_list.add_round_rect(knob_rect, knob * 0.5f, knob_fill);

        if (!text.empty()) {
            draw_list.add_text({track.right() + 12.0f, rect.y,
                                std::max(0.0f, rect.w - track.w - 12.0f), rect.h},
                               text, font_size,
                               themed_color(foreground_role,
                                            ui::ui_theme_color(ui::UIThemeColor::Text)));
        }
        ui::ui_paint_focus_ring(draw_list, track, track_radius, focus_amount());
    }
};

class UIScrollView : public NodeUI {public:
    float scroll_y = 0.0f;
    float content_height = 0.0f;
    float wheel_step = 24.0f;

    UIScrollView() {
        size = math::Vector2f(240.0f, 200.0f);
        min_size = size;
        clip_contents = true;
        layout_mode = UILayoutMode::Column;
    }
    const char* type() const override { return "UIScrollView"; }
    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        NodeUI::collect_type_views(out);
        out.push_back({std::type_index(typeid(UIScrollView)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }
    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
        out["scroll_y"] = scroll_y;
        out["content_height"] = content_height;
        out["wheel_step"] = wheel_step;
    }
    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
        scroll_y = in.value("scroll_y", 0.0f);
        content_height = in.value("content_height", 0.0f);
        wheel_step = in.value("wheel_step", 24.0f);
    }
    bool on_ui_event(const ui::UIEvent& event) override {
        if (event.type != ui::UIEventType::Wheel) return false;
        const float maximum = std::max(0.0f, content_height - size.y);
        scroll_y = std::clamp(scroll_y - event.wheel.y * wheel_step, 0.0f, maximum);
        return true;
    }
    math::Vector2f content_offset() const override {
        return math::Vector2f(0.0f, -scroll_y);
    }
};

} // namespace gryce_engine::components
