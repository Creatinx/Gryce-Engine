#pragma once

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

// Divider — 分隔线（ToolBar / 菜单分组用）
class Divider : public Widget {
public:
    explicit Divider(const char* id = "divider");

    // orientation: true = 垂直分隔线（工具栏中），false = 水平
    void set_vertical(bool vertical) { vertical_ = vertical; }
    bool is_vertical() const { return vertical_; }
    void set_color(const Color& color) { color_ = color; }

    void draw(Renderer* renderer) override;
    const char* type_name() const override { return "Divider"; }

private:
    bool vertical_ = true;
    Color color_ = Color(0.35f, 0.36f, 0.4f, 1.0f);
};

} // namespace GryceEngineUtils::ui
