// 圆角矩形的 CPU 侧几何构造（与后端无关）。
//
// 2D 顶点格式只有 position/color/uv，塞不进"圆角半径"这类逐图元参数，所以圆角
// 在 CPU 侧展开成折线扇形，边缘再加一圈 1 像素的 alpha 渐隐带做抗锯齿。两套
// 渲染器（Renderer2D 走 GL/D3D12，VulkanRenderer2D 独立实现）共用这里的几何，
// 保证同一条圆角命令在三个后端上形状与软边完全一致。
//
// 各 emit_* 都是模板：后端只需传一个"压入顶点"的可调用对象，不必关心顶点缓冲
// 的布局，也不会因为共享代码而互相依赖。

#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

#include "render/render2d.h"

namespace gryce_engine::render_detail {

using Color = render::Color;

// 圆角轮廓上的点：位置 + 单位外法线（沿法线偏移即可做内缩/外扩）。
struct RoundPt {
    float x = 0.0f;
    float y = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
};

constexpr float kRoundPi = 3.14159265358979323846f;

// 抗锯齿带宽：内侧 0.5、外侧 0.5，合计 1 像素的渐隐带。
constexpr float kRoundAa = 0.5f;

// 半径钳制：相邻两角共享一条边，两半径之和超过边长时按比例整体收缩，
// 否则大圆角会把对边压成负长度，扇形会翻折出自交的黑块。
inline void shrink_round_radii(float w, float h, float radii[4]) {
    for (int i = 0; i < 4; ++i) {
        if (!(radii[i] > 0.0f)) radii[i] = 0.0f;
    }
    const float max_r = ((w < h) ? w : h) * 0.5f;
    for (int i = 0; i < 4; ++i) {
        if (radii[i] > max_r) radii[i] = max_r;
    }
    // 四条边各自的约束：T(r0+r1<=w) B(r3+r2<=w) L(r0+r3<=h) R(r1+r2<=h)
    const float pairs[4][2] = {{0, 1}, {3, 2}, {0, 3}, {1, 2}};
    const float limits[4] = {w, w, h, h};
    float scale = 1.0f;
    for (int k = 0; k < 4; ++k) {
        const float sum = radii[static_cast<int>(pairs[k][0])] +
                          radii[static_cast<int>(pairs[k][1])];
        if (sum > limits[k] && sum > 0.0f) {
            const float s = limits[k] / sum;
            if (s < scale) scale = s;
        }
    }
    if (scale < 1.0f) {
        for (int i = 0; i < 4; ++i) radii[i] *= scale;
    }
}

// 每角的分段数：由**基准**半径算，不随 outset 变——内轮廓与外轮廓的顶点数
// 必须相等才能组成环带；按各自半径各算一套会让"内外点数不同"，环带被整段
// 丢掉（症状是阴影或描边只剩外半圈）。
inline int round_arc_segments(const float radii[4]) {
    float m = 0.0f;
    for (int i = 0; i < 4; ++i) {
        if (radii[i] > m) m = radii[i];
    }
    int seg = static_cast<int>(m * 1.2f) + 2;
    if (seg < 3) seg = 3;
    if (seg > 16) seg = 16;
    return seg;
}

// 外轮廓折线（顺时针，左上原点 +y 向下）。outset > 0 外扩、< 0 内缩：
// 圆角矩形沿法线偏移 d 正好等于"矩形与半径同时加 d"，所以这里统一处理。
// 每角恒定输出 seg+1 个点（半径为 0 时全部落在角点上、法线沿两边插值），
// 这样不同 outset 的轮廓点数一致，环带不会因为点数不齐被丢弃。
inline void build_round_outline(float x, float y, float w, float h,
                                const float radii_in[4], float outset, int seg,
                                std::vector<RoundPt>& out) {
    out.clear();
    if (w <= 0.0f || h <= 0.0f) return;

    const float x0 = x - outset;
    const float y0 = y - outset;
    const float x1 = x + w + outset;
    const float y1 = y + h + outset;
    if (x1 <= x0 || y1 <= y0) return;

    float r[4] = {radii_in[0], radii_in[1], radii_in[2], radii_in[3]};
    for (int i = 0; i < 4; ++i) {
        r[i] = (r[i] > 0.0f) ? (r[i] + outset) : 0.0f;
        if (r[i] < 0.0f) r[i] = 0.0f;
    }
    shrink_round_radii(x1 - x0, y1 - y0, r);

    out.reserve(static_cast<std::size_t>(seg + 1) * 4);

    // 四角的圆弧中心、起始角、直角时的角点与两条边法线
    // (顺序：左上 右上 右下 左下)
    const float cx[4] = {x0 + r[0], x1 - r[1], x1 - r[2], x0 + r[3]};
    const float cy[4] = {y0 + r[0], y0 + r[1], y1 - r[2], y1 - r[3]};
    const float a0[4] = {kRoundPi, kRoundPi * 1.5f, 0.0f, kRoundPi * 0.5f};
    const float px[4] = {x0, x1, x1, x0};
    const float py[4] = {y0, y0, y1, y1};
    const float n0x[4] = {-1.0f, 0.0f, 1.0f, 0.0f};
    const float n0y[4] = {0.0f, -1.0f, 0.0f, 1.0f};
    const float n1x[4] = {0.0f, 1.0f, 0.0f, -1.0f};
    const float n1y[4] = {-1.0f, 0.0f, 1.0f, 0.0f};

    for (int k = 0; k < 4; ++k) {
        for (int i = 0; i <= seg; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(seg);
            const float a = a0[k] + t * (kRoundPi * 0.5f);
            const float c = std::cos(a);
            const float s = std::sin(a);
            if (r[k] > 0.0f) {
                out.push_back({cx[k] + r[k] * c, cy[k] + r[k] * s, c, s});
            } else {
                float nx = n0x[k] + (n1x[k] - n0x[k]) * t;
                float ny = n0y[k] + (n1y[k] - n0y[k]) * t;
                const float len = std::sqrt(nx * nx + ny * ny);
                if (len > 1e-6f) { nx /= len; ny /= len; }
                out.push_back({px[k], py[k], nx, ny});
            }
        }
    }
}

// 圆角填充（top/bottom 相同即纯色），边缘带 1 像素 alpha 渐隐。
template <typename Push>
inline void emit_round_fill(Push&& push, float x, float y, float w, float h,
                            const float radii[4], const Color& top,
                            const Color& bottom) {
    if (w <= 0.0f || h <= 0.0f) return;

    const int seg = round_arc_segments(radii);
    std::vector<RoundPt> inner;
    std::vector<RoundPt> outer;
    build_round_outline(x, y, w, h, radii, -kRoundAa, seg, inner);
    if (inner.size() < 3) return;
    build_round_outline(x, y, w, h, radii, kRoundAa, seg, outer);

    // 渐变按顶点 y 在 [y, y+h] 上插值（超出范围的顶点按端点夹取）
    const float inv_h = (h > 0.0f) ? (1.0f / h) : 0.0f;
    auto color_at = [&](float py) {
        float t = (py - y) * inv_h;
        t = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
        return Color(top.r + (bottom.r - top.r) * t,
                     top.g + (bottom.g - top.g) * t,
                     top.b + (bottom.b - top.b) * t,
                     top.a + (bottom.a - top.a) * t);
    };

    // 中心扇形（凸多边形，折线按序展开）
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const Color center_col = color_at(cy);
    const std::size_t n = inner.size();
    for (std::size_t i = 0; i < n; ++i) {
        const RoundPt& a = inner[i];
        const RoundPt& b = inner[(i + 1) % n];
        push(cx, cy, center_col);
        push(a.x, a.y, color_at(a.y));
        push(b.x, b.y, color_at(b.y));
    }

    // 边缘渐隐带
    if (outer.size() == n) {
        for (std::size_t i = 0; i < n; ++i) {
            const RoundPt& a = inner[i];
            const RoundPt& b = inner[(i + 1) % n];
            const RoundPt& oa = outer[i];
            const RoundPt& ob = outer[(i + 1) % n];
            const Color ca = color_at(a.y);
            const Color cb = color_at(b.y);
            const Color ta(ca.r, ca.g, ca.b, 0.0f);
            const Color tb(cb.r, cb.g, cb.b, 0.0f);
            push(a.x, a.y, ca);
            push(b.x, b.y, cb);
            push(ob.x, ob.y, tb);

            push(a.x, a.y, ca);
            push(ob.x, ob.y, tb);
            push(oa.x, oa.y, ta);
        }
    }
}

// 圆角描边，线宽向内生长（外轮廓尺寸不变）。
//
// 描边带的三个轮廓（全部以"框边界"为基准）：
//   edge  = 框边界本身（描边开始处，实心）
//   outer = 边界外 0.5px（AA 起点，alpha 0）
//   innr  = 边界内 thickness（描边结束处）
// 早先 edge 放在"边界内 0.5"、innr 放在"内 1.5"，1px 边就整条落在半像素位置
// 上：光栅化的 top-left 规则会把上/右两侧判成 2px、下/左判成 1px，同一圈边框
// 四个方向不等粗。贴着边界向内取整才有真正的 1px。
template <typename Push>
inline void emit_round_border(Push&& push, float x, float y, float w, float h,
                              const float radii[4], float thickness,
                              const Color& color) {
    if (w <= 0.0f || h <= 0.0f || thickness <= 0.0f) return;
    const float limit = (w < h ? w : h) * 0.5f;
    if (thickness > limit) thickness = limit;

    std::vector<RoundPt> edge;
    std::vector<RoundPt> outer;
    std::vector<RoundPt> innr;
    const int seg = round_arc_segments(radii);
    build_round_outline(x, y, w, h, radii, 0.0f, seg, edge);
    build_round_outline(x, y, w, h, radii, kRoundAa, seg, outer);
    build_round_outline(x, y, w, h, radii, -thickness, seg, innr);
    if (edge.size() < 3 || innr.size() != edge.size() || outer.size() != edge.size()) {
        return;
    }

    const Color clear(color.r, color.g, color.b, 0.0f);
    const std::size_t n = edge.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        // 外侧 AA 带
        push(edge[i].x, edge[i].y, color);
        push(outer[i].x, outer[i].y, clear);
        push(outer[j].x, outer[j].y, clear);

        push(edge[i].x, edge[i].y, color);
        push(outer[j].x, outer[j].y, clear);
        push(edge[j].x, edge[j].y, color);

        // 实心描边带
        push(edge[i].x, edge[i].y, color);
        push(edge[j].x, edge[j].y, color);
        push(innr[j].x, innr[j].y, color);

        push(edge[i].x, edge[i].y, color);
        push(innr[j].x, innr[j].y, color);
        push(innr[i].x, innr[i].y, color);
    }
}

