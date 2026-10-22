// ===========================================================================
// engine/ui/ui_theme.cpp — 主题 token 的实现与 ImGui 落地
//
// token 表本身与 ImGui 无关（无 ImGui 的构建也编译本文件，apply() 退化
// 为 no-op）；只有 apply() 一个函数住在 #ifdef 里。
// ===========================================================================

#include "ui/ui_theme.h"

#ifdef GRYCE_HAS_IMGUI
#include <imgui.h>
#endif

namespace gryce_ui {

namespace {

// 两套调色板（0xRRGGBBAA）。深色与 ImGui 自带深色接近：引擎调试面板换过来
// 没有观感跳变；浅色是同一套 token 的亮色取值。
const uint32_t kPaletteDark[THEME_COLOR_COUNT] = {
    0x1F1F24FF,  // WINDOW_BG
    0x17171BFF,  // CHILD_BG
    0x3A3A42FF,  // BORDER
    0xE6E6EAFF,  // TEXT
    0x9A9AA3FF,  // TEXT_DIM
    0x4C8DFFFF,  // ACCENT
    0x6FA3FFFF,  // ACCENT_HOVER
    0x3B76DDFF,  // ACCENT_ACTIVE
    0x2E2E36FF,  // BUTTON
    0x3A3A44FF,  // BUTTON_HOVER
    0x44444EFF,  // BUTTON_ACTIVE
    0x26262CFF,  // HEADER
    0x32323AFF,  // HEADER_HOVER
    0x2F5D8FFF,  // SELECTION
    0x6BBE6BFF,  // OK
    0xE0C15AFF,  // WARN
    0xE06C6CFF,  // ERROR
};

const uint32_t kPaletteLight[THEME_COLOR_COUNT] = {
    0xF2F2F4FF,  // WINDOW_BG
    0xFFFFFFFF,  // CHILD_BG
    0xC8C8CEFF,  // BORDER
    0x222228FF,  // TEXT
    0x6A6A72FF,  // TEXT_DIM
    0x2E6BE6FF,  // ACCENT
    0x4F86F0FF,  // ACCENT_HOVER
    0x2455B8FF,  // ACCENT_ACTIVE
    0xE4E4EAFF,  // BUTTON
    0xD8D8E0FF,  // BUTTON_HOVER
    0xCACAD2FF,  // BUTTON_ACTIVE
    0xEAEAEFFF,  // HEADER
    0xE0E0E6FF,  // HEADER_HOVER
    0xC9DCFAFF,  // SELECTION
    0x2E8B2EFF,  // OK
    0xA67C00FF,  // WARN
    0xC0392BFF,  // ERROR
};

// 度量与预设无关（两套主题共用同一套几何）。
const float kMetrics[THEME_METRIC_COUNT] = {
    8.0f,   // PAD_X
    5.0f,   // PAD_Y
    22.0f,  // ROW_H
    4.0f,   // ITEM_SPACING
    18.0f,  // INDENT
    1.0f,   // BORDER_W
};

Theme& mutable_theme() {
    static Theme t = [] {
        Theme d{};
        for (int i = 0; i < THEME_COLOR_COUNT; ++i) d.colors[i] = kPaletteDark[i];
        for (int i = 0; i < THEME_METRIC_COUNT; ++i) d.metrics[i] = kMetrics[i];
        return d;
    }();
    return t;
}

} // namespace

const Theme& theme() { return mutable_theme(); }

void set_preset(int preset) {
    Theme& t = mutable_theme();
    const uint32_t* palette = preset == THEME_PRESET_LIGHT ? kPaletteLight : kPaletteDark;
    for (int i = 0; i < THEME_COLOR_COUNT; ++i) t.colors[i] = palette[i];
}

void set_color(int token, uint32_t rgba) {
    if (token < 0 || token >= THEME_COLOR_COUNT) return;
    mutable_theme().colors[token] = rgba;
}

uint32_t color(int token) {
    if (token < 0 || token >= THEME_COLOR_COUNT) return 0;
    return theme().colors[token];
}

void set_metric(int token, float value) {
    if (token < 0 || token >= THEME_METRIC_COUNT) return;
    mutable_theme().metrics[token] = value;
}

float metric(int token) {
    if (token < 0 || token >= THEME_METRIC_COUNT) return 0.0f;
    return theme().metrics[token];
}

void apply() {
#ifdef GRYCE_HAS_IMGUI
    // 无上下文时安全返回：apply 是宿主面的 API（GRender_Init 前后都能调），
    // 而 ImGui 的 GetStyle() 在没上下文时是空指针解引用。渲染器没接管
    // ImGui（--no-debug-ui 且无 UI 回调）时就会走到这里。
    if (ImGui::GetCurrentContext() == nullptr) return;
    const Theme& t = theme();
    ImGuiStyle& s = ImGui::GetStyle();
    auto col = [&](int token) {
        const uint32_t c = t.colors[token];
        return ImVec4(static_cast<float>((c >> 24) & 0xFF) / 255.0f,
                      static_cast<float>((c >> 16) & 0xFF) / 255.0f,
                      static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                      static_cast<float>(c & 0xFF) / 255.0f);
    };
    s.WindowPadding    = ImVec2(t.metrics[THEME_METRIC_PAD_X], t.metrics[THEME_METRIC_PAD_Y]);
    s.FramePadding     = ImVec2(t.metrics[THEME_METRIC_PAD_X], t.metrics[THEME_METRIC_PAD_Y] - 1.0f);
    s.ItemSpacing      = ImVec2(t.metrics[THEME_METRIC_PAD_X], t.metrics[THEME_METRIC_ITEM_SPACING]);
    s.IndentSpacing    = t.metrics[THEME_METRIC_INDENT];
    s.ChildBorderSize  = t.metrics[THEME_METRIC_BORDER_W];
    s.FrameBorderSize  = t.metrics[THEME_METRIC_BORDER_W];
    s.WindowBorderSize = t.metrics[THEME_METRIC_BORDER_W];

    s.Colors[ImGuiCol_WindowBg]         = col(THEME_COLOR_WINDOW_BG);
    s.Colors[ImGuiCol_ChildBg]          = col(THEME_COLOR_CHILD_BG);
    s.Colors[ImGuiCol_PopupBg]          = col(THEME_COLOR_WINDOW_BG);
    s.Colors[ImGuiCol_Border]           = col(THEME_COLOR_BORDER);
    s.Colors[ImGuiCol_Text]             = col(THEME_COLOR_TEXT);
    s.Colors[ImGuiCol_TextDisabled]     = col(THEME_COLOR_TEXT_DIM);
    s.Colors[ImGuiCol_Button]           = col(THEME_COLOR_BUTTON);
    s.Colors[ImGuiCol_ButtonHovered]    = col(THEME_COLOR_BUTTON_HOVER);
    s.Colors[ImGuiCol_ButtonActive]     = col(THEME_COLOR_BUTTON_ACTIVE);
    s.Colors[ImGuiCol_Header]           = col(THEME_COLOR_HEADER);
    s.Colors[ImGuiCol_HeaderHovered]    = col(THEME_COLOR_HEADER_HOVER);
    s.Colors[ImGuiCol_HeaderActive]     = col(THEME_COLOR_HEADER);
    s.Colors[ImGuiCol_FrameBg]          = col(THEME_COLOR_BUTTON);
    s.Colors[ImGuiCol_FrameBgHovered]   = col(THEME_COLOR_BUTTON_HOVER);
    s.Colors[ImGuiCol_FrameBgActive]    = col(THEME_COLOR_BUTTON_ACTIVE);
    s.Colors[ImGuiCol_CheckMark]        = col(THEME_COLOR_ACCENT);
    s.Colors[ImGuiCol_SliderGrab]       = col(THEME_COLOR_ACCENT);
    s.Colors[ImGuiCol_SliderGrabActive] = col(THEME_COLOR_ACCENT_ACTIVE);
    s.Colors[ImGuiCol_TextSelectedBg]   = col(THEME_COLOR_SELECTION);
    s.Colors[ImGuiCol_Separator]        = col(THEME_COLOR_BORDER);
    s.Colors[ImGuiCol_TitleBg]          = col(THEME_COLOR_HEADER);
    s.Colors[ImGuiCol_TitleBgActive]    = col(THEME_COLOR_HEADER);
    s.Colors[ImGuiCol_MenuBarBg]        = col(THEME_COLOR_HEADER);
#endif
}

} // namespace gryce_ui
