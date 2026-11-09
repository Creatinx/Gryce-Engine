#include "ui/ui_theme.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace gryce_engine::ui {

namespace {

constexpr std::size_t kColorCount =
    static_cast<std::size_t>(UIThemeColor::Count);
constexpr std::size_t kMetricCount =
    static_cast<std::size_t>(UIThemeMetric::Count);

struct ThemeData {
    UITheme value;
    uint32_t revision = 1;
};

ThemeData& data() {
    static ThemeData instance = [] {
        ThemeData result;
        const uint32_t colors[] = {
            0x11151DFF, 0x171D28FF, 0x202A38FF, 0x0C1017FF,
            0xF1F4F8FF, 0x9AA6B8FF, 0x354052FF, 0x4C9FFFFF,
            0x69B5FFFF, 0x347BC4FF, 0x294A70FF, 0x59C98AFF,
            0xE5B45FFF, 0xE46972FF, 0x05070ACC, 0x253041FF,
            0xFFFFFF14, 0xFFFFFF1F, 0xFFFFFF0A, 0xFFFFFF24,
            0x000000AA,
        };
        const float metrics[] = {
            8.0f, 6.0f, 26.0f, 28.0f, 1.0f, 14.0f, 12.0f, 20.0f,
            4.0f, 8.0f, 12.0f, 8.0f, 0.083f, 0.167f, 0.25f,
        };
        std::copy(std::begin(colors), std::end(colors), result.value.colors);
        std::copy(std::begin(metrics), std::end(metrics), result.value.metrics);
        return result;
    }();
    return instance;
}

const std::unordered_map<std::string, UIThemeColor>& color_names() {
    static const std::unordered_map<std::string, UIThemeColor> names = {
        {"window", UIThemeColor::Window},
        {"panel", UIThemeColor::Panel},
        {"panel_raised", UIThemeColor::PanelRaised},
        {"input", UIThemeColor::Input},
        {"text", UIThemeColor::Text},
        {"text_muted", UIThemeColor::TextMuted},
        {"border", UIThemeColor::Border},
        {"accent", UIThemeColor::Accent},
        {"accent_hover", UIThemeColor::AccentHover},
        {"accent_pressed", UIThemeColor::AccentPressed},
        {"selection", UIThemeColor::Selection},
        {"success", UIThemeColor::Success},
        {"warning", UIThemeColor::Warning},
        {"error", UIThemeColor::Error},
        {"overlay", UIThemeColor::Overlay},
        {"track", UIThemeColor::Track},
        {"control_fill", UIThemeColor::ControlFill},
        {"control_fill_hover", UIThemeColor::ControlFillHover},
        {"control_fill_pressed", UIThemeColor::ControlFillPressed},
        {"control_stroke", UIThemeColor::ControlStroke},
        {"shadow", UIThemeColor::Shadow},
    };
    return names;
}

const std::unordered_map<std::string, UIThemeMetric>& metric_names() {
    static const std::unordered_map<std::string, UIThemeMetric> names = {
        {"padding", UIThemeMetric::Padding},
        {"gap", UIThemeMetric::Gap},
        {"row_height", UIThemeMetric::RowHeight},
        {"control_height", UIThemeMetric::ControlHeight},
        {"border_width", UIThemeMetric::BorderWidth},
        {"font_size", UIThemeMetric::FontSize},
        {"small_font_size", UIThemeMetric::SmallFontSize},
        {"subtitle_font_size", UIThemeMetric::SubtitleFontSize},
        {"radius_small", UIThemeMetric::RadiusSmall},
        {"radius_medium", UIThemeMetric::RadiusMedium},
        {"radius_large", UIThemeMetric::RadiusLarge},
        {"elevation", UIThemeMetric::Elevation},
        {"motion_fast", UIThemeMetric::MotionFast},
        {"motion_normal", UIThemeMetric::MotionNormal},
        {"motion_slow", UIThemeMetric::MotionSlow},
    };
    return names;
}

uint32_t parse_hex_color(const std::string& source, bool* ok) {
    std::string text = source;
    if (!text.empty() && text.front() == '#') text.erase(text.begin());
    if (text.size() != 6 && text.size() != 8) {
        *ok = false;
        return 0;
    }
    for (char character : text) {
        if (!std::isxdigit(static_cast<unsigned char>(character))) {
            *ok = false;
            return 0;
        }
    }
    if (text.size() == 6) text += "FF";
    *ok = true;
    return static_cast<uint32_t>(std::stoul(text, nullptr, 16));
}

void bump_revision() {
    ++data().revision;
}

} // namespace

const UITheme& ui_theme() {
    return data().value;
}

uint32_t ui_theme_revision() {
    return data().revision;
}

