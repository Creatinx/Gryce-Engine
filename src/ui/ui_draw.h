#pragma once

// 私有绘制辅助：UI 模块内部共享（不对外暴露）

#include <string>
#include <vector>

#include "GryceEngineUtils/ui/ui.h"
#include "render/render2d.h"

namespace GryceEngineUtils::ui::detail {

// UTF-8 字符数
int utf8_length(const std::string& s);

// 估算文本像素宽度（按字体尺寸比例）
float text_width(const std::string& s, float font_size);

// 截断文本到估算宽度，超出部分以省略号结尾
std::string truncate_text(const std::string& s, float font_size, float max_width);

// 圆角矩形近似（用多边形，每角 4 段）
void draw_rounded_rect(gryce_engine::render::IRenderer2D* r2d,
                       float x, float y, float w, float h,
                       float radius, const Color& color);

// 应用透明度
inline Color with_alpha(const Color& c, float alpha) {
    return Color(c.r, c.g, c.b, c.a * alpha);
}

// 焦点环（外扩 2px 的圆角矩形轮廓）
void draw_focus_ring(gryce_engine::render::IRenderer2D* r2d,
                     float x, float y, float w, float h,
                     float radius, const Color& color);

// 文本对齐辅助：计算文本在给定矩形中的 x 位置
float text_align_x(float bounds_x, float bounds_w, float text_width,
                   TextAlign align, float padding_left, float padding_right);

// 文本垂直居中 y 坐标
float text_center_y(float bounds_y, float bounds_h, float font_size);

// 检测颜色是否明显"亮"（用于决定文本颜色）
bool is_light_color(const Color& c);

// 根据 hover/pressed 状态返回正确的背景色
Color widget_background(const Style& s, bool hovered, bool pressed);

// 通用标签文本绘制（整合文本对齐、截断、居中）
void draw_label_text(gryce_engine::render::IRenderer2D* r2d,
                     const Rect& bounds, const Style& s,
                     const std::string& text, float alpha,
                     const Padding& padding);

} // namespace GryceEngineUtils::ui::detail
