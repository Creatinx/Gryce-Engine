#include "GryceEngineUtils/ui/panel.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "ui_draw.h"

namespace GryceEngineUtils::ui {

Panel::Panel(const char* id) : Widget(id) {
    style_.has_background = false; // 容器默认透明
}

void Panel::set_layout(LayoutType type) {
    layout_type_ = type;
    layout_dirty_ = true;
}

void Panel::set_spacing(float gap) {
    spacing_ = std::max(0.0f, gap);
    layout_dirty_ = true;
}

void Panel::set_background(const Color& color) {
    style_.background = color;
    style_.has_background = true;
}

void Panel::set_columns(int columns) {
    columns_ = std::max(0, columns);
    layout_dirty_ = true;
}

void Panel::set_align_items(AlignItems align) {
    align_items_ = align;
    layout_dirty_ = true;
}

void Panel::set_justify_content(JustifyContent justify) {
    justify_content_ = justify;
    layout_dirty_ = true;
}

void Panel::draw(Renderer* renderer) {
    if (!visible_) return;
    if (style_.has_background) {
        Widget::draw(renderer);
    }
}

void Panel::compute_bounds(const Rect& parent_bounds) {
    Widget::compute_bounds(parent_bounds);
}

void Panel::relayout() {
    if (!layout_dirty_) return;
    // 注意：Vertical/Horizontal/Grid 布局模式下，子控件的 position 和 anchor 设置会被忽略，
    // 子控件位置由布局算法自动计算。如需手动控制位置，请使用 Absolute 布局模式。
    const float inner_x = bounds_.x + padding_.left;
    const float inner_y = bounds_.y + padding_.top;
    const float inner_w = std::max(0.0f, bounds_.w - padding_.left - padding_.right);
    const float inner_h = std::max(0.0f, bounds_.h - padding_.top - padding_.bottom);

    if (children_.empty()) {
        layout_dirty_ = false;
        return;
    }

    switch (layout_type_) {
        case LayoutType::Vertical: {
            // --- 第 1 轮：计算总 flex-grow 和固定尺寸子项的高度 ---
            float total_flex_grow = 0.0f;
            float total_fixed_height = 0.0f;
            for (auto* child : children_) {
                if (child->flex_grow() > 0.0f) {
                    total_flex_grow += child->flex_grow();
                } else {
                    const float h = child->size_h() > 0.0f
                                        ? child->size_h()
                                        : child->preferred_height(inner_w);
                    total_fixed_height += h;
                }
                total_fixed_height += spacing_;
            }
            total_fixed_height = std::max(0.0f, total_fixed_height - spacing_);

            // --- 可用空间分配 ---
            const float available = std::max(0.0f, inner_h - total_fixed_height);
            const float flex_unit = total_flex_grow > 0.0f ? available / total_flex_grow : 0.0f;

            // --- 计算内容总高度（用于 justify-content） ---
            float content_height = total_fixed_height;
            for (auto* child : children_) {
                if (child->flex_grow() > 0.0f) {
                    content_height += flex_unit * child->flex_grow();
                }
            }

            // --- justify-content: 计算起始 y ---
            float y = inner_y;
            const float extra_space = std::max(0.0f, inner_h - content_height);
            if (justify_content_ == JustifyContent::Center) {
                y += extra_space * 0.5f;
            } else if (justify_content_ == JustifyContent::FlexEnd) {
                y += extra_space;
            }

            const size_t n = children_.size();
            // --- SpaceAround: 第一个子控件前添加间距 ---
            if (justify_content_ == JustifyContent::SpaceAround && total_flex_grow == 0.0f) {
                y += extra_space / n;
            }

            // --- 第 2 轮：定位子控件 ---
            for (size_t i = 0; i < n; ++i) {
                auto* child = children_[i];

                // 宽度：由 align-items 控制
                float w;
                if (align_items_ == AlignItems::Stretch || align_items_ == AlignItems::FlexStart) {
                    w = child->size_w() > 0.0f ? std::min(child->size_w(), inner_w) : inner_w;
                } else {
                    w = child->size_w() > 0.0f
                            ? std::min(child->size_w(), inner_w)
                            : child->preferred_width();
                }

                // 高度：flex-grow 子控件高度由 flex 分配决定，不叠加 preferred_height
                float h;
                if (child->flex_grow() > 0.0f && total_flex_grow > 0.0f) {
                    h = flex_unit * child->flex_grow();
                    if (child->size_h() > 0.0f) {
                        h = std::max(h, child->size_h());
                    }
                } else {
                    h = child->size_h() > 0.0f
                            ? child->size_h()
                            : child->preferred_height(w);
                }

                // align-items: 计算 x 偏移
                float x = inner_x;
                if (align_items_ == AlignItems::Center) {
                    x += (inner_w - w) * 0.5f;
                } else if (align_items_ == AlignItems::FlexEnd) {
                    x += inner_w - w;
                } // Stretch / FlexStart → x = inner_x

                child->set_bounds(Rect{x, y, w, h});
                y += h + spacing_;

                // SpaceBetween / SpaceAround: 除最后一项外额外分配间距
                if (justify_content_ == JustifyContent::SpaceBetween && total_flex_grow == 0.0f) {
                    if (i < n - 1) {
                        y += extra_space / (n - 1);
                    }
                } else if (justify_content_ == JustifyContent::SpaceAround && total_flex_grow == 0.0f) {
                    y += extra_space / n;
                }
            }
            break;
        }
        case LayoutType::Horizontal: {
            // --- 第 1 轮：计算总 flex-grow 和固定尺寸子项的宽度 ---
            float total_flex_grow = 0.0f;
            float total_fixed_width = 0.0f;
            for (auto* child : children_) {
                if (child->flex_grow() > 0.0f) {
                    total_flex_grow += child->flex_grow();
                } else {
                    const float w = child->size_w() > 0.0f
                                        ? child->size_w()
                                        : child->preferred_width();
                    total_fixed_width += w;
                }
                total_fixed_width += spacing_;
            }
            total_fixed_width = std::max(0.0f, total_fixed_width - spacing_);

            // --- 可用空间分配 ---
            const float available = std::max(0.0f, inner_w - total_fixed_width);
            const float flex_unit = total_flex_grow > 0.0f ? available / total_flex_grow : 0.0f;

            // --- 计算内容总宽度（用于 justify-content） ---
            float content_width = total_fixed_width;
            for (auto* child : children_) {
                if (child->flex_grow() > 0.0f) {
                    content_width += flex_unit * child->flex_grow();
                }
            }

            // --- justify-content: 计算起始 x ---
            float x = inner_x;
            const float extra_space = std::max(0.0f, inner_w - content_width);
            if (justify_content_ == JustifyContent::Center) {
                x += extra_space * 0.5f;
            } else if (justify_content_ == JustifyContent::FlexEnd) {
                x += extra_space;
            }

            const size_t n = children_.size();
            // --- SpaceAround: 第一个子控件前添加间距 ---
            if (justify_content_ == JustifyContent::SpaceAround && total_flex_grow == 0.0f) {
                x += extra_space / n;
            }

            // --- 第 2 轮：定位子控件 ---
            for (size_t i = 0; i < n; ++i) {
                auto* child = children_[i];

                // 宽度：flex-grow 子控件宽度由 flex 分配决定，不叠加 preferred_width
                float w;
                if (child->flex_grow() > 0.0f && total_flex_grow > 0.0f) {
                    w = flex_unit * child->flex_grow();
                    if (child->size_w() > 0.0f) {
                        w = std::max(w, child->size_w());
                    }
                } else {
                    w = child->size_w() > 0.0f
                            ? child->size_w()
                            : child->preferred_width();
                }

                // 高度：由 align-items 控制
                float h;
                if (align_items_ == AlignItems::Stretch || align_items_ == AlignItems::FlexStart) {
                    h = child->size_h() > 0.0f ? std::min(child->size_h(), inner_h) : inner_h;
                } else {
                    h = child->size_h() > 0.0f
                            ? std::min(child->size_h(), inner_h)
                            : child->preferred_height(w);
                }

                // align-items: 计算 y 偏移
                float y = inner_y;
                if (align_items_ == AlignItems::Center) {
                    y += (inner_h - h) * 0.5f;
                } else if (align_items_ == AlignItems::FlexEnd) {
                    y += inner_h - h;
                } // Stretch / FlexStart → y = inner_y

                child->set_bounds(Rect{x, y, w, h});
                x += w + spacing_;

                // SpaceBetween / SpaceAround
                if (justify_content_ == JustifyContent::SpaceBetween && total_flex_grow == 0.0f) {
                    if (i < n - 1) {
                        x += extra_space / (n - 1);
                    }
                } else if (justify_content_ == JustifyContent::SpaceAround && total_flex_grow == 0.0f) {
                    x += extra_space / n;
                }
            }
            break;
        }
        case LayoutType::Grid: {
            const int n = static_cast<int>(children_.size());
            int cols = columns_ > 0 ? columns_ : std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<float>(n)))));
            const int rows = (n + cols - 1) / cols;
            const float cell_w = (inner_w - spacing_ * (cols - 1)) / cols;
            float max_h = 0.0f;
            for (auto* child : children_) {
                const float h = child->size_h() > 0.0f ? child->size_h()
                                                       : child->preferred_height(cell_w);
                max_h = std::max(max_h, h);
            }
            const float cell_h = (inner_h > 0.0f && inner_h >= max_h * rows)
                                     ? (inner_h - spacing_ * (rows - 1)) / rows
                                     : max_h;
            int i = 0;
            for (auto* child : children_) {
                const int r = i / cols;
                const int c = i % cols;
                child->set_bounds(Rect{inner_x + c * (cell_w + spacing_),
                                       inner_y + r * (cell_h + spacing_),
                                       cell_w, cell_h});
                ++i;
            }
            break;
        }
        case LayoutType::Absolute:
        default: {
            for (auto* child : children_) {
                child->compute_bounds(bounds_);
            }
            break;
        }
    }
    layout_dirty_ = false;
}

