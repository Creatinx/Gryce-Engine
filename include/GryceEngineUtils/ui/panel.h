#pragma once

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

// Flexbox 交叉轴对齐（align-items）
enum class AlignItems {
    FlexStart, Center, FlexEnd, Stretch
};

// Flexbox 主轴对齐（justify-content）
enum class JustifyContent {
    FlexStart, Center, FlexEnd, SpaceBetween, SpaceAround
};

class Panel : public Widget {
public:
    explicit Panel(const char* id);

    void set_layout(LayoutType type);
    LayoutType layout_type() const { return layout_type_; }
    void set_spacing(float gap);
    void set_background(const Color& color);
    void set_columns(int columns); // Grid 布局列数（0 = 自动按数量开方）

    // Flexbox 对齐属性
    void set_align_items(AlignItems align);
    AlignItems align_items() const { return align_items_; }
    void set_justify_content(JustifyContent justify);
    JustifyContent justify_content() const { return justify_content_; }

    void draw(Renderer* renderer) override;
    const char* type_name() const override { return "Panel"; }
    bool set_property(const char* key, const char* value) override;

    // 布局：计算子控件 bounds
    void relayout();

protected:
    void compute_bounds(const Rect& parent_bounds) override;

private:
    LayoutType layout_type_ = LayoutType::Absolute;
    float spacing_ = 4.0f;
    int columns_ = 0;
    AlignItems align_items_ = AlignItems::Stretch;
    JustifyContent justify_content_ = JustifyContent::FlexStart;
};

} // namespace GryceEngineUtils::ui
