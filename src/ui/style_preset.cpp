#include "GryceEngineUtils/ui/style_preset.h"

#include <cstring>
#include <unordered_map>

namespace GryceEngineUtils::ui {

// ============================================================================
// 内置预设定义
// ============================================================================
namespace {

// 辅助创建 Color（0~255 → 0~1）
inline Color rgba(int r, int g, int b, int a = 255) {
    return Color(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

// 内置预设表
std::unordered_map<std::string, UIStylePreset> s_presets;
bool s_initialized = false;

void ensure_defaults() {
    if (s_initialized) return;
    s_initialized = true;

    // Primary — 蓝色主题（主要操作按钮）
    s_presets["Primary"] = UIStylePreset{
        .background        = rgba(0, 102, 255),
        .background_hover  = rgba(51, 136, 255),
        .background_pressed = rgba(0, 85, 221),
        .color             = Color::white(),
        .border_color      = rgba(0, 90, 230),
        .border_radius     = 4.0f,
        .font_size         = 16.0f,
    };

    // Default — 灰色主题（标准按钮）
    s_presets["Default"] = UIStylePreset{
        .background        = rgba(51, 51, 51),
        .background_hover  = rgba(68, 68, 68),
        .background_pressed = rgba(34, 34, 34),
        .color             = Color::white(),
        .border_color      = rgba(85, 85, 85),
        .border_radius     = 4.0f,
        .font_size         = 16.0f,
    };

    // Danger — 红色主题（危险操作按钮）
    s_presets["Danger"] = UIStylePreset{
        .background        = rgba(204, 51, 51),
        .background_hover  = rgba(221, 68, 68),
        .background_pressed = rgba(187, 34, 34),
        .color             = Color::white(),
        .border_color      = rgba(180, 40, 40),
        .border_radius     = 4.0f,
        .font_size         = 16.0f,
    };
}

} // anonymous namespace

// ============================================================================
// 静态接口
// ============================================================================
void UIStylePresetSet::init_defaults() {
    ensure_defaults();
}

const UIStylePreset& UIStylePresetSet::get(const char* name) {
    ensure_defaults();
    if (name) {
        auto it = s_presets.find(name);
        if (it != s_presets.end()) {
            return it->second;
        }
    }
    // 不存在时返回 Default
    static const UIStylePreset s_default{
        .background        = rgba(51, 51, 51),
        .background_hover  = rgba(68, 68, 68),
        .background_pressed = rgba(34, 34, 34),
        .color             = Color::white(),
        .border_color      = rgba(85, 85, 85),
        .border_radius     = 4.0f,
        .font_size         = 16.0f,
    };
    return s_default;
}

bool UIStylePresetSet::has(const char* name) {
    ensure_defaults();
    return name && s_presets.find(name) != s_presets.end();
}

void UIStylePresetSet::register_preset(const char* name, const UIStylePreset& preset) {
    if (name) {
        s_presets[name] = preset;
    }
}

} // namespace GryceEngineUtils::ui