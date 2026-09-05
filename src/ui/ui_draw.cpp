#include "ui_draw.h"

#include <algorithm>
#include <cmath>

namespace GryceEngineUtils::ui::detail {

int utf8_length(const std::string& s) {
    int count = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0x80) == 0) {
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
        } else {
            i += 1;
        }
        ++count;
    }
    return count;
}

float text_width(const std::string& s, float font_size) {
    // 粗略估算：平均字形宽约 0.55em
    return static_cast<float>(utf8_length(s)) * font_size * 0.55f;
}

std::string truncate_text(const std::string& s, float font_size, float max_width) {
    if (max_width <= 0.0f) return s;
    if (text_width(s, font_size) <= max_width) return s;

    std::string out;
    for (size_t i = 0; i < s.size();) {
        const size_t start = i;
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        out.append(s, start, len);
        i += len;
        if (text_width(out + "\u2026", font_size) > max_width) {
            out.pop_back();
            out.append("\u2026");
            break;
        }
    }
    return out;
}

void draw_rounded_rect(gryce_engine::render::IRenderer2D* r2d,
                       float x, float y, float w, float h,
                       float radius, const Color& color) {
    if (!r2d || w <= 0.0f || h <= 0.0f) return;
    const float r = std::clamp(radius, 0.0f, std::min(w, h) * 0.5f);
    if (r <= 0.5f) {
        r2d->draw_rect(x, y, w, h, color);
        return;
    }

    // 圆角矩形多边形：4 段圆弧 × 4 段 + 4 个角点
    std::vector<math::Vector2f> pts;
    constexpr int kSegPerCorner = 4;
    const float cx[4] = {x + r, x + w - r, x + w - r, x + r};
    const float cy[4] = {y + r, y + r, y + h - r, y + h - r};
    const float start_angle[4] = {180.0f, 270.0f, 0.0f, 90.0f};
    for (int corner = 0; corner < 4; ++corner) {
        for (int s = 0; s < kSegPerCorner; ++s) {
            const float deg = start_angle[corner] +
                              static_cast<float>(s) * (90.0f / kSegPerCorner);
            const float rad = deg * 3.14159265f / 180.0f;
            pts.emplace_back(cx[corner] + std::cos(rad) * r,
                             cy[corner] + std::sin(rad) * r);
        }
    }
    r2d->draw_polygon(pts, color);
}

void draw_focus_ring(gryce_engine::render::IRenderer2D* r2d,
                     float x, float y, float w, float h,
                     float radius, const Color& color) {
    if (!r2d || w <= 0.0f || h <= 0.0f) return;
    const float fw = 2.0f; // 焦点环宽度
    // 外扩 2px 绘制轮廓（用外圈减内圈模拟轮廓）
    const float ox = x - fw;
    const float oy = y - fw;
    const float ow = w + fw * 2.0f;
    const float oh = h + fw * 2.0f;
    const float r = std::clamp(radius, 0.0f, std::min(w, h) * 0.5f);
    // 绘制 4 条边（使用细矩形避免复杂多边形裁剪）
    // 上边
    r2d->draw_rect(ox, oy, ow, fw, color);
    // 下边
    r2d->draw_rect(ox, oy + oh - fw, ow, fw, color);
    // 左边
    r2d->draw_rect(ox, oy + fw, fw, oh - fw * 2.0f, color);
    // 右边
    r2d->draw_rect(ox + ow - fw, oy + fw, fw, oh - fw * 2.0f, color);
    (void)r;
}

float text_align_x(float bounds_x, float bounds_w, float text_width,
                   TextAlign align, float padding_left, float padding_right) {
    switch (align) {
        case TextAlign::Center:
            return bounds_x + (bounds_w - text_width) * 0.5f;
        case TextAlign::Right:
            return bounds_x + bounds_w - text_width - padding_right;
        case TextAlign::Left:
        default:
            return bounds_x + padding_left;
    }
}

float text_center_y(float bounds_y, float bounds_h, float font_size) {
    return bounds_y + (bounds_h - font_size) * 0.5f;
}

bool is_light_color(const Color& c) {
    // 标准亮度公式：0.2126*R + 0.7152*G + 0.0722*B
    const float luminance = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    return luminance > 0.5f;
}

Color widget_background(const Style& s, bool hovered, bool pressed) {
    if (pressed) return s.background_pressed;
    if (hovered) return s.background_hover;
    return s.background;
}

void draw_label_text(gryce_engine::render::IRenderer2D* r2d,
                     const Rect& bounds, const Style& s,
                     const std::string& text, float alpha,
                     const Padding& padding) {
    if (text.empty() || !r2d) return;
    const float font_size = s.font_size;
    const float max_w = std::max(0.0f, bounds.w - padding.left - padding.right);
    const std::string shown = truncate_text(text, font_size, max_w);
    const float tw = text_width(shown, font_size);
    const float x = text_align_x(bounds.x, bounds.w, tw, s.text_align, padding.left, padding.right);
    const float y = text_center_y(bounds.y, bounds.h, font_size);
    r2d->draw_text(x, y, shown, font_size, with_alpha(s.color, alpha));
}

} // namespace GryceEngineUtils::ui::detail
