#include "engine_theme_modern.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

#include "../localization/localization.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "utils/glog/glog_lib.h"

namespace EngineTheme {

namespace {

static ImFont* g_modern_ui_font = nullptr;
static ImFont* g_modern_mono_font = nullptr;

constexpr float k_window_rounding = 10.0f;
constexpr float k_button_rounding = 7.0f;
constexpr float k_input_rounding  = 5.0f;
constexpr float k_card_rounding   = 12.0f;

static FontSizeClass g_font_size_class = FontSizeClass::Medium;

float ui_size_for_class(FontSizeClass c) {
    switch (c) {
        case FontSizeClass::Small:  return 14.0f;
        case FontSizeClass::Large:  return 18.0f;
        default:                    return 16.0f;
    }
}

float mono_size_for_class(FontSizeClass c) {
    switch (c) {
        case FontSizeClass::Small:  return 11.0f;
        case FontSizeClass::Large:  return 14.0f;
        default:                    return 12.0f;
    }
}

ImVec4 U32ToVec4(ImU32 c) {
    return ImVec4(
        static_cast<float>((c >> 0)  & 0xFF) / 255.0f,
        static_cast<float>((c >> 8)  & 0xFF) / 255.0f,
        static_cast<float>((c >> 16) & 0xFF) / 255.0f,
        static_cast<float>((c >> 24) & 0xFF) / 255.0f);
}

void SetColor(ImGuiCol idx, ImU32 c) {
    ImGui::GetStyle().Colors[idx] = U32ToVec4(c);
}

void SetColorAlpha(ImGuiCol idx, ImU32 c, uint8_t alpha) {
    ImU32 with_alpha = (c & 0x00FFFFFF) | (static_cast<ImU32>(alpha) << 24);
    SetColor(idx, with_alpha);
}

std::string project_font_path(const std::string& filename) {
    std::string root = gryce_engine::resources::Project::instance().root();
    std::string p = root + "/editor/project/fonts/" + filename;
    if (std::filesystem::exists(p)) return p;
    try {
        std::string resolved = gryce_engine::resources::ResourcePath::resolve("res:/fonts/" + filename);
        if (!resolved.empty() && std::filesystem::exists(resolved)) return resolved;
    } catch (...) {}
    return std::string{};
}

std::string first_existing(const std::vector<std::string>& candidates) {
    for (const auto& p : candidates) {
        if (std::filesystem::exists(p)) return p;
    }
    return std::string{};
}

std::string find_modern_ui_font() {
    return first_existing({
        project_font_path("SFProText-Regular.otf"),
        project_font_path("SF-Pro-Text-Regular.otf"),
        project_font_path("Inter-Regular.ttf"),
        project_font_path("Roboto-Medium.ttf"),
    });
}

std::string find_modern_display_font() {
    return first_existing({
        project_font_path("SFProDisplay-Regular.otf"),
        project_font_path("SF-Pro-Display-Regular.otf"),
        project_font_path("Inter-Regular.ttf"),
        project_font_path("Roboto-Medium.ttf"),
    });
}

std::string find_modern_mono_font() {
    return first_existing({
        project_font_path("SFMono-Regular.otf"),
        project_font_path("SF-Mono-Regular.otf"),
        project_font_path("JetBrainsMono-Regular.ttf"),
        project_font_path("Cousine-Regular.ttf"),
    });
}

std::string system_cjk_font_path(gryce_engine::editor::Language lang) {
    std::vector<std::string> candidates;
#ifdef _WIN32
    if (lang == gryce_engine::editor::Language::Chinese) {
        candidates = {
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/msyhl.ttc",
            "C:/Windows/Fonts/simsun.ttc",
        };
    } else {
        candidates = {
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/simsun.ttc",
        };
    }
#else
    if (lang == gryce_engine::editor::Language::Chinese) {
        candidates = {
            "/System/Library/Fonts/PingFang.ttc",
            "/System/Library/Fonts/STHeiti Light.ttc",
            "/System/Library/Fonts/Hiragino Sans GB.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        };
    } else {
        candidates = {
            "/System/Library/Fonts/PingFang.ttc",
            "/System/Library/Fonts/STHeiti Light.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        };
    }
#endif
    return first_existing(candidates);
}

// 简单放大镜图标（线条风格，类似现代系统图标）
void draw_search_icon(ImDrawList* dl, ImVec2 center, float radius, ImU32 col) {
    dl->AddCircle(center, radius, col, 12, 1.5f);
    ImVec2 handle_end(center.x + radius * 0.75f, center.y + radius * 0.75f);
    dl->AddLine(ImVec2(center.x + radius * 0.55f, center.y + radius * 0.55f),
                handle_end, col, 1.5f);
}

// 返回箭头图标（左向 V 形）
void draw_back_arrow(ImDrawList* dl, ImVec2 center, float size, ImU32 col) {
    float half = size * 0.5f;
    dl->AddLine(ImVec2(center.x + half, center.y - half),
                ImVec2(center.x - half, center.y),
                col, 2.0f);
    dl->AddLine(ImVec2(center.x - half, center.y),
                ImVec2(center.x + half, center.y + half),
                col, 2.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------
void ModernDark::Apply() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = k_window_rounding;
    style.ChildRounding     = k_window_rounding;
    style.PopupRounding     = 8.0f;
    style.FrameRounding     = k_input_rounding;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;
    style.ScrollbarRounding = 5.0f;

    style.WindowBorderSize = 0.0f;
    style.PopupBorderSize  = 0.0f;
    style.ChildBorderSize  = 0.0f;
    style.FrameBorderSize  = 0.0f;
    style.TabBorderSize    = 0.0f;

    style.WindowPadding    = ImVec2(10.0f, 10.0f);
    style.FramePadding     = ImVec2(8.0f, 5.0f);
    style.CellPadding      = ImVec2(8.0f, 4.0f);
    style.ItemSpacing      = ImVec2(6.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing    = 16.0f;
    style.ScrollbarSize    = 10.0f;
    style.GrabMinSize      = 10.0f;

    style.AntiAliasedLines = true;
    style.AntiAliasedFill  = true;

    SetColor(ImGuiCol_Text,                 TextPrimary);
    SetColor(ImGuiCol_TextDisabled,         TextDisabled);
    SetColorAlpha(ImGuiCol_TextSelectedBg,  Accent, 60);

    SetColor(ImGuiCol_WindowBg,             WindowBackground);
    SetColor(ImGuiCol_ChildBg,              ContentBackground);
    SetColor(ImGuiCol_PopupBg,              ContentBackground);

    SetColor(ImGuiCol_Border,               Separator);
    SetColorAlpha(ImGuiCol_BorderShadow,    WindowBackground, 0);

    SetColor(ImGuiCol_FrameBg,              ContentBackground);
    SetColor(ImGuiCol_FrameBgHovered,       ContentBackground2);
    SetColor(ImGuiCol_FrameBgActive,        ContentBackground2);

    SetColor(ImGuiCol_TitleBg,              WindowBackground);
    SetColor(ImGuiCol_TitleBgActive,        ContentBackground);
    SetColor(ImGuiCol_TitleBgCollapsed,     WindowBackground);

    SetColor(ImGuiCol_MenuBarBg,            ContentBackground);
    SetColor(ImGuiCol_ScrollbarBg,          WindowBackground);
    SetColor(ImGuiCol_ScrollbarGrab,        Separator);
    SetColorAlpha(ImGuiCol_ScrollbarGrabHovered, Separator, 220);
    SetColor(ImGuiCol_ScrollbarGrabActive,  Accent);

    SetColor(ImGuiCol_CheckMark,            Accent);
    SetColorAlpha(ImGuiCol_SliderGrab,      Separator, 220);
    SetColor(ImGuiCol_SliderGrabActive,     Accent);

    SetColor(ImGuiCol_Button,               ContentBackground2);
    SetColor(ImGuiCol_ButtonHovered,        Separator);
    SetColor(ImGuiCol_ButtonActive,         Accent);

    SetColor(ImGuiCol_Header,               ContentBackground);
    SetColor(ImGuiCol_HeaderHovered,        ContentBackground2);
    SetColor(ImGuiCol_HeaderActive,         Separator);

    SetColor(ImGuiCol_Separator,            Separator);
    SetColorAlpha(ImGuiCol_SeparatorHovered,Accent, 180);
    SetColor(ImGuiCol_SeparatorActive,      Accent);

    SetColorAlpha(ImGuiCol_ResizeGrip,      Separator, 80);
    SetColorAlpha(ImGuiCol_ResizeGripHovered, Accent, 180);
    SetColor(ImGuiCol_ResizeGripActive,     Accent);

    SetColor(ImGuiCol_Tab,                  ContentBackground);
    SetColor(ImGuiCol_TabHovered,           ContentBackground2);
    SetColor(ImGuiCol_TabActive,            ContentBackground2);
    SetColor(ImGuiCol_TabUnfocused,         WindowBackground);
    SetColor(ImGuiCol_TabUnfocusedActive,   ContentBackground);

    SetColorAlpha(ImGuiCol_DockingPreview,  Accent, 120);
    SetColorAlpha(ImGuiCol_DockingEmptyBg,  WindowBackground, 80);

    SetColor(ImGuiCol_PlotLines,            Accent);
    SetColor(ImGuiCol_PlotLinesHovered,     Yellow);
    SetColor(ImGuiCol_PlotHistogram,        Accent);
    SetColor(ImGuiCol_PlotHistogramHovered, Yellow);

    SetColor(ImGuiCol_TableHeaderBg,        ContentBackground);
    SetColor(ImGuiCol_TableBorderStrong,    Separator);
    SetColorAlpha(ImGuiCol_TableBorderLight,Separator, 160);
    SetColorAlpha(ImGuiCol_TableRowBg,      ContentBackground, 0);
    SetColorAlpha(ImGuiCol_TableRowBgAlt,   ContentBackground2, 120);

    SetColorAlpha(ImGuiCol_DragDropTarget,  Accent, 220);
    SetColor(ImGuiCol_NavHighlight,         Accent);
    SetColor(ImGuiCol_NavWindowingHighlight,Accent);
    SetColor(ImGuiCol_NavWindowingDimBg,    ModalDim);
    SetColor(ImGuiCol_ModalWindowDimBg,     ModalDim);
}

// ---------------------------------------------------------------------------
// ModernLight::Apply
// ---------------------------------------------------------------------------
void ModernLight::Apply() {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = k_window_rounding;
    style.ChildRounding     = k_window_rounding;
    style.PopupRounding     = 10.0f;
    style.FrameRounding     = k_input_rounding;
    style.GrabRounding      = 7.0f;
    style.TabRounding       = 7.0f;
    style.ScrollbarRounding = 7.0f;

    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize  = 1.0f;
    style.ChildBorderSize  = 1.0f;
    style.FrameBorderSize  = 1.0f;
    style.TabBorderSize    = 1.0f;

    style.WindowPadding    = ImVec2(12.0f, 12.0f);
    style.FramePadding     = ImVec2(10.0f, 6.0f);
    style.CellPadding      = ImVec2(10.0f, 5.0f);
    style.ItemSpacing      = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(8.0f, 5.0f);
    style.IndentSpacing    = 18.0f;
    style.ScrollbarSize    = 12.0f;
    style.GrabMinSize      = 12.0f;

    style.AntiAliasedLines = true;
    style.AntiAliasedFill  = true;

    SetColor(ImGuiCol_Text,                 TextPrimary);
    SetColor(ImGuiCol_TextDisabled,         TextDisabled);
    SetColorAlpha(ImGuiCol_TextSelectedBg,  Accent, 40);

    SetColor(ImGuiCol_WindowBg,             WindowBackground);
    SetColor(ImGuiCol_ChildBg,              ContentBackground);
    SetColor(ImGuiCol_PopupBg,              ContentBackground);

    // 边框使用与浅色背景形成对比的深灰色，并给弹出层/窗口加柔和阴影
    SetColor(ImGuiCol_Border,               TextSecondary);
    SetColorAlpha(ImGuiCol_BorderShadow,    IM_COL32(0x00, 0x00, 0x00, 0xFF), 80);

    SetColor(ImGuiCol_FrameBg,              SecondaryPanel);
    SetColor(ImGuiCol_FrameBgHovered,       Separator);
    SetColor(ImGuiCol_FrameBgActive,        Separator);

    SetColor(ImGuiCol_TitleBg,              WindowBackground);
    SetColor(ImGuiCol_TitleBgActive,        ContentBackground);
    SetColor(ImGuiCol_TitleBgCollapsed,     WindowBackground);

    SetColor(ImGuiCol_MenuBarBg,            ContentBackground);
    SetColor(ImGuiCol_ScrollbarBg,          WindowBackground);
    SetColor(ImGuiCol_ScrollbarGrab,        Separator);
    SetColorAlpha(ImGuiCol_ScrollbarGrabHovered, Separator, 220);
    SetColor(ImGuiCol_ScrollbarGrabActive,  Accent);

    SetColor(ImGuiCol_CheckMark,            Accent);
    SetColorAlpha(ImGuiCol_SliderGrab,      Separator, 220);
    SetColor(ImGuiCol_SliderGrabActive,     Accent);

    SetColor(ImGuiCol_Button,               Accent);
    SetColor(ImGuiCol_ButtonHovered,        AccentHover);
    SetColor(ImGuiCol_ButtonActive,         AccentActive);

    SetColor(ImGuiCol_Header,               ContentBackground);
    SetColor(ImGuiCol_HeaderHovered,        SelectedFill);
    SetColor(ImGuiCol_HeaderActive,         SelectedFill);

    SetColor(ImGuiCol_Separator,            Separator);
    SetColorAlpha(ImGuiCol_SeparatorHovered,Separator, 180);
    SetColor(ImGuiCol_SeparatorActive,      Accent);

    SetColorAlpha(ImGuiCol_ResizeGrip,      Separator, 80);
    SetColorAlpha(ImGuiCol_ResizeGripHovered, Accent, 180);
    SetColor(ImGuiCol_ResizeGripActive,     Accent);

    SetColor(ImGuiCol_Tab,                  ContentBackground);
    SetColor(ImGuiCol_TabHovered,           SelectedFill);
    SetColor(ImGuiCol_TabActive,            SelectedFill);
    SetColor(ImGuiCol_TabUnfocused,         WindowBackground);
    SetColor(ImGuiCol_TabUnfocusedActive,   ContentBackground);

    SetColorAlpha(ImGuiCol_DockingPreview,  Accent, 80);
    SetColorAlpha(ImGuiCol_DockingEmptyBg,  WindowBackground, 80);

    SetColor(ImGuiCol_PlotLines,            Accent);
    SetColor(ImGuiCol_PlotLinesHovered,     Yellow);
    SetColor(ImGuiCol_PlotHistogram,        Accent);
    SetColor(ImGuiCol_PlotHistogramHovered, Yellow);

    SetColor(ImGuiCol_TableHeaderBg,        ContentBackground);
    SetColor(ImGuiCol_TableBorderStrong,    Separator);
    SetColorAlpha(ImGuiCol_TableBorderLight,Separator, 160);
    SetColorAlpha(ImGuiCol_TableRowBg,      ContentBackground, 0);
    SetColorAlpha(ImGuiCol_TableRowBgAlt,   SecondaryPanel, 120);

    SetColorAlpha(ImGuiCol_DragDropTarget,  Accent, 220);
    SetColor(ImGuiCol_NavHighlight,         Accent);
    SetColor(ImGuiCol_NavWindowingHighlight,Accent);
    SetColor(ImGuiCol_NavWindowingDimBg,    ModalDim);
    SetColor(ImGuiCol_ModalWindowDimBg,     ModalDim);
}

// ---------------------------------------------------------------------------
// SidebarItem
// ---------------------------------------------------------------------------
bool SidebarItem(const char* label, const char* icon_text, bool selected,
                 ImU32 indicator_color, const ImVec2& size_arg) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = size_arg.x > 0 ? size_arg.x : ImGui::GetContentRegionAvail().x;
    float height = size_arg.y > 0 ? size_arg.y : 38.0f;

    ImGui::PushID(label);
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton("##sidebar", ImVec2(width, height));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    ImU32 bg = selected ? ModernDark::ContentBackground2
             : hovered  ? ModernDark::ContentBackground
             : 0;
    if (bg != 0) {
        dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), bg, 4.0f);
    }

    if (selected) {
        float ind_h = height * 0.45f;
        float ind_y = pos.y + (height - ind_h) * 0.5f;
        dl->AddRectFilled(ImVec2(pos.x + 4.0f, ind_y),
                          ImVec2(pos.x + 8.0f,  ind_y + ind_h),
                          indicator_color, 2.0f);
    }

    // 图标
    ImVec2 icon_size = ImGui::CalcTextSize(icon_text);
    ImVec2 icon_pos(pos.x + 14.0f + (22.0f - icon_size.x) * 0.5f,
                    pos.y + (height - icon_size.y) * 0.5f);
    dl->AddText(icon_pos, selected ? ModernDark::TextPrimary : ModernDark::TextSecondary, icon_text);

    // 文字
    ImVec2 text_size = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pos.x + 44.0f, pos.y + (height - text_size.y) * 0.5f),
                selected ? ModernDark::TextPrimary : ModernDark::TextSecondary, label);

