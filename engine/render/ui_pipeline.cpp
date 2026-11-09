#include "render/ui_pipeline.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "render/render2d.h"
#include "render/rhi_handle.h"
#include "ui/ui_drawlist.h"

namespace gryce_engine::render {

namespace {

Color unpack_color(uint32_t rgba) {
    constexpr float inv = 1.0f / 255.0f;
    return Color(((rgba >> 24u) & 0xffu) * inv,
                 ((rgba >> 16u) & 0xffu) * inv,
                 ((rgba >> 8u) & 0xffu) * inv,
                 (rgba & 0xffu) * inv);
}

} // namespace

void render_ui_draw_list(IRenderer2D& renderer, const ui::UIDrawList& draw_list,
                         int screen_width, int screen_height) {
    if (screen_width <= 0 || screen_height <= 0 || draw_list.empty()) return;

    // 字体抗锯齿：图集是按档烘焙的位图，绘制时取**最近的一档**。缺档就只能
    // 缩放，缩放比一旦偏离 1:1，笔画边缘就是双线性插值出来的灰边（缩小还会
    // 欠采样出台阶）。所以这里把本帧真正用到的每种字号都报给后端，让它一次
    // 补齐；has_size 的 12% 容差会把 12/13/14 这类邻近字号合并，不会抖动。
    std::vector<std::pair<float, int>> size_hits;
    for (const ui::UIDrawCommand& command : draw_list.commands()) {
        if (command.type != ui::UIDrawCommandType::Text &&
            command.type != ui::UIDrawCommandType::TextColored) {
            continue;
        }
        if (command.font_size <= 0.0f) continue;
        const float q = std::round(command.font_size);
        bool merged = false;
        for (auto& hit : size_hits) {
            if (std::fabs(hit.first - q) < 0.5f) {
                ++hit.second;
                merged = true;
                break;
            }
        }
        if (!merged) size_hits.emplace_back(q, 1);
    }
    if (!size_hits.empty()) {
        // 图集档位有上限，按使用次数保留最常用的几种字号。
        std::sort(size_hits.begin(), size_hits.end(),
                  [](const std::pair<float, int>& a, const std::pair<float, int>& b) {
                      return a.second > b.second;
                  });
        if (size_hits.size() > 8) size_hits.resize(8);
        std::vector<float> sizes;
        sizes.reserve(size_hits.size());
        // 覆盖层按 overlay_scale 倍渲染时，字号也要乘上去：图集烘出的是**离屏
        // 目标上的像素尺寸**，这样降采样时才是真正的多采样，而不是把逻辑字号
        // 的字形放大一倍再缩回来。
        const float overlay_scale = renderer.overlay_scale();
        for (const auto& hit : size_hits) sizes.push_back(hit.first * overlay_scale);
        renderer.ensure_text_raster_sizes(sizes);
    }

    // 覆盖层目标与字体档位都就绪之后才进入绘制：overlay_scale 为 2 时图集
    // 已经按 2x 物理像素烘好，中途不会再重建纹理。
    renderer.begin_overlay_pass(screen_width, screen_height);
    std::vector<ui::UIRect> clips;

    // 基线由垂直对齐与**大写高**共同决定：把 font_size 当基线偏移会让文字在
    // 控件里整体偏上（字的下伸部与行距被算进了视觉重心）。Middle 让大写字母
    // 的顶边与底边到 rect 上下边的距离相等，才是"看起来居中"。
    auto baseline_of = [&renderer](const ui::UIDrawCommand& command) -> float {
        const float cap = renderer.text_cap_height(command.font_size);
        switch (command.valign) {
            case ui::UITextVAlign::Top:
                return command.rect.y + cap;
            case ui::UITextVAlign::Middle:
                return command.rect.y + (command.rect.h + cap) * 0.5f;
            case ui::UITextVAlign::Baseline:
            default:
                return command.rect.y + command.font_size;
        }
    };

    const std::vector<ui::UIDrawCommand>& commands = draw_list.commands();
    const std::size_t layer_split =
        std::min(draw_list.top_layer_start(), commands.size());
    for (std::size_t command_index = 0; command_index < commands.size(); ++command_index) {
        if (layer_split != 0 && command_index == layer_split) {
            // 置顶层（菜单浮层、对话框）之前把页面这一层提交掉：文字批次天然
            // 排在矩形之后，不切的话浮层底色会被页面文字盖住。
            renderer.flush_pending();
        }
        const ui::UIDrawCommand& command = commands[command_index];
        switch (command.type) {
            case ui::UIDrawCommandType::Rect: {
                // 直角且无描边/渐变时走原来的快路径：不展开任何圆角三角形。
                if (!command.rounded() && command.border_width <= 0.0f &&
                    command.color_bottom == 0u) {
                    renderer.draw_rect(command.rect.x, command.rect.y,
                                       command.rect.w, command.rect.h,
                                       unpack_color(command.color));
                    break;
                }
                if (command.color_bottom != 0u) {
                    renderer.draw_round_rect_gradient(
                        command.rect.x, command.rect.y,
                        command.rect.w, command.rect.h, command.radii,
                        unpack_color(command.color),
                        unpack_color(command.color_bottom));
                } else {
                    renderer.draw_round_rect(command.rect.x, command.rect.y,
                                             command.rect.w, command.rect.h,
                                             command.radii,
                                             unpack_color(command.color));
                }
                if (command.border_width > 0.0f) {
                    renderer.draw_round_rect_border(
                        command.rect.x, command.rect.y,
                        command.rect.w, command.rect.h, command.radii,
                        command.border_width,
                        unpack_color(command.border_color));
                }
                break;
            }
            case ui::UIDrawCommandType::Shadow:
                renderer.draw_round_shadow(command.rect.x, command.rect.y,
                                           command.rect.w, command.rect.h,
                                           command.radii, command.spread,
                                           unpack_color(command.color));
                break;
            case ui::UIDrawCommandType::Text: {
                // 对齐需要字形总宽：居中/右对齐的按钮文字靠这里算偏移，
                // 否则所有文字都从 rect 左缘起笔，控件看起来像"贴纸"。
                float text_x = command.rect.x;
                if (command.align != ui::UITextAlign::Left) {
                    const float width =
                        renderer.measure_text(command.text.c_str(), command.font_size);
                    if (command.align == ui::UITextAlign::Center) {
                        text_x += (command.rect.w - width) * 0.5f;
                    } else {
                        text_x += command.rect.w - width;
                    }
                }
                renderer.draw_text(text_x,
                                   baseline_of(command),
                                   command.text, command.font_size,
                                   unpack_color(command.color));
                break;
            }
            case ui::UIDrawCommandType::TextColored: {
                std::vector<ColoredRun> runs;
                runs.reserve(command.runs.size());
                float total_width = 0.0f;
                for (const ui::UITextRun& run : command.runs) {
                    ColoredRun colored;
                    colored.text = run.text;
                    colored.color = unpack_color(run.color);
                    if (command.align != ui::UITextAlign::Left) {
                        total_width += renderer.measure_text(run.text.c_str(),
                                                             command.font_size);
                    }
                    runs.push_back(std::move(colored));
                }
                if (!runs.empty()) {
                    float text_x = command.rect.x;
                    if (command.align == ui::UITextAlign::Center) {
                        text_x += (command.rect.w - total_width) * 0.5f;
                    } else if (command.align == ui::UITextAlign::Right) {
                        text_x += command.rect.w - total_width;
                    }
                    renderer.draw_text_colored(text_x,
                                               baseline_of(command),
                                               command.font_size, runs);
                }
                break;
            }
            case ui::UIDrawCommandType::Line: {
                // 旋转四边形 + 两端圆点。厚度小于 1 像素时按 1 像素画，
                // 否则细笔画会在降采样里变成虚线。
                const float dx = command.x1 - command.x0;
                const float dy = command.y1 - command.y0;
                const float length = std::sqrt(dx * dx + dy * dy);
                const float half = std::max(0.5f, command.thickness * 0.5f);
                if (length < 1e-4f || half <= 0.0f) break;
                const float nx = -dy / length * half;
                const float ny = dx / length * half;
                std::vector<math::Vector2f> quad;
                quad.reserve(4);
                quad.push_back({command.x0 + nx, command.y0 + ny});
                quad.push_back({command.x1 + nx, command.y1 + ny});
                quad.push_back({command.x1 - nx, command.y1 - ny});
                quad.push_back({command.x0 - nx, command.y0 - ny});
                const Color stroke = unpack_color(command.color);
                renderer.draw_polygon(quad, stroke);
                renderer.draw_circle(command.x0, command.y0, half, 10, stroke);
                renderer.draw_circle(command.x1, command.y1, half, 10, stroke);
                break;
            }
            case ui::UIDrawCommandType::Image: {
                RHITextureHandle texture;
                texture.index = command.texture_index;
                texture.generation = command.texture_generation;
                renderer.draw_sprite_region(command.rect.x, command.rect.y,
                                            command.rect.w, command.rect.h,
                                            command.u0, command.v0,
                                            command.u1, command.v1,
                                            texture, unpack_color(command.color));
                break;
            }
            case ui::UIDrawCommandType::PushClip: {
                clips.push_back(command.rect);
                const auto x = static_cast<int>(std::floor(command.rect.x));
                const auto y = static_cast<int>(std::floor(command.rect.y));
                const auto w = std::max(0, static_cast<int>(std::ceil(command.rect.w)));
                const auto h = std::max(0, static_cast<int>(std::ceil(command.rect.h)));
                renderer.set_scissor(x, y, w, h);
                break;
            }
            case ui::UIDrawCommandType::PopClip:
                if (!clips.empty()) clips.pop_back();
                if (clips.empty()) {
                    renderer.reset_scissor();
                } else {
                    const ui::UIRect& clip = clips.back();
                    renderer.set_scissor(static_cast<int>(std::floor(clip.x)),
                                         static_cast<int>(std::floor(clip.y)),
                                         std::max(0, static_cast<int>(std::ceil(clip.w))),
                                         std::max(0, static_cast<int>(std::ceil(clip.h))));
                }
                break;
        }
    }

    renderer.reset_scissor();
    renderer.end_overlay_pass();
}

} // namespace gryce_engine::render
