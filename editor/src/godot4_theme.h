#pragma once

// Godot 4.x Dark 主题 — 仿照 Godot 4.7 Editor 的 UI 配色方案。
// 特点：深灰背景 (#2B2B2B)、蓝色强调色 (#4C9AFF)、紧凑但宽松的间距。
// 参考：Godot 4.x Dark 调色板（ImU32 0xAABBGGRR）。

#include <imgui.h>

namespace gryce_engine::editor {

namespace godot4 {

inline constexpr ImU32 Color(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return (a << 24) | (b << 0) | (g << 8) | (r << 16);
}

// === 基础色（Godot 4.7 editor-dark）===
inline constexpr ImU32 BG_PANEL_DARK   = Color(0xFF, 0x1E, 0x1E, 0x1E); // #1E1E1E 最暗
inline constexpr ImU32 BG_PANEL        = Color(0xFF, 0x27, 0x27, 0x27); // #272727 面板
inline constexpr ImU32 BG_BASE         = Color(0xFF, 0x2E, 0x2E, 0x2E); // #2E2E2E 窗口背景
inline constexpr ImU32 BG_LIGHT        = Color(0xFF, 0x38, 0x38, 0x38); // #383838 浅背景（菜单/输入）
inline constexpr ImU32 BG_MENU_BAR     = Color(0xFF, 0x24, 0x24, 0x24); // #242424 菜单栏
inline constexpr ImU32 BG_INPUT        = Color(0xFF, 0x35, 0x35, 0x35); // 输入框
inline constexpr ImU32 BG_BUTTON       = Color(0xFF, 0x3C, 0x3C, 0x3C); // 按钮
inline constexpr ImU32 BG_BUTTON_H     = Color(0xFF, 0x48, 0x48, 0x48); // 按钮悬停
inline constexpr ImU32 BG_BUTTON_A     = Color(0xFF, 0x55, 0x55, 0x55); // 按钮按下

// === 强调色：Godot 蓝 ===
inline constexpr ImU32 ACCENT_BLUE     = Color(0xFF, 0x4C, 0x9A, 0xFF); // #4C9AFF
inline constexpr ImU32 ACCENT_BLUE_H   = Color(0xFF, 0x66, 0xA8, 0xFF);
inline constexpr ImU32 ACCENT_BLUE_A   = Color(0xFF, 0x33, 0x7F, 0xE5);
inline constexpr ImU32 ACCENT_HEADER   = Color(0xFF, 0x4C, 0x9A, 0xFF);

// === 边框与分隔线 ===
inline constexpr ImU32 BORDER          = Color(0xFF, 0x13, 0x13, 0x13);
inline constexpr ImU32 BORDER_LIGHT    = Color(0xFF, 0x4F, 0x4F, 0x4F);
inline constexpr ImU32 SEPARATOR       = Color(0xFF, 0x13, 0x13, 0x13);
inline constexpr ImU32 SEPARATOR_LIGHT = Color(0xFF, 0x4F, 0x4F, 0x4F);

// === 文本 ===
inline constexpr ImU32 TEXT_PRIMARY    = Color(0xFF, 0xF5, 0xF5, 0xF5);
inline constexpr ImU32 TEXT_SECONDARY  = Color(0xFF, 0xA0, 0xA0, 0xA0);
inline constexpr ImU32 TEXT_DISABLED   = Color(0xFF, 0x6D, 0x6D, 0x6D);
inline constexpr ImU32 TEXT_SELECTED   = Color(0xFF, 0xFF, 0xFF, 0xFF);

// === Tabs ===
inline constexpr ImU32 TAB_BG          = Color(0xFF, 0x2C, 0x2C, 0x2C);
inline constexpr ImU32 TAB_HOVER       = Color(0xFF, 0x3E, 0x3E, 0x3E);
inline constexpr ImU32 TAB_ACTIVE      = Color(0xFF, 0x4C, 0x9A, 0xFF);
inline constexpr ImU32 TAB_UNFOCUSED   = Color(0xFF, 0x38, 0x38, 0x38);

// === 滚动条 ===
inline constexpr ImU32 SCROLL_BG       = Color(0xFF, 0x27, 0x27, 0x27);
inline constexpr ImU32 SCROLL_GRAB     = Color(0xFF, 0x55, 0x55, 0x55);
inline constexpr ImU32 SCROLL_GRAB_H   = Color(0xFF, 0x66, 0x66, 0x66);
inline constexpr ImU32 SCROLL_GRAB_A   = Color(0xFF, 0x77, 0x77, 0x77);

// === 状态色 ===
inline constexpr ImU32 CHECK_MARK      = Color(0xFF, 0x4C, 0x9A, 0xFF);
inline constexpr ImU32 SLIDER_GRAB     = Color(0xFF, 0x4C, 0x9A, 0xFF);
inline constexpr ImU32 MODAL_DIM       = Color(0x55, 0x00, 0x00, 0x00);
inline constexpr ImU32 DOCK_EMPTY      = Color(0xFF, 0x22, 0x22, 0x22);

} // namespace godot4

inline void StyleColorsGodot4() {
    ImGuiStyle* style = &ImGui::GetStyle();

    // === Godot 风格：圆角（比 Blender 小但有） ===
    style->WindowRounding        = 6.0f;
    style->ChildRounding         = 4.0f;
    style->FrameRounding         = 4.0f;
    style->PopupRounding         = 4.0f;
    style->ScrollbarRounding     = 4.0f;
    style->GrabRounding          = 4.0f;
    style->TabRounding           = 4.0f;
    style->TabBorderSize         = 0.0f;

    // === 边框 ===
    style->WindowBorderSize      = 1.0f;
    style->ChildBorderSize       = 0.0f;
    style->PopupBorderSize       = 1.0f;
    style->FrameBorderSize       = 1.0f;
    style->ScrollbarSize         = 12.0f;

    // === 间距（Godot 紧凑+宽松平衡） ===
    style->FramePadding          = ImVec2(8, 4);
    style->WindowPadding         = ImVec2(8, 8);
    style->ItemSpacing           = ImVec2(8, 6);
    style->ItemInnerSpacing      = ImVec2(6, 4);
    style->ScrollbarSize         = 14.0f;
    style->GrabMinSize           = 10;
    style->IndentSpacing         = 20;

    // === Alpha ===
    style->Alpha                 = 1.00f;
    style->DisabledAlpha         = 0.55f;

    ImVec4* c = style->Colors;

    // === 基础面板 ===
    c[ImGuiCol_Text]                 = ImVec4(0.96f, 0.96f, 0.96f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.43f, 0.43f, 0.43f, 1.00f);
    c[ImGuiCol_WindowBg]             = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_Border]               = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // === Frame / Input ===
    c[ImGuiCol_FrameBg]              = ImVec4(0.21f, 0.21f, 0.21f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.28f, 0.28f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);

