#pragma once

// 统一编辑器主题入口：Dark（Blender 风格暗色）/ Light（浅色）。
// 浅色主题基于 Blender 风格结构映射，浅灰背景 + 深色文字 + 蓝强调色。

#include <cstdint>

#include <imgui.h>

#include "blender_theme.h"

namespace gryce_engine::editor {

enum class EditorTheme : int {
    Dark = 0,
    Light = 1
};

inline const char* EditorThemeName(EditorTheme theme) {
    switch (theme) {
    case EditorTheme::Light: return "Light";
    case EditorTheme::Dark:
    default:                 return "Dark";
    }
}

// 浅色主题：浅灰/米白背景、深灰文字、蓝强调色。
inline void StyleColorsLight() {
    ImGuiStyle* style = &ImGui::GetStyle();

    // 沿用 Blender 的扁平布局与间隔，仅替换配色。
    style->WindowRounding       = 0.0f;
    style->ChildRounding        = 0.0f;
    style->FrameRounding        = 2.0f;
    style->PopupRounding        = 2.0f;
    style->ScrollbarRounding    = 2.0f;
    style->GrabRounding         = 2.0f;
    style->TabRounding          = 2.0f;
    style->TabBorderSize        = 0.0f;
    style->WindowBorderSize     = 0.0f;
    style->ChildBorderSize      = 0.0f;
    style->PopupBorderSize      = 1.0f;
    style->FrameBorderSize      = 0.0f;
    style->ScrollbarSize        = 14.0f;
    style->FramePadding         = ImVec2(8.0f, 4.0f);
    style->ItemSpacing          = ImVec2(8.0f, 4.0f);
    style->ItemInnerSpacing     = ImVec2(6.0f, 4.0f);
    style->IndentSpacing        = 20.0f;
    style->GrabMinSize          = 10.0f;
    style->WindowPadding        = ImVec2(10.0f, 10.0f);
    style->CellPadding          = ImVec2(4.0f, 2.0f);

    ImVec4* c = style->Colors;

    c[ImGuiCol_Text]                 = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.48f, 0.48f, 0.50f, 1.00f);
    c[ImGuiCol_TextLink]             = ImVec4(0.16f, 0.45f, 0.85f, 1.00f);

    c[ImGuiCol_WindowBg]             = ImVec4(0.93f, 0.93f, 0.93f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.97f, 0.97f, 0.97f, 0.98f);

    c[ImGuiCol_Border]               = ImVec4(0.72f, 0.72f, 0.74f, 1.00f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_FrameBg]              = ImVec4(0.88f, 0.88f, 0.89f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.83f, 0.85f, 0.88f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.78f, 0.82f, 0.88f, 1.00f);

    c[ImGuiCol_TitleBg]              = ImVec4(0.86f, 0.86f, 0.87f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.80f, 0.82f, 0.85f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);

    c[ImGuiCol_MenuBarBg]            = ImVec4(0.88f, 0.88f, 0.89f, 1.00f);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.88f, 0.88f, 0.89f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.70f, 0.70f, 0.72f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.60f, 0.62f, 0.66f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.48f, 0.55f, 0.65f, 1.00f);

    c[ImGuiCol_Button]               = ImVec4(0.84f, 0.84f, 0.85f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.76f, 0.82f, 0.90f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.65f, 0.75f, 0.88f, 1.00f);

    c[ImGuiCol_Header]               = ImVec4(0.82f, 0.84f, 0.87f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.74f, 0.80f, 0.88f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.66f, 0.75f, 0.88f, 1.00f);

    c[ImGuiCol_Separator]            = ImVec4(0.76f, 0.76f, 0.78f, 1.00f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.40f, 0.60f, 0.85f, 0.60f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);

    c[ImGuiCol_ResizeGrip]           = ImVec4(0.70f, 0.70f, 0.72f, 1.00f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.40f, 0.60f, 0.85f, 0.70f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);

    c[ImGuiCol_Tab]                  = ImVec4(0.86f, 0.86f, 0.88f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.76f, 0.78f, 0.82f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]  = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
    c[ImGuiCol_TabDimmed]            = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.84f, 0.84f, 0.86f, 1.00f);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.40f, 0.60f, 0.85f, 0.55f);
    c[ImGuiCol_TabActive]            = ImVec4(0.82f, 0.84f, 0.87f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.88f, 0.88f, 0.90f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.84f, 0.84f, 0.86f, 1.00f);

    c[ImGuiCol_CheckMark]            = ImVec4(0.16f, 0.45f, 0.85f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.35f, 0.55f, 0.80f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.25f, 0.48f, 0.80f, 1.00f);

    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.30f, 0.55f, 0.85f, 0.30f);

    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.84f, 0.84f, 0.86f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.72f, 0.72f, 0.74f, 1.00f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.80f, 0.80f, 0.82f, 1.00f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);

    c[ImGuiCol_DragDropTarget]       = ImVec4(0.20f, 0.55f, 0.90f, 0.90f);

    c[ImGuiCol_NavHighlight]         = ImVec4(0.30f, 0.55f, 0.85f, 0.70f);
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(0.40f, 0.40f, 0.40f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.00f, 0.00f, 0.00f, 0.20f);

    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.35f);

    c[ImGuiCol_DockingPreview]       = ImVec4(0.30f, 0.55f, 0.85f, 0.50f);
    c[ImGuiCol_DockingEmptyBg]       = ImVec4(0.86f, 0.86f, 0.88f, 1.00f);

    c[ImGuiCol_PlotLines]            = ImVec4(0.55f, 0.55f, 0.57f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]     = ImVec4(0.25f, 0.48f, 0.80f, 1.00f);
    c[ImGuiCol_PlotHistogram]        = ImVec4(0.30f, 0.55f, 0.85f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.20f, 0.45f, 0.80f, 1.00f);
}

inline void ApplyEditorTheme(EditorTheme theme) {
    switch (theme) {
    case EditorTheme::Light: StyleColorsLight(); break;
    case EditorTheme::Dark:
    default:                 StyleColorsBlender(); break;
    }
}

} // namespace gryce_engine::editor