#pragma once

// Blender 4.x Dark 主题 — 扁平、低对比深灰、蓝色强调色。
// 参考 Blender 默认界面主题：无圆角、深灰分区、选中/强调用蓝、
// 活动 Tab 用蓝色顶线（overline）而非整块填充。

#include <imgui.h>

namespace gryce_engine::editor {

namespace blender {

inline constexpr ImU32 Color(uint32_t a, uint32_t r, uint32_t g, uint32_t b) {
    return (a << 24) | (b << 0) | (g << 8) | (r << 16);
}

// === 基础色 ===
// 窗口/主区域
inline constexpr ImU32 BG_DARKEST     = Color(0xFF, 0x1F, 0x1F, 0x1F);
// 菜单栏 / 标题栏 / 分区头
inline constexpr ImU32 BG_DARK        = Color(0xFF, 0x28, 0x28, 0x28);
// 面板 / 子窗口
inline constexpr ImU32 BG_MID         = Color(0xFF, 0x2D, 0x2D, 0x2D);
// 控件底
inline constexpr ImU32 BG_LIGHT       = Color(0xFF, 0x31, 0x31, 0x31);
inline constexpr ImU32 BG_INPUT       = Color(0xFF, 0x3A, 0x3A, 0x3A);
inline constexpr ImU32 BG_INPUT_HOVER = Color(0xFF, 0x44, 0x44, 0x44);
inline constexpr ImU32 BG_INPUT_ACTIVE= Color(0xFF, 0x4A, 0x4A, 0x4A);
inline constexpr ImU32 BG_BUTTON      = Color(0xFF, 0x3A, 0x3A, 0x3A);
inline constexpr ImU32 BG_BUTTON_HOVER= Color(0xFF, 0x46, 0x46, 0x46);
inline constexpr ImU32 BG_BUTTON_ACT  = Color(0xFF, 0x4E, 0x4E, 0x4E);

// === 装饰色 ===
inline constexpr ImU32 BORDER         = Color(0xFF, 0x12, 0x12, 0x12);
inline constexpr ImU32 BORDER_LIGHT   = Color(0xFF, 0x4A, 0x4A, 0x4A);

// === 强调色 ===
inline constexpr ImU32 BLUE_ACCENT    = Color(0xFF, 0x4C, 0x9A, 0xFF);
inline constexpr ImU32 BLUE_HOVER     = Color(0xFF, 0x6C, 0xAE, 0xFF);
inline constexpr ImU32 BLUE_ACTIVE    = Color(0xFF, 0x3B, 0x82, 0xE0);

// === 文字 ===
inline constexpr ImU32 TEXT_MAIN      = Color(0xFF, 0xF0, 0xF0, 0xF0);
inline constexpr ImU32 TEXT_DISABLED  = Color(0xFF, 0x5C, 0x5C, 0x5C);
inline constexpr ImU32 TEXT_SELECTED  = Color(0xFF, 0xFF, 0xFF, 0xFF);

// === 滚动条 ===
inline constexpr ImU32 SCROLL_BG      = Color(0xFF, 0x28, 0x28, 0x28);
inline constexpr ImU32 SCROLL_GRAB    = Color(0xFF, 0x4A, 0x4A, 0x4A);
inline constexpr ImU32 SCROLL_GRAB_H  = Color(0xFF, 0x5A, 0x5A, 0x5A);
inline constexpr ImU32 SCROLL_GRAB_A  = Color(0xFF, 0x6A, 0x6A, 0x6A);

// === 状态色 ===
inline constexpr ImU32 CHECK_MARK     = Color(0xFF, 0x4C, 0x9A, 0xFF);
inline constexpr ImU32 SLIDER_GRAB    = Color(0xFF, 0x4C, 0x9A, 0xFF);
inline constexpr ImU32 SEPARATOR      = Color(0xFF, 0x18, 0x18, 0x18);
inline constexpr ImU32 RESIZE_GRIP    = Color(0xFF, 0x4A, 0x4A, 0x4A);

// === Tab ===
inline constexpr ImU32 TAB_BG         = Color(0xFF, 0x28, 0x28, 0x28);
inline constexpr ImU32 TAB_HOVER      = Color(0xFF, 0x38, 0x38, 0x38);
inline constexpr ImU32 TAB_ACTIVE     = Color(0xFF, 0x3A, 0x3A, 0x3A);
inline constexpr ImU32 TAB_UNFOCUSED  = Color(0xFF, 0x2D, 0x2D, 0x2D);

// === 特殊 ===
inline constexpr ImU32 DOCK_EMPTY     = Color(0xFF, 0x22, 0x22, 0x22);
inline constexpr ImU32 MODAL_DIM      = Color(0x73, 0x00, 0x00, 0x00);

} // namespace blender

inline void StyleColorsBlender() {
    ImGuiStyle* style = &ImGui::GetStyle();

    // === 圆角：Blender 是扁平风格，几乎无圆角 ===
    style->WindowRounding        = 0.0f;
    style->ChildRounding         = 0.0f;
    style->FrameRounding         = 2.0f;
    style->PopupRounding         = 2.0f;
    style->ScrollbarRounding     = 2.0f;
    style->GrabRounding          = 2.0f;
    style->TabRounding           = 2.0f;
    style->TabBorderSize         = 0.0f;

    // === 边框 ===
    style->WindowBorderSize      = 0.0f;
    style->ChildBorderSize       = 0.0f;
    style->PopupBorderSize       = 1.0f;
    style->FrameBorderSize       = 0.0f;
    style->ScrollbarSize         = 14.0f;

    // === 间距与内边距（Blender 紧凑布局） ===
    style->FramePadding          = ImVec2(8.0f, 4.0f);
    style->ItemSpacing           = ImVec2(8.0f, 4.0f);
    style->ItemInnerSpacing      = ImVec2(6.0f, 4.0f);
    style->IndentSpacing         = 20.0f;
    style->GrabMinSize           = 10.0f;
    style->WindowPadding         = ImVec2(10.0f, 10.0f);
    style->CellPadding           = ImVec2(4.0f, 2.0f);

    // === 颜色 ===
    ImVec4* c = style->Colors;

    c[ImGuiCol_Text]                 = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.36f, 0.36f, 0.36f, 1.00f);
    c[ImGuiCol_TextLink]             = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);

    c[ImGuiCol_WindowBg]             = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_ChildBg]              = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_PopupBg]              = ImVec4(0.20f, 0.20f, 0.20f, 0.98f);

    c[ImGuiCol_Border]               = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    c[ImGuiCol_FrameBg]              = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
    c[ImGuiCol_FrameBgHovered]       = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);

    c[ImGuiCol_TitleBg]              = ImVec4(0.12f, 0.12f, 0.12f, 1.00f);
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.11f, 0.11f, 0.11f, 1.00f);

    c[ImGuiCol_MenuBarBg]            = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);

    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.35f, 0.35f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.42f, 0.42f, 0.42f, 1.00f);

    c[ImGuiCol_Button]               = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.27f, 0.27f, 0.27f, 1.00f);
    c[ImGuiCol_ButtonActive]         = ImVec4(0.31f, 0.31f, 0.31f, 1.00f);

    // 折叠头/面板头：Blender 面板头为中性灰，选中态才用蓝
    c[ImGuiCol_Header]               = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.24f, 0.24f, 0.24f, 1.00f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.18f, 0.27f, 0.42f, 1.00f);

    c[ImGuiCol_Separator]            = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);
    c[ImGuiCol_SeparatorHovered]     = ImVec4(0.30f, 0.60f, 1.00f, 0.55f);
    c[ImGuiCol_SeparatorActive]      = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);

    c[ImGuiCol_ResizeGrip]           = ImVec4(0.29f, 0.29f, 0.29f, 1.00f);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.30f, 0.60f, 1.00f, 0.70f);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);

    // Tab：深灰底，选中项用蓝色顶线（Blender 风格），不用整块蓝填充
    c[ImGuiCol_Tab]                  = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]  = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
    c[ImGuiCol_TabDimmed]            = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.30f, 0.60f, 1.00f, 0.55f);
    c[ImGuiCol_TabActive]            = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);
    c[ImGuiCol_TabUnfocused]         = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive]   = ImVec4(0.23f, 0.23f, 0.23f, 1.00f);

    c[ImGuiCol_CheckMark]            = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]           = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive]     = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);

    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.30f, 0.60f, 1.00f, 0.30f);

    // 表格（层级/检查器列表）
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.02f);

    c[ImGuiCol_DragDropTarget]       = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);

    c[ImGuiCol_NavHighlight]         = ImVec4(0.30f, 0.60f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(1.00f, 1.00f, 1.00f, 0.60f);
    c[ImGuiCol_NavWindowingDimBg]    = ImVec4(0.00f, 0.00f, 0.00f, 0.40f);

    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.45f);

    c[ImGuiCol_DockingPreview]       = ImVec4(0.30f, 0.60f, 1.00f, 0.50f);
    c[ImGuiCol_DockingEmptyBg]       = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);

    c[ImGuiCol_PlotLines]            = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]     = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
    c[ImGuiCol_PlotHistogram]        = ImVec4(0.30f, 0.60f, 1.00f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered] = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
}

} // namespace gryce_engine::editor