// 圆角外阴影：把扩散半径切成同心环，alpha 按 (1 - d/spread)^2 衰减，看起来
// 接近高斯，但只用顶点色——不需要额外的模糊 pass，也不会为了阴影引入离屏
// 目标。color 的 alpha 是贴近矩形处的浓度。
template <typename Push>
inline void emit_round_shadow(Push&& push, float x, float y, float w, float h,
                              const float radii[4], float spread,
                              const Color& color) {
    if (w <= 0.0f || h <= 0.0f || spread <= 0.0f || color.a <= 0.0f) return;

    constexpr int kRings = 8;
    const int seg = round_arc_segments(radii);
    std::vector<RoundPt> outer;
    std::vector<RoundPt> inner;
    for (int i = 0; i < kRings; ++i) {
        const float t0 = static_cast<float>(i) / static_cast<float>(kRings);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(kRings);
        const float d0 = spread * t0;
        const float d1 = spread * t1;
        const float tmid = (t0 + t1) * 0.5f;
        const float falloff = 1.0f - tmid;
        const Color c(color.r, color.g, color.b, color.a * falloff * falloff);

        build_round_outline(x, y, w, h, radii, d0, seg, inner);
        build_round_outline(x, y, w, h, radii, d1, seg, outer);
        if (inner.size() < 3 || inner.size() != outer.size()) continue;

        const std::size_t n = inner.size();
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t j = (k + 1) % n;
            push(inner[k].x, inner[k].y, c);
            push(outer[k].x, outer[k].y, c);
            push(outer[j].x, outer[j].y, c);

            push(inner[k].x, inner[k].y, c);
            push(outer[j].x, outer[j].y, c);
            push(inner[j].x, inner[j].y, c);
        }
    }
}

} // namespace gryce_engine::render_detail
