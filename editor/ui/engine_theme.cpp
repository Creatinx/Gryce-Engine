#include "engine_theme.h"

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

static ImFont* g_code_font = nullptr;

ImVec4 U32ToVec4(ImU32 c) {
    return ImVec4(
        static_cast<float>((c >> 0)  & 0xFF) / 255.0f,
        static_cast<float>((c >> 8)  & 0xFF) / 255.0f,
        static_cast<float>((c >> 16) & 0xFF) / 255.0f,
        static_cast<float>((c >> 24) & 0xFF) / 255.0f);
}

void ApplyCommonStyle(float rounding_window, float rounding_frame, float scrollbar_size) {
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = rounding_window;
    style.ChildRounding     = rounding_window;
    style.PopupRounding     = rounding_frame;
    style.FrameRounding     = rounding_frame;
    style.GrabRounding      = rounding_frame;
    style.TabRounding       = rounding_frame;
    style.ScrollbarRounding = rounding_frame;
    style.ScrollbarSize     = scrollbar_size;

    style.FrameBorderSize  = 1.0f;
    style.WindowBorderSize = 1.0f;
    style.PopupBorderSize  = 1.0f;
    style.ChildBorderSize  = 1.0f;
    style.TabBorderSize    = 1.0f;

    style.WindowPadding     = ImVec2(10.0f, 10.0f);
    style.FramePadding      = ImVec2(10.0f, 5.0f);
    style.ItemSpacing       = ImVec2(8.0f, 5.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
    style.CellPadding       = ImVec2(6.0f, 4.0f);
    style.IndentSpacing     = 18.0f;
    style.GrabMinSize       = 12.0f;

    style.AntiAliasedLines = true;
    style.AntiAliasedFill  = true;
}

void SetColor(ImGuiCol idx, ImU32 c) {
    ImGui::GetStyle().Colors[idx] = U32ToVec4(c);
}

void SetColor(ImGuiCol idx, ImU32 c, uint8_t alpha) {
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

std::string find_ui_font(const char* requested_path) {
    if (requested_path && std::strlen(requested_path) > 0 && std::filesystem::exists(requested_path)) {
        return requested_path;
    }

    std::string p = project_font_path("Inter-Regular.ttf");
    if (!p.empty()) return p;

    p = project_font_path("Roboto-Medium.ttf");
    if (!p.empty()) return p;

    return std::string{};
}

std::string find_mono_font(const char* requested_path) {
    if (requested_path && std::strlen(requested_path) > 0 && std::filesystem::exists(requested_path)) {
        return requested_path;
    }

    std::string p = project_font_path("JetBrainsMono-Regular.ttf");
    if (!p.empty()) return p;

    p = project_font_path("Cousine-Regular.ttf");
    if (!p.empty()) return p;

    return std::string{};
}

} // namespace

// ---------------------------------------------------------------------------
// Dark Apply
// ---------------------------------------------------------------------------
void Dark::Apply() {
    ApplyCommonStyle(8.0f, 6.0f, 14.0f);

    SetColor(ImGuiCol_Text,                 TextPrimary);
    SetColor(ImGuiCol_TextDisabled,         TextSecondary);
    SetColor(ImGuiCol_TextSelectedBg,       Accent, 80);

    SetColor(ImGuiCol_WindowBg,             Bg0);
    SetColor(ImGuiCol_ChildBg,              Bg0);
    SetColor(ImGuiCol_PopupBg,              Bg1);

    SetColor(ImGuiCol_Border,               ColorFromHex(0x30363D));
    SetColor(ImGuiCol_BorderShadow,         ColorFromHex(0x000000), 80);

    SetColor(ImGuiCol_FrameBg,              Bg1);
    SetColor(ImGuiCol_FrameBgHovered,       Bg2);
    SetColor(ImGuiCol_FrameBgActive,        Bg3);

    SetColor(ImGuiCol_TitleBg,              Bg0);
    SetColor(ImGuiCol_TitleBgActive,        Bg1);
    SetColor(ImGuiCol_TitleBgCollapsed,     Bg0);

    SetColor(ImGuiCol_MenuBarBg,            Bg1);
    SetColor(ImGuiCol_ScrollbarBg,          Bg0);
    SetColor(ImGuiCol_ScrollbarGrab,        Bg3);
    SetColor(ImGuiCol_ScrollbarGrabHovered, Bg3, 220);
    SetColor(ImGuiCol_ScrollbarGrabActive,  Accent);

    SetColor(ImGuiCol_CheckMark,            Accent);
    SetColor(ImGuiCol_SliderGrab,           Bg3, 220);
    SetColor(ImGuiCol_SliderGrabActive,     Accent);

    SetColor(ImGuiCol_Button,               Bg2);
    SetColor(ImGuiCol_ButtonHovered,        Bg3);
    SetColor(ImGuiCol_ButtonActive,         Accent);

    SetColor(ImGuiCol_Header,               Bg1);
    SetColor(ImGuiCol_HeaderHovered,        Bg2);
    SetColor(ImGuiCol_HeaderActive,         Bg3);

    SetColor(ImGuiCol_Separator,            Bg3);
    SetColor(ImGuiCol_SeparatorHovered,     Accent, 180);
    SetColor(ImGuiCol_SeparatorActive,      Accent);

    SetColor(ImGuiCol_ResizeGrip,           Bg3, 80);
    SetColor(ImGuiCol_ResizeGripHovered,    Accent, 180);
    SetColor(ImGuiCol_ResizeGripActive,     Accent);

    SetColor(ImGuiCol_Tab,                  Bg1);
    SetColor(ImGuiCol_TabHovered,           Bg2);
    SetColor(ImGuiCol_TabActive,            Bg2);
    SetColor(ImGuiCol_TabUnfocused,         Bg0);
    SetColor(ImGuiCol_TabUnfocusedActive,   Bg1);

    SetColor(ImGuiCol_DockingPreview,       Accent, 120);
    SetColor(ImGuiCol_DockingEmptyBg,       Bg0, 80);

    SetColor(ImGuiCol_PlotLines,            Accent);
    SetColor(ImGuiCol_PlotLinesHovered,     Yellow);
    SetColor(ImGuiCol_PlotHistogram,        Accent);
    SetColor(ImGuiCol_PlotHistogramHovered, Yellow);

    SetColor(ImGuiCol_TableHeaderBg,        Bg1);
    SetColor(ImGuiCol_TableBorderStrong,    Bg3);
    SetColor(ImGuiCol_TableBorderLight,     Bg2);
    SetColor(ImGuiCol_TableRowBg,            Bg0, 0);
    SetColor(ImGuiCol_TableRowBgAlt,        Bg1, 80);

    SetColor(ImGuiCol_DragDropTarget,       Accent, 220);
    SetColor(ImGuiCol_NavHighlight,         Accent);
    SetColor(ImGuiCol_NavWindowingHighlight,Accent);
    SetColor(ImGuiCol_NavWindowingDimBg,    Bg0, 120);
    SetColor(ImGuiCol_ModalWindowDimBg,     Bg0, 120);
}

// ---------------------------------------------------------------------------
// Light Apply
// ---------------------------------------------------------------------------
void Light::Apply() {
    ApplyCommonStyle(8.0f, 6.0f, 14.0f);

    SetColor(ImGuiCol_Text,                 TextPrimary);
    SetColor(ImGuiCol_TextDisabled,         TextSecondary);
    SetColor(ImGuiCol_TextSelectedBg,       Accent, 60);

    SetColor(ImGuiCol_WindowBg,             Bg0);
    SetColor(ImGuiCol_ChildBg,              Bg0);
    SetColor(ImGuiCol_PopupBg,              Bg0);

    SetColor(ImGuiCol_Border,               ColorFromHex(0xD0D7DE));
    SetColor(ImGuiCol_BorderShadow,         ColorFromHex(0x000000), 40);

    SetColor(ImGuiCol_FrameBg,              Bg1);
    SetColor(ImGuiCol_FrameBgHovered,       Bg2);
    SetColor(ImGuiCol_FrameBgActive,        Bg3);

    SetColor(ImGuiCol_TitleBg,              Bg1);
    SetColor(ImGuiCol_TitleBgActive,        Bg2);
    SetColor(ImGuiCol_TitleBgCollapsed,     Bg1);

    SetColor(ImGuiCol_MenuBarBg,            Bg1);
    SetColor(ImGuiCol_ScrollbarBg,          Bg1);
    SetColor(ImGuiCol_ScrollbarGrab,        Bg3);
    SetColor(ImGuiCol_ScrollbarGrabHovered, Bg3, 220);
    SetColor(ImGuiCol_ScrollbarGrabActive,  Accent);

    SetColor(ImGuiCol_CheckMark,            Accent);
    SetColor(ImGuiCol_SliderGrab,           Bg3, 220);
    SetColor(ImGuiCol_SliderGrabActive,     Accent);

    SetColor(ImGuiCol_Button,               Bg2);
    SetColor(ImGuiCol_ButtonHovered,        Bg3);
    SetColor(ImGuiCol_ButtonActive,         Accent);

    SetColor(ImGuiCol_Header,               Bg1);
    SetColor(ImGuiCol_HeaderHovered,        Bg2);
    SetColor(ImGuiCol_HeaderActive,         Bg3);

    SetColor(ImGuiCol_Separator,            Bg3);
    SetColor(ImGuiCol_SeparatorHovered,     Accent, 180);
    SetColor(ImGuiCol_SeparatorActive,      Accent);

    SetColor(ImGuiCol_ResizeGrip,           Bg3, 80);
    SetColor(ImGuiCol_ResizeGripHovered,    Accent, 180);
    SetColor(ImGuiCol_ResizeGripActive,     Accent);

    SetColor(ImGuiCol_Tab,                  Bg1);
    SetColor(ImGuiCol_TabHovered,           Bg2);
    SetColor(ImGuiCol_TabActive,            Bg2);
    SetColor(ImGuiCol_TabUnfocused,         Bg1);
    SetColor(ImGuiCol_TabUnfocusedActive,   Bg1);

    SetColor(ImGuiCol_DockingPreview,       Accent, 120);
    SetColor(ImGuiCol_DockingEmptyBg,       Bg0, 80);

    SetColor(ImGuiCol_PlotLines,            Accent);
    SetColor(ImGuiCol_PlotLinesHovered,     Yellow);
    SetColor(ImGuiCol_PlotHistogram,        Accent);
    SetColor(ImGuiCol_PlotHistogramHovered, Yellow);

    SetColor(ImGuiCol_TableHeaderBg,        Bg1);
    SetColor(ImGuiCol_TableBorderStrong,    Bg3);
    SetColor(ImGuiCol_TableBorderLight,     Bg2);
    SetColor(ImGuiCol_TableRowBg,            Bg0, 0);
    SetColor(ImGuiCol_TableRowBgAlt,        Bg1, 60);

    SetColor(ImGuiCol_DragDropTarget,       Accent, 220);
    SetColor(ImGuiCol_NavHighlight,         Accent);
    SetColor(ImGuiCol_NavWindowingHighlight,Accent);
    SetColor(ImGuiCol_NavWindowingDimBg,    Bg0, 80);
    SetColor(ImGuiCol_ModalWindowDimBg,     Bg0, 80);
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------
bool SelectableWithIndicator(const char* label,
                             bool selected,
                             ImU32 indicator_color,
                             ImGuiSelectableFlags flags,
                             const ImVec2& size_arg) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();

    bool clicked = ImGui::Selectable(label, selected, flags, size_arg);

    if (selected) {
        float h = ImGui::GetFrameHeight();
        float pad = 4.0f;
        draw_list->AddRectFilled(
            ImVec2(pos.x, pos.y + pad),
            ImVec2(pos.x + 3.0f, pos.y + h - pad),
            indicator_color, 1.5f);
    }
    return clicked;
}

void PropertyRow(const char* label, const std::function<void()>& content, float label_width) {
    if (ImGui::BeginTable("##property_row", 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, label_width);
        ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);

        ImGui::TableNextColumn();
        if (content) content();

        ImGui::EndTable();
    }
}

