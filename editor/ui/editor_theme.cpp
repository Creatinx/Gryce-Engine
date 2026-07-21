#include "editor_theme.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

#include <nlohmann/json.hpp>

#include "../localization/localization.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::editor {

namespace {

// ---------------------------------------------------------------------------
// HSL / RGB 转换：用于从单一 hue 生成整套强调色梯度
// ---------------------------------------------------------------------------
struct Rgb { float r, g, b; };

// 把 overlay（含 alpha）按 alpha 混合到 base 上，避免直接相加导致黑/白纯色。
ImVec4 blend_overlay(ImVec4 base, ImVec4 overlay) {
    const float a = std::clamp(overlay.w, 0.0f, 1.0f);
    return ImVec4(base.x + (overlay.x - base.x) * a,
                  base.y + (overlay.y - base.y) * a,
                  base.z + (overlay.z - base.z) * a,
                  base.w);
}

Rgb hsl_to_rgb(float h, float s, float l) {
    float c = (1.0f - std::abs(2.0f * l - 1.0f)) * s;
    float hp = h * 6.0f;
    float x = c * (1.0f - std::abs(std::fmod(hp, 2.0f) - 1.0f));
    Rgb rgb{0.0f, 0.0f, 0.0f};
    if (hp < 1.0f)       rgb = {c, x, 0.0f};
    else if (hp < 2.0f)  rgb = {x, c, 0.0f};
    else if (hp < 3.0f)  rgb = {0.0f, c, x};
    else if (hp < 4.0f)  rgb = {0.0f, x, c};
    else if (hp < 5.0f)  rgb = {x, 0.0f, c};
    else                 rgb = {c, 0.0f, x};
    float m = l - 0.5f * c;
    return {rgb.r + m, rgb.g + m, rgb.b + m};
}

ImVec4 accent_color(float hue, float s, float l, float a = 1.0f) {
    Rgb c = hsl_to_rgb(hue, s, l);
    return ImVec4(c.r, c.g, c.b, a);
}

// ---------------------------------------------------------------------------
// Fluent Design 风格常量
// ---------------------------------------------------------------------------
struct FluentPalette {
    ImVec4 bg_layer;
    ImVec4 surface;
    ImVec4 card;
    ImVec4 border;
    ImVec4 text_primary;
    ImVec4 text_secondary;
    ImVec4 text_disabled;
    ImVec4 hover;
    ImVec4 pressed;
    ImVec4 subtle;
};

FluentPalette make_dark_palette() {
    return {
        ImVec4(0.125f, 0.125f, 0.125f, 1.00f), // bg_layer   #202020
        ImVec4(0.176f, 0.176f, 0.176f, 1.00f), // surface    #2D2D2D
        ImVec4(0.196f, 0.196f, 0.196f, 1.00f), // card       #323232
        ImVec4(0.275f, 0.275f, 0.275f, 0.80f), // border     #464646
        ImVec4(1.000f, 1.000f, 1.000f, 1.00f), // text_primary
        ImVec4(0.690f, 0.690f, 0.690f, 1.00f), // text_secondary
        ImVec4(0.450f, 0.450f, 0.450f, 1.00f), // text_disabled
        ImVec4(1.000f, 1.000f, 1.000f, 0.06f), // hover
        ImVec4(1.000f, 1.000f, 1.000f, 0.10f), // pressed
        ImVec4(1.000f, 1.000f, 1.000f, 0.04f), // subtle
    };
}

FluentPalette make_light_palette() {
    return {
        ImVec4(0.953f, 0.953f, 0.953f, 1.00f), // bg_layer   #F3F3F3
        ImVec4(1.000f, 1.000f, 1.000f, 1.00f), // surface    #FFFFFF
        ImVec4(0.980f, 0.980f, 0.980f, 1.00f), // card       #FAFAFA
        ImVec4(0.878f, 0.878f, 0.878f, 0.80f), // border     #E0E0E0
        ImVec4(0.102f, 0.102f, 0.102f, 1.00f), // text_primary
        ImVec4(0.380f, 0.380f, 0.380f, 1.00f), // text_secondary
        ImVec4(0.600f, 0.600f, 0.600f, 1.00f), // text_disabled
        ImVec4(0.000f, 0.000f, 0.000f, 0.04f), // hover
        ImVec4(0.000f, 0.000f, 0.000f, 0.07f), // pressed
        ImVec4(0.000f, 0.000f, 0.000f, 0.02f), // subtle
    };
}

void set_common_style(ImGuiStyle& style, const ThemeConfig& config) {
    style.WindowPadding     = ImVec2(10.0f, 8.0f);
    style.FramePadding      = ImVec2(8.0f, 5.0f);
    style.ItemSpacing       = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    style.CellPadding       = ImVec2(6.0f, 4.0f);
    style.TouchExtraPadding = ImVec2(0.0f, 0.0f);
    style.IndentSpacing     = 16.0f; // Unity 风格层级缩进
    style.ScrollbarSize     = 16.0f;
    style.GrabMinSize       = 12.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.TabBorderSize     = 1.0f;

    const float r = std::max(0.0f, config.rounding);
    style.WindowRounding    = r;
    style.ChildRounding     = r;
    style.FrameRounding     = r;
    style.PopupRounding     = r;
    style.ScrollbarRounding = r;
    style.GrabRounding      = r;
    style.TabRounding       = r;
    style.TabBarBorderSize  = 1.0f;
}

void apply_palette(ImGuiStyle& style, const FluentPalette& p, float hue, bool shadow) {
    ImVec4* c = style.Colors;

    ImVec4 accent      = accent_color(hue, 1.00f, 0.43f);
    ImVec4 accent_light= accent_color(hue, 0.85f, 0.55f);
    ImVec4 accent_dark = accent_color(hue, 1.00f, 0.35f);
    ImVec4 accent_sub  = accent_color(hue, 0.70f, 0.50f, 0.15f);

    c[ImGuiCol_Text]                 = p.text_primary;
    c[ImGuiCol_TextDisabled]         = p.text_disabled;
    c[ImGuiCol_WindowBg]             = ImVec4(p.bg_layer.x, p.bg_layer.y, p.bg_layer.z, 0.98f);
    c[ImGuiCol_ChildBg]              = p.surface;
    c[ImGuiCol_PopupBg]              = ImVec4(p.surface.x, p.surface.y, p.surface.z, 0.98f);
    c[ImGuiCol_Border]               = p.border;
    c[ImGuiCol_BorderShadow]         = shadow ? ImVec4(0.0f, 0.0f, 0.0f, 0.10f) : ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    c[ImGuiCol_FrameBg]              = p.card;
    c[ImGuiCol_FrameBgHovered]       = blend_overlay(p.card, p.hover);
    c[ImGuiCol_FrameBgActive]        = blend_overlay(p.card, p.pressed);

    c[ImGuiCol_TitleBg]              = p.surface;
    c[ImGuiCol_TitleBgActive]        = p.card;
    c[ImGuiCol_TitleBgCollapsed]     = p.surface;
    c[ImGuiCol_MenuBarBg]            = p.surface;

    c[ImGuiCol_ScrollbarBg]          = p.bg_layer;
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(p.text_secondary.x, p.text_secondary.y, p.text_secondary.z, 0.55f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(p.text_secondary.x, p.text_secondary.y, p.text_secondary.z, 0.75f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(p.text_secondary.x, p.text_secondary.y, p.text_secondary.z, 0.90f);

    c[ImGuiCol_CheckMark]            = accent;
    c[ImGuiCol_SliderGrab]           = accent;
    c[ImGuiCol_SliderGrabActive]     = accent_light;

    c[ImGuiCol_Button]               = accent;
    c[ImGuiCol_ButtonHovered]        = accent_light;
    c[ImGuiCol_ButtonActive]         = accent_dark;

    c[ImGuiCol_Header]               = accent_sub;
    c[ImGuiCol_HeaderHovered]        = blend_overlay(accent_sub, p.hover);
    c[ImGuiCol_HeaderActive]         = ImVec4(accent.x, accent.y, accent.z, 0.35f);

    c[ImGuiCol_Separator]            = p.border;
    c[ImGuiCol_SeparatorHovered]     = accent;
    c[ImGuiCol_SeparatorActive]      = accent_light;

    c[ImGuiCol_ResizeGrip]           = accent_color(hue, 0.4f, 0.5f, 0.25f);
    c[ImGuiCol_ResizeGripHovered]    = accent_color(hue, 0.5f, 0.55f, 0.45f);
    c[ImGuiCol_ResizeGripActive]     = accent;

    c[ImGuiCol_Tab]                  = p.surface;
    c[ImGuiCol_TabHovered]           = accent_light;
    c[ImGuiCol_TabActive]            = accent;
    c[ImGuiCol_TabUnfocused]         = p.surface;
    c[ImGuiCol_TabUnfocusedActive]   = p.card;

    c[ImGuiCol_DockingPreview]       = accent_color(hue, 0.8f, 0.55f, 0.55f);
    c[ImGuiCol_DockingEmptyBg]       = p.bg_layer;

    c[ImGuiCol_PlotLines]            = accent;
    c[ImGuiCol_PlotLinesHovered]     = accent_light;
    c[ImGuiCol_PlotHistogram]        = accent;
    c[ImGuiCol_PlotHistogramHovered] = accent_light;

    c[ImGuiCol_TableHeaderBg]        = p.card;
    c[ImGuiCol_TableBorderStrong]    = p.border;
    c[ImGuiCol_TableBorderLight]     = p.border;
    c[ImGuiCol_TableRowBg]           = p.surface;
    c[ImGuiCol_TableRowBgAlt]        = p.card;

    c[ImGuiCol_TextSelectedBg]       = accent_color(hue, 0.6f, 0.5f, 0.40f);
    c[ImGuiCol_DragDropTarget]       = accent;
    c[ImGuiCol_NavHighlight]         = accent;
    c[ImGuiCol_NavWindowingHighlight]= accent;
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.0f, 0.0f, 0.0f, 0.45f);
}

std::string default_font_path() {
    // 优先项目内置字体
    try {
        std::string bundled = resources::ResourcePath::resolve("res:/fonts/Roboto-Medium.ttf");
        if (!bundled.empty() && std::filesystem::exists(bundled)) return bundled;
    } catch (...) {}
    std::string fallback = resources::Project::instance().root() + "/editor/project/fonts/Roboto-Medium.ttf";
    if (std::filesystem::exists(fallback)) return fallback;
    // 兜底 ImGui 自带字体
    std::string imgui_font = resources::Project::instance().root() + "/third_party/imgui/misc/fonts/Roboto-Medium.ttf";
    if (std::filesystem::exists(imgui_font)) return imgui_font;
    return std::string{};
}

std::string system_cjk_font_path(Language lang) {
    // 按优先级尝试系统 CJK 字体；中文优先微软雅黑族，其余回退通用中文字体
    std::vector<std::string> candidates;
#ifdef _WIN32
    if (lang == Language::Chinese) {
        candidates = {
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/msyhl.ttc",
            "C:/Windows/Fonts/simsun.ttc",
            "C:/Windows/Fonts/NotoSansSC-VF.ttf",
        };
    } else {
        candidates = {
            "C:/Windows/Fonts/NotoSansSC-VF.ttf",
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/simsun.ttc",
        };
    }
#else
    (void)lang;
    candidates = {
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
    };
#endif
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) return p;
    }
    return std::string{};
}