    return clicked;
}

// ---------------------------------------------------------------------------
// ToolbarButton
// ---------------------------------------------------------------------------
bool ToolbarButton(const char* id, const char* icon_text, const ImVec2& size) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, U32ToVec4(ModernDark::ContentBackground2));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  U32ToVec4(ModernDark::Separator));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, k_button_rounding);
    bool pressed = ImGui::Button(icon_text, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImGui::PopID();
    return pressed;
}

// ---------------------------------------------------------------------------
// SearchField
// ---------------------------------------------------------------------------
bool SearchField(const char* label, char* buf, size_t buf_size, const char* hint, float width) {
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, k_input_rounding);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ModernDark::ContentBackground);

    if (width > 0.0f) {
        ImGui::SetNextItemWidth(width);
    }

    ImVec2 start = ImGui::GetCursorScreenPos();
    bool changed = ImGui::InputTextWithHint("##search", hint, buf, buf_size);

    // 左侧放大镜图标
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float frame_h = ImGui::GetFrameHeight();
    draw_search_icon(dl, ImVec2(start.x + 12.0f, start.y + frame_h * 0.5f),
                     frame_h * 0.25f, ModernDark::TextSecondary);

    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------------------
// SegmentedControl
// ---------------------------------------------------------------------------
bool SegmentedControl(const char* label, int* current_item,
                      const char* const items[], int items_count, float width) {
    if (!current_item || items_count <= 0) return false;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 start = ImGui::GetCursorScreenPos();
    float total_w = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    float item_w = total_w / static_cast<float>(items_count);
    float height = 24.0f;

    ImGui::PushID(label);
    bool changed = false;

    for (int i = 0; i < items_count; ++i) {
        ImVec2 item_min(start.x + i * item_w, start.y);
        ImVec2 item_max(start.x + (i + 1) * item_w, start.y + height);

        ImGui::SetCursorScreenPos(item_min);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##seg", ImVec2(item_w, height));
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        if (clicked) {
            *current_item = i;
            changed = true;
        }

        bool selected = (*current_item == i);

        // 背景：整体胶囊
        ImDrawFlags flags = 0;
        if (i == 0) flags = ImDrawFlags_RoundCornersLeft;
        else if (i == items_count - 1) flags = ImDrawFlags_RoundCornersRight;

        if (selected) {
            dl->AddRectFilled(item_min, item_max, ModernDark::Accent, 4.0f, flags);
        } else if (hovered) {
            dl->AddRectFilled(item_min, item_max, ModernDark::ContentBackground2, 4.0f, flags);
        }

        // 文字居中
        ImVec2 text_size = ImGui::CalcTextSize(items[i]);
        dl->AddText(ImVec2(item_min.x + (item_w - text_size.x) * 0.5f,
                           item_min.y + (height - text_size.y) * 0.5f),
                    selected ? ModernDark::WindowBackground : ModernDark::TextPrimary,
                    items[i]);
    }

    ImGui::PopID();
    ImGui::Dummy(ImVec2(total_w, height));
    return changed;
}