bool IconButton(const char* id, const char* icon_text, const ImVec2& size) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
    bool pressed = ImGui::Button(icon_text, size);
    ImGui::PopStyleColor(3);
    ImGui::PopID();
    return pressed;
}

// ---------------------------------------------------------------------------
// Style scaling
// ---------------------------------------------------------------------------
void ScaleStyle(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------
bool LoadFonts(const char* ui_font_path, float ui_size, const char* mono_font_path, float mono_size) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontAtlas* atlas = io.Fonts;

    std::string ui_path = find_ui_font(ui_font_path);
    if (ui_path.empty()) {
        GLOG_WARN("EngineTheme: no usable UI font found, using ImGui default");
        return false;
    }

    atlas->Clear();

    // UI 字体：Latin / Cyrillic / 通用标点
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

    ImFont* ui_font = atlas->AddFontFromFileTTF(ui_path.c_str(), ui_size, &ui_cfg, latin_ranges);
    if (!ui_font) {
        GLOG_ERROR("EngineTheme: failed to load UI font '{}'", ui_path);
        atlas->Clear();
        return false;
    }
    GLOG_INFO("EngineTheme: loaded UI font '{}' size={:.1f}", ui_path, ui_size);

    // CJK 字体回退：无论当前语言设置如何都合并中文字形，
    // 确保英文界面下也能正确渲染中文路径、名称等文本。
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

    // 等宽代码字体（不合并到 UI 字体）
    g_code_font = nullptr;
    std::string mono_path = find_mono_font(mono_font_path);
    if (!mono_path.empty()) {
        ImFontConfig mono_cfg{};
        mono_cfg.FontDataOwnedByAtlas = false;
        mono_cfg.MergeMode = false;
        mono_cfg.PixelSnapH = true;
        g_code_font = atlas->AddFontFromFileTTF(mono_path.c_str(), mono_size, &mono_cfg, latin_ranges);
        if (g_code_font) {
            GLOG_INFO("EngineTheme: loaded code font '{}' size={:.1f}", mono_path, mono_size);
        }
    }

    if (!atlas->Build()) {
        GLOG_ERROR("EngineTheme: failed to build font atlas");
        atlas->Clear();
        g_code_font = nullptr;
        return false;
    }

    io.FontDefault = ui_font;
    return true;
}

ImFont* CodeFont() {
    return g_code_font;
}

} // namespace EngineTheme
