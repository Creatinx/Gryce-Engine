#include "GryceEngineUtils/ui/toolbar.h"

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

ToolBar::ToolBar(const char* id) : Panel(id) {
    set_layout(LayoutType::Horizontal);
    set_spacing(6.0f);
    set_padding({8.0f, 6.0f, 8.0f, 6.0f});
    set_background(Color(0.10f, 0.11f, 0.14f, 1.0f));
}

Button* ToolBar::add_button(const char* id, const char* text) {
    auto* btn = new Button(id, text);
    const float font = btn->style().font_size;
    const float w = detail::text_width(text ? text : "", font) + 32.0f;
    btn->set_size(w, 0.0f);
    btn->style().padding = Padding{8.0f, 4.0f, 8.0f, 4.0f};
    add_child(btn);
    return btn;
}

Divider* ToolBar::add_divider(const char* id) {
    auto* div = new Divider(id ? id : "toolbar_divider");
    div->set_vertical(true);
    div->set_size(2.0f, 22.0f);
    add_child(div);
    return div;
}

} // namespace GryceEngineUtils::ui