// ---------------------------------------------------------------------------
// ToggleSwitch
// ---------------------------------------------------------------------------
bool ToggleSwitch(const char* label, bool* v) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    const float width  = 36.0f;
    const float height = 20.0f;
    const float radius = height * 0.5f;

    ImGui::PushID(label);
    bool clicked = ImGui::InvisibleButton("##toggle", ImVec2(width, height));
    if (clicked && v) *v = !*v;
    ImGui::PopID();

    bool on = v ? *v : false;

    // 轨道
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                      on ? ModernDark::Green : ModernDark::Separator, radius);

    // 滑块
    float knob_x = on ? (pos.x + width - radius) : (pos.x + radius);
    dl->AddCircleFilled(ImVec2(knob_x, pos.y + radius),
                        radius - 2.0f, ModernDark::WindowBackground);

    // 标签在右侧
    ImVec2 text_size(0, 0);
    if (label && label[0]) {
        text_size = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(pos.x + width + 8.0f, pos.y + (height - text_size.y) * 0.5f),
                    ModernDark::TextPrimary, label);
    }

    ImGui::Dummy(ImVec2(width + (label && label[0] ? 8.0f + text_size.x : 0.0f), height));
    return clicked;
}

// ---------------------------------------------------------------------------
// SidebarItemLight
// ---------------------------------------------------------------------------
bool SidebarItemLight(const char* label, const char* icon_text, bool selected, const ImVec2& size_arg) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = size_arg.x > 0 ? size_arg.x : ImGui::GetContentRegionAvail().x;
    float height = size_arg.y > 0 ? size_arg.y : 44.0f;

    ImGui::PushID(label);
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton("##sidebar_light", ImVec2(width, height));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    ImU32 bg = selected ? ModernLight::SelectedFill
             : hovered  ? ModernLight::SecondaryPanel
             : 0;
    if (bg != 0) {
        dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), bg, 10.0f);
    }

    ImVec2 icon_size = ImGui::CalcTextSize(icon_text);
    ImVec2 icon_pos(pos.x + 12.0f + (28.0f - icon_size.x) * 0.5f,
                    pos.y + (height - icon_size.y) * 0.5f);
    dl->AddText(icon_pos, selected ? ModernLight::Accent : ModernLight::TextSecondary, icon_text);

    ImVec2 text_size = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pos.x + 48.0f, pos.y + (height - text_size.y) * 0.5f),
                selected ? ModernLight::Accent : ModernLight::TextPrimary, label);

    return clicked;
}

