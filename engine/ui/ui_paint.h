// Fluent 风格的表面绘制：圆角阶梯、层次（投影）、以及悬停/按下的状态色。
//
// 控件只描述"这是什么表面"（半径档、底色、是否有层次），具体怎么展开成
// 圆角三角形、投影画在填充之前还是之后，全在这里统一决定。这样所有控件的
// 圆角观感一致，换主题半径时不需要逐个控件改数值。

#pragma once

#include <algorithm>

#include "ui/ui_drawlist.h"
#include "ui/ui_theme.h"

namespace gryce_engine::ui {

// ---------------------------------------------------------------------------
// 圆角阶梯
// ---------------------------------------------------------------------------

struct UISurface {
    UIRect rect{};
    float radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t fill = 0xFFFFFFFFu;
    // 非 0 时按"上浅下深"竖直渐变到该色，Fluent 的表面有微弱顶光。
    uint32_t fill_bottom = 0x00000000u;
    uint32_t border = 0x00000000u;
    float border_width = 0.0f;
    // 投影扩散半径，0 = 不投影。
    float elevation = 0.0f;
};

// 控件的 corner_radius < 0 时按自己的 radius_tier 取主题值，否则用显式值。
inline float ui_resolve_radius(float explicit_radius, UIRadiusTier tier) {
    return explicit_radius >= 0.0f ? explicit_radius : ui_theme_radius(tier);
}

inline void ui_fill_radii(float radius, float out[4]) {
    const float r = std::max(0.0f, radius);
    out[0] = out[1] = out[2] = out[3] = r;
}

// 胶囊形半径（进度条、开关轨道）：高度的一半。
inline float ui_pill_radius(float height) { return std::max(0.0f, height) * 0.5f; }

// ---------------------------------------------------------------------------
// 贴图填充
// ---------------------------------------------------------------------------

enum class UIFitMode : uint8_t {
    // 拉伸到整个盒子（会改变长宽比）。
    Stretch = 0,
    // 等比缩放到完全放得下，居中留白（默认）。
    Contain,
    // 等比缩放到铺满盒子，超出部分裁掉（相册缩略图）。
    Cover,
    // 不缩放，按原像素居中；放不下时按原尺寸裁切。
    Native,
};

// 贴图在盒子里的目标矩形与采样范围。Cover/Native 需要同时裁 UV，否则
// 只会得到"拉伸变形"而不是"裁切"。
struct UIFittedImage {
    UIRect rect{};
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
};

inline UIFittedImage ui_fit_image(const UIRect& box, int src_w, int src_h,
                                  UIFitMode mode) {
    UIFittedImage out;
    out.rect = box;
    if (src_w <= 0 || src_h <= 0 || box.w <= 0.0f || box.h <= 0.0f) return out;
    const float src_aspect = static_cast<float>(src_w) / static_cast<float>(src_h);
    const float box_aspect = box.w / std::max(1.0f, box.h);
    switch (mode) {
        case UIFitMode::Stretch:
            break;
        case UIFitMode::Contain:
            if (src_aspect > box_aspect) {
                out.rect.h = box.w / src_aspect;
                out.rect.y += (box.h - out.rect.h) * 0.5f;
            } else {
                out.rect.w = box.h * src_aspect;
                out.rect.x += (box.w - out.rect.w) * 0.5f;
            }
            break;
        case UIFitMode::Cover:
            if (src_aspect > box_aspect) {
                // 源更宽：左右各裁掉一份，保留中间。
                const float keep = box_aspect / src_aspect;
                out.u0 = (1.0f - keep) * 0.5f;
                out.u1 = 1.0f - out.u0;
            } else {
                const float keep = src_aspect / box_aspect;
                out.v0 = (1.0f - keep) * 0.5f;
                out.v1 = 1.0f - out.v0;
            }
            break;
        case UIFitMode::Native: {
            // 目标框按原像素尺寸居中；比盒子大的方向按盒子裁 UV。
            const float draw_w = static_cast<float>(src_w);
            const float draw_h = static_cast<float>(src_h);
            if (draw_w > box.w) {
                const float keep = box.w / draw_w;
                out.u0 = (1.0f - keep) * 0.5f;
                out.u1 = 1.0f - out.u0;
                out.rect.w = box.w;
            } else {
                out.rect.w = draw_w;
            }
            if (draw_h > box.h) {
                const float keep = box.h / draw_h;
                out.v0 = (1.0f - keep) * 0.5f;
                out.v1 = 1.0f - out.v0;
                out.rect.h = box.h;
            } else {
                out.rect.h = draw_h;
            }
            out.rect.x += (box.w - out.rect.w) * 0.5f;
            out.rect.y += (box.h - out.rect.h) * 0.5f;
            break;
        }
    }
    return out;
}

// 按下时的视觉内缩（Fluent 的按压反馈）。只缩绘制用的矩形，不动布局矩形：
// 布局矩形同时用于命中测试，收缩会让指针在控件边缘反复进出。
inline UIRect ui_press_rect(const UIRect& rect, float press_scale,
                            float press_amount) {
    if (press_scale <= 0.0f || press_amount <= 0.0f) return rect;
    const float shrink = press_amount * press_scale;
    const float dx = rect.w * shrink * 0.5f;
    const float dy = rect.h * shrink * 0.5f;
    return {rect.x + dx, rect.y + dy,
            std::max(0.0f, rect.w - dx * 2.0f),
            std::max(0.0f, rect.h - dy * 2.0f)};
}

// ---------------------------------------------------------------------------
// 颜色工具
// ---------------------------------------------------------------------------

// 只改 alpha，保留 RGB。
inline uint32_t ui_with_alpha(uint32_t rgba, float alpha) {
    const float clamped = std::clamp(alpha, 0.0f, 1.0f);
    return (rgba & 0xFFFFFF00u) | static_cast<uint32_t>(clamped * 255.0f + 0.5f);
}

// ---------------------------------------------------------------------------
// 表面绘制
// ---------------------------------------------------------------------------

// 按"投影 -> 填充 -> 描边"的顺序提交命令。投影向下偏移约三分之一个扩散半径，
// 这是 Fluent 的默认光源方向（略偏上）。
inline void ui_paint_surface(UIDrawList& draw_list, UISurface surface) {
    if (surface.rect.empty()) return;

    if (surface.elevation > 0.0f) {
        const uint32_t shadow_color = ui_theme_color(UIThemeColor::Shadow);
        if ((shadow_color & 0xFFu) != 0u) {
            const UIRect shadow_rect{surface.rect.x,
                                     surface.rect.y + surface.elevation * 0.45f,
                                     surface.rect.w, surface.rect.h};
            draw_list.add_round_shadow(shadow_rect, surface.radii[0],
                                       surface.elevation, shadow_color);
        }
    }

    if (surface.border_width > 0.0f && (surface.border & 0xFFu) != 0u) {
        draw_list.add_round_rect(surface.rect, surface.radii, surface.fill,
                                 surface.fill_bottom, surface.border_width,
                                 surface.border);
    } else if (surface.fill_bottom != 0u) {
        draw_list.add_round_rect(surface.rect, surface.radii, surface.fill,
                                 surface.fill_bottom);
    } else {
        draw_list.add_round_rect(surface.rect, surface.radii, surface.fill);
    }
}

// 焦点框：Fluent 的 focus visual 是贴着控件内缘的两像素描边，随焦点淡入。
inline void ui_paint_focus_ring(UIDrawList& draw_list, const UIRect& rect,
                                float radius, float focus_amount,
                                bool accent = true) {
    if (focus_amount <= 0.01f) return;
    const uint32_t base = ui_theme_color(accent ? UIThemeColor::Accent
                                                : UIThemeColor::Border);
    float radii[4];
    ui_fill_radii(radius, radii);
    draw_list.add_round_rect(rect, radii, 0x00000000u, 0u, 2.0f,
                             ui_with_alpha(base, focus_amount * 0.9f));
}

// ---------------------------------------------------------------------------
// Fluent 控件表面
// ---------------------------------------------------------------------------

// 控件的三态填充。三档都取自主题的半透明 ControlFill，直接按交互量插值，
// 不必给每个控件单独记一组 rest/hover/pressed 颜色。
inline uint32_t ui_control_fill(float hover_amount, float press_amount,
                                float rest_alpha = 1.0f) {
    uint32_t color = ui_theme_mix(ui_theme_color(UIThemeColor::ControlFill),
                                  ui_theme_color(UIThemeColor::ControlFillHover),
                                  hover_amount);
    color = ui_theme_mix(color, ui_theme_color(UIThemeColor::ControlFillPressed),
                         press_amount);
    if (rest_alpha < 1.0f) {
        const float alpha = static_cast<float>(color & 0xFFu) / 255.0f;
        color = ui_with_alpha(color, alpha * rest_alpha);
    }
    return color;
}

// 控件的发丝边：静止态就存在（Fluent 的按钮靠这一圈白边定位边界），
// hover 提亮、聚焦再压一档。
inline uint32_t ui_control_stroke(float hover_amount, float focus_amount) {
    return ui_with_alpha(ui_theme_color(UIThemeColor::ControlStroke),
                         1.0f + 0.8f * hover_amount + 0.6f * focus_amount);
}

// 标准控件的绘制流程：投影 -> 填充 -> 描边 -> 焦点环。返回用于文字/图标
// 计算的绘制矩形（已按按压内缩）。
inline UIRect ui_paint_control(UIDrawList& draw_list, const UIRect& rect,
                               const float radii[4], uint32_t fill,
                               float elevation, float hover_amount,
                               float focus_amount, float border_width = 1.0f) {
    UISurface surface;
    surface.rect = rect;
    for (int i = 0; i < 4; ++i) surface.radii[i] = radii[i];
    surface.fill = fill;
    surface.border = ui_control_stroke(hover_amount, focus_amount);
    surface.border_width = border_width;
    surface.elevation = elevation;
    ui_paint_surface(draw_list, surface);
    return rect;
}

// ---------------------------------------------------------------------------
// 图标笔画（勾选标记 / 下拉箭头 / 分隔线）
// ---------------------------------------------------------------------------

// 勾选标记：两笔圆头线，比例按 20x20 的方形取，缩放到任意尺寸都成立。
inline void ui_paint_check(UIDrawList& draw_list, const UIRect& box,
                           uint32_t color, float thickness) {
    const float s = std::min(box.w, box.h);
    const float x = box.x + (box.w - s) * 0.5f;
    const float y = box.y + (box.h - s) * 0.5f;
    const float t = thickness > 0.0f ? thickness : std::max(1.0f, s * 0.12f);
    draw_list.add_line(x + s * 0.26f, y + s * 0.53f,
                       x + s * 0.44f, y + s * 0.70f, t, color);
    draw_list.add_line(x + s * 0.44f, y + s * 0.70f,
                       x + s * 0.75f, y + s * 0.32f, t, color);
}

// 下拉箭头：一个倒 V。size 是箭头开口宽度。
inline void ui_paint_chevron(UIDrawList& draw_list, float cx, float cy,
                             float size, uint32_t color, float thickness,
                             bool up = false) {
    const float half = size * 0.5f;
    const float h = size * 0.32f;
    const float dir = up ? -1.0f : 1.0f;
    const float t = thickness > 0.0f ? thickness : 1.4f;
    draw_list.add_line(cx - half, cy - h * dir * 0.5f,
                       cx, cy + h * dir * 0.5f, t, color);
    draw_list.add_line(cx, cy + h * dir * 0.5f,
                       cx + half, cy - h * dir * 0.5f, t, color);
}

// 列表/树/表格的选中行：圆角淡强调底 + 左侧 3px 竖条。整行铺满饱和强调色
// 是老式列表的做法，Fluent 只用一层淡底加一条指示条。
inline void ui_paint_row_selection(UIDrawList& draw_list, const UIRect& row,
                                   uint32_t accent, float inset = 2.0f) {
    if (row.w <= 0.0f || row.h <= 0.0f) return;
    const float radius = ui_theme_radius(UIRadiusTier::Small);
    const UIRect body{row.x + inset, row.y + 1.0f,
                      std::max(0.0f, row.w - inset * 2.0f),
                      std::max(0.0f, row.h - 2.0f)};
    draw_list.add_round_rect(body, radius, ui_with_alpha(accent, 0.28f));
    draw_list.add_round_rect({body.x + 2.0f, body.y + body.h * 0.22f,
                              3.0f, body.h * 0.56f},
                             ui_pill_radius(3.0f), accent);
}

// 1 像素水平分隔线（Fluent 的 DividerStrokeColorDefault）。用 1 像素矩形，
// 不用 Line：水平线走直角快路径，不展开三角形。
inline void ui_paint_divider(UIDrawList& draw_list, float x, float y, float w) {
    draw_list.add_rect({x, y, w, 1.0f},
                       ui_with_alpha(ui_theme_color(UIThemeColor::Border), 0.7f));
}

} // namespace gryce_engine::ui