bool Panel::set_property(const char* key, const char* value) {
    if (std::strcmp(key, "layout") == 0) {
        if (std::strcmp(value, "vertical") == 0) set_layout(LayoutType::Vertical);
        else if (std::strcmp(value, "horizontal") == 0) set_layout(LayoutType::Horizontal);
        else if (std::strcmp(value, "grid") == 0) set_layout(LayoutType::Grid);
        else if (std::strcmp(value, "absolute") == 0) set_layout(LayoutType::Absolute);
        return true;
    }
    if (std::strcmp(key, "spacing") == 0) {
        set_spacing(static_cast<float>(std::atof(value)));
        return true;
    }
    if (std::strcmp(key, "columns") == 0) {
        set_columns(std::atoi(value));
        return true;
    }
    // Flexbox：align-items
    if (std::strcmp(key, "align-items") == 0) {
        if (std::strcmp(value, "flex-start") == 0) set_align_items(AlignItems::FlexStart);
        else if (std::strcmp(value, "center") == 0) set_align_items(AlignItems::Center);
        else if (std::strcmp(value, "flex-end") == 0) set_align_items(AlignItems::FlexEnd);
        else if (std::strcmp(value, "stretch") == 0) set_align_items(AlignItems::Stretch);
        return true;
    }
    // Flexbox：justify-content
    if (std::strcmp(key, "justify-content") == 0) {
        if (std::strcmp(value, "flex-start") == 0) set_justify_content(JustifyContent::FlexStart);
        else if (std::strcmp(value, "center") == 0) set_justify_content(JustifyContent::Center);
        else if (std::strcmp(value, "flex-end") == 0) set_justify_content(JustifyContent::FlexEnd);
        else if (std::strcmp(value, "space-between") == 0) set_justify_content(JustifyContent::SpaceBetween);
        else if (std::strcmp(value, "space-around") == 0) set_justify_content(JustifyContent::SpaceAround);
        return true;
    }
    return Widget::set_property(key, value);
}

} // namespace GryceEngineUtils::ui