// ---------------------------------------------------------------------------
// NavBar
// ---------------------------------------------------------------------------
bool NavBar(const char* title, bool* back_clicked, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;

    // 背景
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ModernLight::ContentBackground, 0.0f);

    // 返回按钮区域
    float back_w = 44.0f;
    ImGui::PushID(title);
    ImGui::SetCursorScreenPos(pos);
    bool back = ImGui::InvisibleButton("##nav_back", ImVec2(back_w, height));
    bool back_hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    draw_back_arrow(dl, ImVec2(pos.x + 18.0f, pos.y + height * 0.5f), 7.0f,
                    back_hovered ? ModernLight::Accent : ModernLight::TextPrimary);

    // 标题居中
    ImVec2 text_size = ImGui::CalcTextSize(title);
    dl->AddText(ImVec2(pos.x + (width - text_size.x) * 0.5f,
                       pos.y + (height - text_size.y) * 0.5f),
                ModernLight::TextPrimary, title);

    ImGui::Dummy(ImVec2(width, height));

    if (back_clicked) *back_clicked = back;
    return false;
}

// ---------------------------------------------------------------------------
// ListRow
// ---------------------------------------------------------------------------
bool ListRow(const char* label, bool selected, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;

    ImGui::PushID(label);
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton("##listrow", ImVec2(width, height));
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    ImU32 bg = selected ? ModernLight::SelectedFill
             : hovered  ? ModernLight::SecondaryPanel
             : 0;
    if (bg != 0) {
        dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), bg, 10.0f);
    }

    ImVec2 text_size = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(pos.x + 16.0f, pos.y + (height - text_size.y) * 0.5f),
                selected ? ModernLight::Accent : ModernLight::TextPrimary, label);

    return clicked;
}