std::string theme_config_path(const std::string& project_root) {
    return project_root + "/editor_theme.json";
}

} // namespace

ThemeConfig default_theme_config() {
    return ThemeConfig{};
}

bool load_editor_font(const ThemeConfig& config) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontAtlas* atlas = io.Fonts;

    std::string path = config.font_path;
    if (path.empty()) {
        path = default_font_path();
    }

    if (path.empty() || !std::filesystem::exists(path)) {
        GLOG_WARN("EditorTheme: no usable font found, using ImGui default");
        return false;
    }

    // 若字体配置与当前语言均未变化，跳过重建，避免色调/圆角调整时重复 Build atlas。
    static ThemeConfig s_last_config{};
    static Language s_last_lang = Language::English;
    static bool s_has_last = false;
    const Language lang = Localization::instance().current_language();
    if (s_has_last &&
        std::abs(s_last_config.font_size - config.font_size) < 0.001f &&
        s_last_config.font_path == config.font_path &&
        s_last_lang == lang) {
        return true;
    }

    // 运行时热重载：先清除旧字体数据，再重新构建 atlas。
    // 注意：此函数应在上一帧渲染完成后、下一帧 NewFrame 之前调用。
    atlas->Clear();

    ImFontConfig font_cfg{};
    font_cfg.FontDataOwnedByAtlas = false;
    font_cfg.MergeMode = false;

    // 主字体负责拉丁、西里尔和通用标点
    static const ImWchar latin_ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x0100, 0x017F, // Latin Extended-A
        0x0400, 0x04FF, // Cyrillic
        0x2000, 0x206F, // General Punctuation
        0,
    };

    ImFont* primary = atlas->AddFontFromFileTTF(path.c_str(), config.font_size, &font_cfg, latin_ranges);
    if (!primary) {
        GLOG_ERROR("EditorTheme: failed to load primary font '{}'", path);
        return false;
    }
    GLOG_INFO("EditorTheme: loaded primary font '{}' size={:.1f}", path, config.font_size);

    // 若当前语言为中文，合并系统 CJK 字体以正确显示汉字
    if (lang == Language::Chinese) {
        std::string cjk_path = system_cjk_font_path(lang);
        if (!cjk_path.empty()) {
            ImFontConfig cjk_cfg{};
            cjk_cfg.FontDataOwnedByAtlas = false;
            cjk_cfg.MergeMode = true;
            // CJK 字形通常更大，给 atlas 留些边距
            cjk_cfg.PixelSnapH = true;

            static const ImWchar cjk_ranges[] = {
                0x3000, 0x30FF, // CJK Symbols / Punctuation / Kana
                0x31F0, 0x31FF, // Kana Phonetic Extensions
                0x3400, 0x4DBF, // CJK Unified Ideographs Extension A
                0x4E00, 0x9FFF, // CJK Unified Ideographs
                0xFF00, 0xFFEF, // Halfwidth/Fullwidth forms
                0,
            };

            ImFont* cjk_font = atlas->AddFontFromFileTTF(cjk_path.c_str(), config.font_size, &cjk_cfg, cjk_ranges);
            if (cjk_font) {
                GLOG_INFO("EditorTheme: merged CJK font '{}' for language '{}'", cjk_path, language_code(lang));
            } else {
                GLOG_WARN("EditorTheme: failed to merge CJK font '{}'", cjk_path);
            }
        } else {
            GLOG_WARN("EditorTheme: no system CJK font found for language '{}'", language_code(lang));
        }
    }

    // 重建字体 atlas；失败时回退到 ImGui 默认字体，避免无字体可用。
    if (!atlas->Build()) {
        GLOG_ERROR("EditorTheme: failed to build font atlas");
        atlas->Clear();
        return false;
    }

    io.FontDefault = primary;

    s_last_config = config;
    s_last_lang = lang;
    s_has_last = true;
    return true;
}

