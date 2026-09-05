#pragma once

// GryceEngineUtils::ui::style_preset.h — 样式预设系统
//
// 提供硬编码的 "Primary"、"Default"、"Danger" 三种预设样式。
// 预设包含背景色、悬停色、按下色、文字色、边框色、圆角和字体大小。
// 当 .uif 中 style="Primary" 时，自动应用对应预设的样式值到控件。

#include <string>

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

// 样式预设定义
struct UIStylePreset {
    Color background;          // 背景色
    Color background_hover;    // 悬停背景色
    Color background_pressed;  // 按下背景色
    Color color;               // 文字颜色
    Color border_color;        // 边框颜色
    float border_radius = 4.0f; // 圆角
    float font_size = 16.0f;   // 字体大小

    // 应用预设到控件的 Style 结构
    void apply_to(Style& style) const {
        style.background = background;
        style.background_hover = background_hover;
        style.background_pressed = background_pressed;
        style.color = color;
        style.border_color = border_color;
        style.border_radius = border_radius;
        style.font_size = font_size;
    }
};

// 预设注册表
class UIStylePresetSet {
public:
    UIStylePresetSet() = delete;

    // 获取指定名称的预设（不存在时返回 Default）
    static const UIStylePreset& get(const char* name);

    // 检查预设是否存在
    static bool has(const char* name);

    // 注册自定义预设
    static void register_preset(const char* name, const UIStylePreset& preset);

private:
    // 初始化内置预设（懒加载，只第一次调用时执行）
    static void init_defaults();
};

} // namespace GryceEngineUtils::ui