// ---------------------------------------------------------------------------
// FilledButton / OutlineButton / TextButton
// ---------------------------------------------------------------------------
bool FilledButton(const char* label, const ImVec2& size) {
    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_Button,        U32ToVec4(ModernLight::Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, U32ToVec4(ModernLight::Accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  U32ToVec4(ModernLight::Accent));
    ImGui::PushStyleColor(ImGuiCol_Text,          U32ToVec4(ModernLight::ContentBackground));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, k_button_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0f, 7.0f));
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    return pressed;
}

bool OutlineButton(const char* label, const ImVec2& size) {
    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_Button,        U32ToVec4(ModernLight::ContentBackground));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, U32ToVec4(ModernLight::SelectedFill));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  U32ToVec4(ModernLight::SelectedFill));
    ImGui::PushStyleColor(ImGuiCol_Text,          U32ToVec4(ModernLight::Accent));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, k_button_rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, U32ToVec4(ModernLight::Accent));
    bool pressed = ImGui::Button(label, size);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    ImGui::PopID();
    return pressed;
}

bool TextButton(const char* label) {
    ImGui::PushID(label);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text,          U32ToVec4(ModernLight::Accent));
    bool pressed = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    ImGui::PopID();
    return pressed;
}

// ---------------------------------------------------------------------------
// Card
// ---------------------------------------------------------------------------
static ImVec2 g_card_content_min;
static ImVec2 g_card_content_max;