void apply_theme(ThemePreset preset, const ThemeConfig& config) {
    ImGuiStyle& style = ImGui::GetStyle();
    set_common_style(style, config);

    if (preset == ThemePreset::Dark) {
        ImGui::StyleColorsDark();
        apply_palette(style, make_dark_palette(), config.accent_hue, config.shadow);
    } else {
        ImGui::StyleColorsLight();
        // 浅色模式下黑色阴影与浅色背景不协调，强制禁用
        apply_palette(style, make_light_palette(), config.accent_hue, false);
    }

    // 加载字体仅在上下文存在时执行；失败不影响主题应用
    if (ImGui::GetCurrentContext()) {
        load_editor_font(config);
    }
}

void save_theme_config(const std::string& project_root, const ThemeConfig& config, ThemePreset preset) {
    nlohmann::json j;
    j["preset"]      = (preset == ThemePreset::Dark) ? "dark" : "light";
    j["accent_hue"]  = config.accent_hue;
    j["font_size"]   = config.font_size;
    j["font_path"]   = config.font_path;
    j["rounding"]    = config.rounding;
    j["shadow"]      = config.shadow;

    std::string path = theme_config_path(project_root);
    std::ofstream ofs(path);
    if (!ofs) {
        GLOG_ERROR("EditorTheme: failed to write '{}'", path);
        return;
    }
    ofs << j.dump(4);
    GLOG_INFO("EditorTheme: saved config '{}'", path);
}

bool load_theme_config(const std::string& project_root, ThemeConfig& out_config, ThemePreset& out_preset) {
    std::string path = theme_config_path(project_root);
    if (!std::filesystem::exists(path)) {
        return false;
    }
    std::ifstream ifs(path);
    if (!ifs) {
        GLOG_ERROR("EditorTheme: failed to open '{}'", path);
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        out_config.accent_hue  = j.value("accent_hue", out_config.accent_hue);
        out_config.font_size   = j.value("font_size", out_config.font_size);
        out_config.font_path   = j.value("font_path", out_config.font_path);
        out_config.rounding    = j.value("rounding", out_config.rounding);
        out_config.shadow      = j.value("shadow", out_config.shadow);
        std::string preset_str = j.value("preset", "dark");
        out_preset = (preset_str == "light") ? ThemePreset::Light : ThemePreset::Dark;
    } catch (const std::exception& e) {
        GLOG_ERROR("EditorTheme: failed to parse '{}': {}", path, e.what());
        return false;
    }
    GLOG_INFO("EditorTheme: loaded config '{}'", path);
    return true;
}

} // namespace gryce_engine::editor