    // === Title ===
    c[ImGuiCol_TitleBg]              = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.18f, 0.20f, 0.23f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.14f, 0.14f, 0.14f, 0.60f);

    // === Menu Bar ===
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);

    // === Scrollbar ===
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.40f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.46f, 0.46f, 0.46f, 1.00f);

    // === Check / Slider ===
    c[ImGuiCol_CheckMark]            = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.33f, 0.65f, 1.00f, 1.00f);

    // === Button（Godot 中性背景 + 悬停微亮 + 按下深蓝）===
    c[ImGuiCol_Button]               = ImVec4(0.235f, 0.235f, 0.235f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.282f, 0.282f, 0.282f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.33f, 0.33f, 0.33f, 1.00f);

    // === Header（Collapsing / TreeNode — Godot 蓝色选中）===
    c[ImGuiCol_Header]               = ImVec4(0.30f, 0.60f, 1.00f, 0.35f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.30f, 0.60f, 1.00f, 0.50f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.30f, 0.60f, 1.00f, 0.55f);

    // === Separator ===
    c[ImGuiCol_Separator]            = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.30f, 0.60f, 1.00f, 0.40f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.30f, 0.60f, 1.00f, 0.55f);

    // === Resize Grip ===
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.30f, 0.60f, 1.00f, 0.15f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.30f, 0.60f, 1.00f, 0.40f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.30f, 0.60f, 1.00f, 0.55f);

    // === Tabs（Godot：无圆角选中蓝色，无选中时 tab 灰）===
    c[ImGuiCol_Tab]                  = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    c[ImGuiCol_TabActive]            = ImVec4(0.30f, 0.60f, 1.00f, 0.75f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.30f, 0.60f, 1.00f, 0.55f);

    // === Docking Preview ===
    c[ImGuiCol_DockingPreview]       = ImVec4(0.30f, 0.60f, 1.00f, 0.55f);
    c[ImGuiCol_DockingEmptyBg]       = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);

    // === Plot ===
    c[ImGuiCol_PlotLines]            = ImVec4(0.61f, 0.61f, 0.61f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]     = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    c[ImGuiCol_PlotHistogram]        = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);

    // === Table / Selection ===
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.30f, 0.60f, 1.00f, 0.40f);

    // === Drag / Modal ===
    c[ImGuiCol_DragDropTarget]       = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    c[ImGuiCol_NavHighlight]         = ImVec4(0.30f, 0.60f, 1.00f, 0.55f);
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);
}

} // namespace gryce_engine::editor