void BeginCard(const char* id, const ImVec2& size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = size.x > 0 ? size.x : ImGui::GetContentRegionAvail().x;
    float height = size.y > 0 ? size.y : 0.0f;

    ImGui::PushID(id);
    g_card_content_min = pos;
    g_card_content_max = ImVec2(pos.x + width, pos.y + height);

    // 先绘制背景与阴影（阴影用半透明矩形模拟）
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + (height > 0 ? height : 1.0f)),
                      ModernLight::ContentBackground, k_card_rounding);
    dl->AddRect(pos, ImVec2(pos.x + width, pos.y + (height > 0 ? height : 1.0f)),
                ModernLight::Separator, k_card_rounding, 0, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(pos.x + 12.0f, pos.y + 12.0f));
}

void EndCard() {
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    float h = cursor.y - g_card_content_min.y + 12.0f;
    g_card_content_max.y = g_card_content_min.y + h;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(g_card_content_min, g_card_content_max,
                      ModernLight::ContentBackground, k_card_rounding);
    dl->AddRect(g_card_content_min, g_card_content_max,
                ModernLight::Separator, k_card_rounding, 0, 1.0f);

    ImGui::PopID();
    ImGui::Dummy(ImVec2(g_card_content_max.x - g_card_content_min.x, h));
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
bool LoadModernFonts(float ui_size, float mono_size) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontAtlas* atlas = io.Fonts;

    std::string ui_path = find_modern_ui_font();
    if (ui_path.empty()) {
        GLOG_WARN("EngineTheme: no usable UI font found");
        return false;
    }

    atlas->Clear();

    static const ImWchar latin_ranges[] = {
        0x0020, 0x00FF,
        0x0100, 0x017F,
        0x0400, 0x04FF,
        0x2000, 0x206F,
        0,
    };

    ImFontConfig ui_cfg{};
    ui_cfg.FontDataOwnedByAtlas = false;
    ui_cfg.MergeMode = false;

    g_modern_ui_font = atlas->AddFontFromFileTTF(ui_path.c_str(), ui_size, &ui_cfg, latin_ranges);
    if (!g_modern_ui_font) {
        GLOG_ERROR("EngineTheme: failed to load UI font '{}'", ui_path);
        atlas->Clear();
        return false;
    }
    GLOG_INFO("EngineTheme: loaded UI font '{}' size={:.1f}", ui_path, ui_size);

    // CJK 字体回退：无论当前语言设置如何都合并中文字形
    using Language = gryce_engine::editor::Language;
    const Language lang = gryce_engine::editor::Localization::instance().current_language();
    std::string cjk_path = system_cjk_font_path(lang);
    if (cjk_path.empty()) {
        cjk_path = system_cjk_font_path(Language::Chinese);
    }
    if (!cjk_path.empty()) {
        ImFontConfig cjk_cfg{};
        cjk_cfg.FontDataOwnedByAtlas = false;
        cjk_cfg.MergeMode = true;
        cjk_cfg.PixelSnapH = true;
        cjk_cfg.OversampleH = 1;
        cjk_cfg.OversampleV = 1;

        static const ImWchar cjk_ranges[] = {
            0x3000, 0x30FF,
            0x31F0, 0x31FF,
            0x3400, 0x4DBF,
            0x4E00, 0x9FFF,
            0xFF00, 0xFFEF,
            0,
        };

        ImFont* cjk_font = atlas->AddFontFromFileTTF(cjk_path.c_str(), ui_size, &cjk_cfg, cjk_ranges);
        if (cjk_font) {
            GLOG_INFO("EngineTheme: merged CJK font '{}'", cjk_path);
        } else {
            GLOG_WARN("EngineTheme: failed to merge CJK font '{}'", cjk_path);
        }
    }

    // 等宽代码字体
    g_modern_mono_font = nullptr;
    std::string mono_path = find_modern_mono_font();
    if (!mono_path.empty()) {
        ImFontConfig mono_cfg{};
        mono_cfg.FontDataOwnedByAtlas = false;
        mono_cfg.MergeMode = false;
        mono_cfg.PixelSnapH = true;
        g_modern_mono_font = atlas->AddFontFromFileTTF(mono_path.c_str(), mono_size, &mono_cfg, latin_ranges);
        if (g_modern_mono_font) {
            GLOG_INFO("EngineTheme: loaded code font '{}' size={:.1f}", mono_path, mono_size);
        }
    }

    if (!atlas->Build()) {
        GLOG_ERROR("EngineTheme: failed to build font atlas");
        atlas->Clear();
        g_modern_ui_font = nullptr;
        g_modern_mono_font = nullptr;
        return false;
    }

    io.FontDefault = g_modern_ui_font;
    return true;
}

bool LoadModernFonts(FontSizeClass size_class) {
    g_font_size_class = size_class;
    return LoadModernFonts(ui_size_for_class(size_class), mono_size_for_class(size_class));
}

ImFont* ModernUIFont() { return g_modern_ui_font; }
ImFont* ModernMonoFont() { return g_modern_mono_font; }

} // namespace EngineTheme
