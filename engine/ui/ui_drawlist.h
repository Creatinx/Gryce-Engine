#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ui/ui_geometry.h"

namespace gryce_engine::ui {

struct TextureRef {
    uint32_t index = 0xFFFFFFFFu;
    uint32_t generation = 0;
    int width = 0;
    int height = 0;

    bool valid() const { return index != 0xFFFFFFFFu && width > 0 && height > 0; }
};

enum class UIDrawCommandType : uint8_t {
    Rect = 0,
    Text = 1,
    PushClip = 2,
    PopClip = 3,
    Image = 4,
    // 一行多色文本：runs 里的每一段按顺序横向推进行（语法高亮用）。
    // 与 Text 的区别只在颜色粒度——Text 整行一个颜色，TextColored 每 run 一个颜色。
    TextColored = 5,
    // 圆角外阴影：rect 是投影体本身，spread 是扩散半径。
    Shadow = 6,
    // 圆头线段：起点/终点 + 线宽。Fluent 的勾选标记、下拉箭头、菜单分隔线
    // 都是这种"短圆头笔画"，用旋转四边形 + 两端圆点拼出来。
    Line = 7,
};

// 文本在 rect 内的水平对齐方式。
enum class UITextAlign : uint8_t {
    Left = 0,
    Center,
    Right,
};

// 文本在 rect 内的垂直对齐方式。控件里的单行文字默认 Middle（真正按大写高
// 居中）；逐行排版的正文（代码编辑器、控制台）用 Baseline 保持行距节奏。
enum class UITextVAlign : uint8_t {
    // 大写字母顶边贴 rect 顶。
    Top = 0,
    // 大写字母在 rect 内垂直居中——控件标签用这个。
    Middle,
    // 基线固定在 rect.y + font_size，保留逐行排版的老口径。
    Baseline,
};

// 一段同色文本。文本模型层不依赖后端 Color，直接用 0xRRGGBBAA。
struct UITextRun {
    std::string text;
    uint32_t color = 0xFFFFFFFFu;
};

// 稳定的模型层命令。颜色使用 0xRRGGBBAA，不携带 GPU 句柄或后端对象。
struct UIDrawCommand {
    UIDrawCommandType type = UIDrawCommandType::Rect;
    UIRect rect;
    uint32_t color = 0xFFFFFFFFu;
    float font_size = 0.0f;
    std::string text;
    uint32_t texture_index = 0xFFFFFFFFu;
    uint32_t texture_generation = 0;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
    std::vector<UITextRun> runs;   // 仅 TextColored 使用

    // ---- 圆角 / 描边 / 阴影（Rect 与 Shadow 使用）----------------------
    // 半径顺序固定为 左上、右上、右下、左下，单位逻辑像素。全 0 等价于直角，
    // 此时后端走原来的矩形路径，不为圆角多展开任何三角形。
    float radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // 向内生长的描边；border_width 为 0 时不描边。
    float border_width = 0.0f;
    uint32_t border_color = 0x00000000u;
    // 竖直渐变的底色。0 表示纯色（用 color），非 0 时由 color 渐变到它。
    uint32_t color_bottom = 0x00000000u;
    // Shadow 的扩散半径。
    float spread = 0.0f;

    // ---- 文本对齐（Text / TextColored）--------------------------------
    UITextAlign align = UITextAlign::Left;
    UITextVAlign valign = UITextVAlign::Middle;

    // ---- 线段（Line）---------------------------------------------------
    // 起点与终点为绝对坐标，thickness 是笔画宽度，两端按半宽做成圆头。
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float thickness = 0.0f;

    bool rounded() const {
        return radii[0] > 0.0f || radii[1] > 0.0f ||
               radii[2] > 0.0f || radii[3] > 0.0f;
    }
};

class UIDrawList {
public:
    void reset() {
        commands_.clear();
        top_layer_start_ = 0;
    }

    // 置顶层（菜单浮层、对话框）在命令流里的起点。0 表示没有置顶层。
    // 后端要在这一条之前把已经积累的批次提交掉：文字批次天然排在矩形之后，
    // 同一趟里浮层的底色会被页面文字盖住。
    void set_top_layer_start(std::size_t index) { top_layer_start_ = index; }
    std::size_t top_layer_start() const { return top_layer_start_; }

    void add_rect(const UIRect& rect, uint32_t rgba) {
        commands_.push_back({UIDrawCommandType::Rect, rect, rgba});
    }

    // 圆角填充（四角同半径）。radius 为 0 时退化成直角。
    void add_round_rect(const UIRect& rect, float radius, uint32_t rgba) {
        const float radii[4] = {radius, radius, radius, radius};
        add_round_rect(rect, radii, rgba);
    }

    // 圆角填充，可带竖直渐变（bottom_rgba 非 0）与内描边（border_width > 0）。
    // 渐变与描边都靠顶点色/额外环带完成，不需要新的着色器或纹理。
    void add_round_rect(const UIRect& rect, const float radii_in[4], uint32_t rgba,
                        uint32_t bottom_rgba = 0u,
                        float border_width = 0.0f,
                        uint32_t border_rgba = 0x00000000u) {
        UIDrawCommand command;
        command.type = UIDrawCommandType::Rect;
        command.rect = rect;
        command.color = rgba;
        for (int i = 0; i < 4; ++i) {
            command.radii[i] = std::max(0.0f, radii_in[i]);
        }
        command.color_bottom = bottom_rgba;
        command.border_width = std::max(0.0f, border_width);
        command.border_color = border_rgba;
        commands_.push_back(std::move(command));
    }