uint32_t ui_theme_color(UIThemeColor color) {
    const auto index = static_cast<std::size_t>(color);
    return index < kColorCount ? data().value.colors[index] : 0;
}

float ui_theme_metric(UIThemeMetric metric) {
    const auto index = static_cast<std::size_t>(metric);
    return index < kMetricCount ? data().value.metrics[index] : 0.0f;
}

float ui_theme_radius(UIRadiusTier tier) {
    switch (tier) {
        case UIRadiusTier::Small: return ui_theme_metric(UIThemeMetric::RadiusSmall);
        case UIRadiusTier::Large: return ui_theme_metric(UIThemeMetric::RadiusLarge);
        case UIRadiusTier::Medium:
        default: return ui_theme_metric(UIThemeMetric::RadiusMedium);
    }
}

void set_ui_theme_color(UIThemeColor color, uint32_t rgba) {
    const auto index = static_cast<std::size_t>(color);
    if (index >= kColorCount) return;
    data().value.colors[index] = rgba;
    bump_revision();
}

void set_ui_theme_metric(UIThemeMetric metric, float value) {
    const auto index = static_cast<std::size_t>(metric);
    if (index >= kMetricCount) return;
    data().value.metrics[index] = std::max(0.0f, value);
    bump_revision();
}

void set_ui_theme_preset(UIThemePreset preset) {
    const uint32_t dark[] = {
        0x11151DFF, 0x171D28FF, 0x202A38FF, 0x0C1017FF,
        0xF1F4F8FF, 0x9AA6B8FF, 0x354052FF, 0x4C9FFFFF,
        0x69B5FFFF, 0x347BC4FF, 0x294A70FF, 0x59C98AFF,
        0xE5B45FFF, 0xE46972FF, 0x05070ACC, 0x253041FF,
        0xFFFFFF14, 0xFFFFFF1F, 0xFFFFFF0A, 0xFFFFFF24,
        0x000000AA,
    };
    // WinUI 3 浅色：页面 #F0F0F0、卡片纯白、按钮白 70% 叠在页面上再配 1px
    // 深边。浅色主题下"白底白按钮"全靠这圈边和卡片自己的边区分层次。
    const uint32_t light[] = {
        0xF0F0F0FF, 0xF5F5F5FF, 0xFFFFFFFF, 0xFFFFFFFF,
        0x1A1A1AFF, 0x5C5C5CFF, 0xD0D0D0FF, 0x0067C0FF,
        0x1975C5FF, 0x005BA4FF, 0x0067C033, 0x0F7B0FFF,
        0x9D5D00FF, 0xC42B1CFF, 0x00000055, 0x0000004D,
        0xFFFFFFB3, 0xFFFFFFFF, 0xE6E6E6E6, 0x00000018,
        0x0000001F,
    };
    const uint32_t contrast[] = {
        0x000000FF, 0x0A0A0AFF, 0x181818FF, 0x000000FF,
        0xFFFFFFFF, 0xC8C8C8FF, 0xE0E0E0FF, 0x00D8FFFF,
        0x60E7FFFF, 0x00A8C8FF, 0xFFFFFF44, 0x00FF90FF,
        0xFFD000FF, 0xFF5050FF, 0x000000AA, 0x222222FF,
        0x1A1A1AFF, 0x2A2A2AFF, 0x000000FF, 0xE0E0E0FF,
        0x00000000,
    };
    const uint32_t nord[] = {
        0x2E3440FF, 0x353C4AFF, 0x3B4252FF, 0x272C36FF,
        0xECEFF4FF, 0xA6B0C0FF, 0x4C566AFF, 0x88C0D0FF,
        0x9ED0DFFF, 0x5E81ACFF, 0x5E81ACFF, 0xA3BE8CFF,
        0xEBCB8BFF, 0xBF616AFF, 0x12151ACC, 0x434C5EFF,
        0xFFFFFF14, 0xFFFFFF1F, 0xFFFFFF0A, 0xFFFFFF24,
        0x00000099,
    };
    // WinUI 3 / Fluent 2 的深色默认值。控件填充与描边都写成**半透明白**，
    // 这样同一套按钮叠在 base/layer/card 三层底色上都自然提亮一档，不需要
    // 为每种背景额外配一组按钮色。
    const uint32_t fluent[] = {
        0x181818FF, 0x1C1C1CFF, 0x323232FF, 0x2A2A2AFF,
        0xFFFFFFFF, 0xA8A8A8FF, 0xFFFFFF1F, 0x0078D4FF,
        0x1A86D9FF, 0x0067B8FF, 0x0078D440, 0x6CCB5FFF,
        0xFCE100FF, 0xFF99A4FF, 0x00000099, 0xFFFFFF4D,
        0xFFFFFF0F, 0xFFFFFF17, 0xFFFFFF0A, 0xFFFFFF24,
        0x000000AA,
    };

    // 半径与动效跟随主题：高对比度要更方、更"硬"（Fluent 在对比度模式下也会
    // 收紧圆角并关掉投影），其余主题沿用 Fluent 的 4/8/12 阶梯。
    const float radius_dark[3] = {4.0f, 8.0f, 12.0f};
    const float radius_contrast[3] = {2.0f, 3.0f, 4.0f};
    const float* radii = radius_dark;
    float elevation = 8.0f;
    // 间距节奏：Fluent 用 4px 网格 + 32px 控件高。其余主题保持既有密度，
    // 免得换预设时整屏重新排一次。
    float padding = 8.0f;
    float gap = 6.0f;
    float row_height = 26.0f;
    float control_height = 28.0f;

    const uint32_t* source = dark;
    const char* name = "Dark";
    switch (preset) {
        case UIThemePreset::Light: source = light; name = "Light"; break;
        case UIThemePreset::HighContrast:
            source = contrast; name = "High Contrast";
            radii = radius_contrast; elevation = 0.0f;
            break;
        case UIThemePreset::Nord: source = nord; name = "Nord"; break;
        case UIThemePreset::Fluent:
            source = fluent; name = "Fluent";
            padding = 12.0f; gap = 8.0f;
            row_height = 32.0f; control_height = 32.0f;
            break;
        default: break;
    }
    std::copy(source, source + kColorCount, data().value.colors);
    auto set_metric = [](UIThemeMetric metric, float value) {
        data().value.metrics[static_cast<std::size_t>(metric)] = value;
    };
    set_metric(UIThemeMetric::RadiusSmall, radii[0]);
    set_metric(UIThemeMetric::RadiusMedium, radii[1]);
    set_metric(UIThemeMetric::RadiusLarge, radii[2]);
    set_metric(UIThemeMetric::Elevation, elevation);
    set_metric(UIThemeMetric::Padding, padding);
    set_metric(UIThemeMetric::Gap, gap);
    set_metric(UIThemeMetric::RowHeight, row_height);
    set_metric(UIThemeMetric::ControlHeight, control_height);
    data().value.name = name;
    bump_revision();
}

int load_ui_theme_json(const char* json_text) {
    if (!json_text || !*json_text) return 0;
    nlohmann::json json = nlohmann::json::parse(json_text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return 0;

    ThemeData& target = data();
    if (json.contains("name") && json["name"].is_string()) {
        static std::string theme_name;
        theme_name = json["name"].get<std::string>();
        target.value.name = theme_name.c_str();
    }
    if (json.contains("colors") && json["colors"].is_object()) {
        for (const auto& [name, color] : json["colors"].items()) {
            const auto it = color_names().find(name);
            if (it == color_names().end() || !color.is_string()) continue;
            bool ok = false;
            const uint32_t parsed = parse_hex_color(color.get<std::string>(), &ok);
            if (ok) {
                target.value.colors[static_cast<std::size_t>(it->second)] = parsed;
            }
        }
    }
    if (json.contains("metrics") && json["metrics"].is_object()) {
        for (const auto& [name, value] : json["metrics"].items()) {
            const auto it = metric_names().find(name);
            if (it == metric_names().end() || !value.is_number()) continue;
            target.value.metrics[static_cast<std::size_t>(it->second)] =
                std::max(0.0f, value.get<float>());
        }
    }
    bump_revision();
    return 1;
}

int load_ui_theme_file(const char* path) {
    if (!path || !*path) return 0;
    std::ifstream input(path, std::ios::binary);
    if (!input) return 0;
    std::string text((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    return load_ui_theme_json(text.c_str());
}

uint32_t ui_theme_rgba(float r, float g, float b, float a) {
    auto channel = [](float value) {
        return static_cast<uint32_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (channel(r) << 24u) | (channel(g) << 16u) |
           (channel(b) << 8u) | channel(a);
}

uint32_t ui_theme_mix(uint32_t from, uint32_t to, float amount) {
    const float t = std::clamp(amount, 0.0f, 1.0f);
    auto mix_channel = [t](uint32_t a, uint32_t b) {
        return static_cast<uint32_t>(static_cast<float>(a) * (1.0f - t) +
                                     static_cast<float>(b) * t + 0.5f);
    };
    return (mix_channel(from >> 24u, to >> 24u) << 24u) |
           (mix_channel((from >> 16u) & 0xFFu, (to >> 16u) & 0xFFu) << 16u) |
           (mix_channel((from >> 8u) & 0xFFu, (to >> 8u) & 0xFFu) << 8u) |
           mix_channel(from & 0xFFu, to & 0xFFu);
}

} // namespace gryce_engine::ui