    // 四角同半径的带描边/渐变版本。
    void add_round_rect(const UIRect& rect, float radius, uint32_t rgba,
                        uint32_t bottom_rgba, float border_width,
                        uint32_t border_rgba) {
        const float radii[4] = {radius, radius, radius, radius};
        add_round_rect(rect, radii, rgba, bottom_rgba, border_width, border_rgba);
    }

    // 圆角外阴影。必须画在投影体之前，调用方负责顺序。
    void add_round_shadow(const UIRect& rect, float radius, float spread,
                          uint32_t rgba) {
        UIDrawCommand command;
        command.type = UIDrawCommandType::Shadow;
        command.rect = rect;
        command.color = rgba;
        for (int i = 0; i < 4; ++i) command.radii[i] = std::max(0.0f, radius);
        command.spread = std::max(0.0f, spread);
        commands_.push_back(std::move(command));
    }

    void add_text(const UIRect& rect, std::string text, float font_size, uint32_t rgba,
                  UITextAlign align = UITextAlign::Left,
                  UITextVAlign valign = UITextVAlign::Middle) {
        UIDrawCommand command;
        command.type = UIDrawCommandType::Text;
        command.rect = rect;
        command.color = rgba;
        command.font_size = font_size;
        command.align = align;
        command.valign = valign;
        command.text = std::move(text);
        commands_.push_back(std::move(command));
    }

    void add_text_colored(const UIRect& rect, std::vector<UITextRun> runs, float font_size,
                          UITextAlign align = UITextAlign::Left,
                          UITextVAlign valign = UITextVAlign::Middle) {
        UIDrawCommand command;
        command.type = UIDrawCommandType::TextColored;
        command.rect = rect;
        command.font_size = font_size;
        command.align = align;
        command.valign = valign;
        command.runs = std::move(runs);
        commands_.push_back(std::move(command));
    }

    // 圆头线段。端点按 thickness 的一半各补一个圆，短笔画（勾选标记、箭头）
    // 才不会出现方角。
    void add_line(float x0, float y0, float x1, float y1, float thickness, uint32_t rgba) {
        UIDrawCommand command;
        command.type = UIDrawCommandType::Line;
        command.x0 = x0;
        command.y0 = y0;
        command.x1 = x1;
        command.y1 = y1;
        command.thickness = thickness;
        command.color = rgba;
        commands_.push_back(std::move(command));
    }

    void push_clip(const UIRect& rect) {
        commands_.push_back({UIDrawCommandType::PushClip, rect});
    }

    void add_image(const UIRect& rect, uint32_t texture_index,
                   uint32_t texture_generation,
                   float u0 = 0.0f, float v0 = 0.0f,
                   float u1 = 1.0f, float v1 = 1.0f,
                   uint32_t rgba = 0xFFFFFFFFu) {
        UIDrawCommand command;
        command.type = UIDrawCommandType::Image;
        command.rect = rect;
        command.color = rgba;
        command.texture_index = texture_index;
        command.texture_generation = texture_generation;
        command.u0 = u0;
        command.v0 = v0;
        command.u1 = u1;
        command.v1 = v1;
        commands_.push_back(std::move(command));
    }

    void pop_clip() {
        commands_.push_back({UIDrawCommandType::PopClip});
    }

    void append(const UIDrawList& other, float opacity = 1.0f) {
        const float alpha_scale = std::clamp(opacity, 0.0f, 1.0f);
        auto fade = [alpha_scale](uint32_t rgba) -> uint32_t {
            const uint32_t alpha = rgba & 0xFFu;
            const uint32_t scaled = static_cast<uint32_t>(
                static_cast<float>(alpha) * alpha_scale + 0.5f);
            return (rgba & 0xFFFFFF00u) | scaled;
        };
        for (UIDrawCommand command : other.commands_) {
            if (command.type != UIDrawCommandType::PushClip &&
                command.type != UIDrawCommandType::PopClip &&
                alpha_scale < 1.0f) {
                command.color = fade(command.color);
                // 描边与渐变底色同样是 0xRRGGBBAA，透明度必须一起乘，
                // 否则整体淡出时描边会残留成实线。
                if ((command.border_color & 0xFFu) != 0u) {
                    command.border_color = fade(command.border_color);
                }
                if (command.color_bottom != 0u) {
                    command.color_bottom = fade(command.color_bottom);
                }
                for (UITextRun& run : command.runs) {
                    run.color = fade(run.color);
                }
            }
            commands_.push_back(std::move(command));
        }
    }

    bool empty() const { return commands_.empty(); }
    std::size_t size() const { return commands_.size(); }
    const std::vector<UIDrawCommand>& commands() const { return commands_; }

private:
    std::vector<UIDrawCommand> commands_;
    std::size_t top_layer_start_ = 0;
};

inline uint32_t ui_color(float r, float g, float b, float a = 1.0f) {
    auto channel = [](float value) -> uint32_t {
        const float clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
    };
    return (channel(r) << 24u) | (channel(g) << 16u) |
           (channel(b) << 8u) | channel(a);
}

} // namespace gryce_engine::ui
