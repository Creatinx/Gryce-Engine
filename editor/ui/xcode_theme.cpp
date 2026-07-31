#include "xcode_theme.h"

#include <imgui.h>

namespace gryce_engine::editor {

namespace {

constexpr ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

// Apple HIG 共用尺寸：扁平、克制描边、宽松留白
void apply_apple_sizes(ImGuiStyle& style) {
    style.WindowPadding      = ImVec2(12.0f, 10.0f);
    style.FramePadding       = ImVec2(8.0f, 5.0f);
    style.CellPadding        = ImVec2(6.0f, 4.0f);
    style.ItemSpacing        = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing   = ImVec2(6.0f, 4.0f);
    style.TouchExtraPadding  = ImVec2(0.0f, 0.0f);
    style.IndentSpacing      = 18.0f;
    style.ScrollbarSize      = 12.0f;
    style.GrabMinSize        = 10.0f;

    style.WindowRounding     = 8.0f;
    style.ChildRounding      = 6.0f;
    style.FrameRounding      = 5.0f;  // 按钮/输入框 ~5-6px
    style.PopupRounding      = 8.0f;
    style.ScrollbarRounding  = 9.0f;  // 细滚动条 + 圆头滑块
    style.GrabRounding       = 5.0f;
    style.TabRounding        = 6.0f;

    style.WindowBorderSize   = 1.0f;
    style.ChildBorderSize    = 1.0f;
    style.PopupBorderSize    = 1.0f;
    style.FrameBorderSize    = 0.0f;  // 暗色：扁平输入框无描边
    style.TabBorderSize      = 0.0f;
    style.TabBarBorderSize   = 1.0f;

    style.AntiAliasedFill    = true;
    style.AntiAliasedLines   = true;
    style.AntiAliasedLinesUseTex = true;
    style.DisabledAlpha      = 0.6f;
}

} // namespace

void apply_xcode_dark() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    apply_apple_sizes(style);

    // Xcode 暗色调色板
    const ImVec4 WindowBg      = rgb(0x25, 0x26, 0x2B);        // #25262B 面板/窗口
    const ImVec4 ContentBg     = rgb(0x1E, 0x1F, 0x24);        // #1E1F24 主内容区
    const ImVec4 InsetBg       = rgb(0x16, 0x17, 0x1B);        // #16171B 内嵌输入区
    const ImVec4 PopupBg       = rgb(0x2C, 0x2D, 0x33);        // 弹窗略亮于窗口
    const ImVec4 Accent        = rgb(0x0A, 0x84, 0xFF);        // #0A84FF Xcode 蓝
    const ImVec4 AccentHi      = rgb(0x40, 0x9C, 0xFF);        // #409CFF 高亮蓝
    const ImVec4 Separator     = rgb(0x3A, 0x3A, 0x3C, 0.6f);  // 细分隔线
    const ImVec4 TextPrimary   = rgb(0xE8, 0xE8, 0xE8);
    const ImVec4 TextDisabled  = rgb(0xFF, 0xFF, 0xFF, 0.4f);  // Apple 禁用态 ~40%
    const ImVec4 FrameHover    = rgb(0x32, 0x33, 0x39);
    const ImVec4 FrameActive   = rgb(0x3A, 0x3B, 0x42);
    const ImVec4 ButtonBg      = rgb(0x3A, 0x3B, 0x42);
    const ImVec4 ButtonHover   = rgb(0x4A, 0x4B, 0x54);

    // ---- 文本 ----
    c[ImGuiCol_Text]                  = TextPrimary;
    c[ImGuiCol_TextDisabled]          = TextDisabled;

    // ---- 背景 ----
    c[ImGuiCol_WindowBg]              = WindowBg;
    c[ImGuiCol_ChildBg]               = ContentBg;
    c[ImGuiCol_PopupBg]               = PopupBg;
    c[ImGuiCol_MenuBarBg]             = ContentBg;
    c[ImGuiCol_DockingEmptyBg]        = ContentBg;

    // ---- 边框 ----
    c[ImGuiCol_Border]                = Separator;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    // ---- 输入框 / 帧控件（内嵌深色、扁平）----
    c[ImGuiCol_FrameBg]               = InsetBg;
    c[ImGuiCol_FrameBgHovered]        = FrameHover;
    c[ImGuiCol_FrameBgActive]         = FrameActive;

    // ---- 标题栏 ----
    c[ImGuiCol_TitleBg]               = ContentBg;
    c[ImGuiCol_TitleBgActive]         = WindowBg;
    c[ImGuiCol_TitleBgCollapsed]      = ContentBg;

    // ---- 滚动条（细、低存在感的滑块）----
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0.15f);
    c[ImGuiCol_ScrollbarGrab]         = rgb(0x4A, 0x4B, 0x52);
    c[ImGuiCol_ScrollbarGrabHovered]  = rgb(0x5A, 0x5B, 0x63);
    c[ImGuiCol_ScrollbarGrabActive]   = rgb(0x6A, 0x6B, 0x74);

    // ---- 强调控件 ----
    c[ImGuiCol_CheckMark]             = Accent;
    c[ImGuiCol_SliderGrab]            = Accent;
    c[ImGuiCol_SliderGrabActive]      = AccentHi;
    c[ImGuiCol_NavHighlight]          = AccentHi;
    c[ImGuiCol_TextSelectedBg]        = ImVec4(Accent.x, Accent.y, Accent.z, 0.35f);

    // ---- 按钮 ----
    c[ImGuiCol_Button]                = ButtonBg;
    c[ImGuiCol_ButtonHovered]         = ButtonHover;
    c[ImGuiCol_ButtonActive]          = Accent;

    // ---- 可选项/树节点/表头（Header 系列）----
    c[ImGuiCol_Header]                = rgb(0x2E, 0x2F, 0x36);
    c[ImGuiCol_HeaderHovered]         = rgb(0x3A, 0x3B, 0x43);
    c[ImGuiCol_HeaderActive]          = ImVec4(Accent.x, Accent.y, Accent.z, 0.55f);

    // ---- 分隔线 ----
    c[ImGuiCol_Separator]             = Separator;
    c[ImGuiCol_SeparatorHovered]      = Accent;
    c[ImGuiCol_SeparatorActive]       = AccentHi;

    // ---- 角落调整手柄 ----
    c[ImGuiCol_ResizeGrip]            = rgb(0x4A, 0x4B, 0x52, 0.4f);
    c[ImGuiCol_ResizeGripHovered]     = rgb(0x5A, 0x5B, 0x63, 0.6f);
    c[ImGuiCol_ResizeGripActive]      = Accent;

    // ---- 标签页（Xcode：非活动标签压暗，活动标签贴近内容色）----
    c[ImGuiCol_Tab]                   = rgb(0x1B, 0x1C, 0x21);
    c[ImGuiCol_TabHovered]            = rgb(0x3A, 0x3B, 0x43);
    c[ImGuiCol_TabActive]             = rgb(0x32, 0x33, 0x39);
    c[ImGuiCol_TabUnfocused]          = rgb(0x1B, 0x1C, 0x21);
    c[ImGuiCol_TabUnfocusedActive]    = rgb(0x25, 0x26, 0x2B);

    // ---- 表格 ----
    c[ImGuiCol_TableHeaderBg]         = ContentBg;
    c[ImGuiCol_TableBorderStrong]     = Separator;
    c[ImGuiCol_TableBorderLight]      = rgb(0x3A, 0x3A, 0x3C, 0.4f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = rgb(0xFF, 0xFF, 0xFF, 0.03f);

    // ---- 拖拽 / 停靠 ----
    c[ImGuiCol_DragDropTarget]        = AccentHi;
    c[ImGuiCol_DockingPreview]        = ImVec4(Accent.x, Accent.y, Accent.z, 0.7f);

    // ---- 其他 ----
    c[ImGuiCol_PlotLines]             = Accent;
    c[ImGuiCol_PlotLinesHovered]      = AccentHi;
    c[ImGuiCol_PlotHistogram]         = Accent;
    c[ImGuiCol_PlotHistogramHovered]  = AccentHi;
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.5f);
}

void apply_xcode_light() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* c = style.Colors;

    apply_apple_sizes(style);
    // 亮色下输入框用细描边勾勒（Apple HIG 亮色外观）
    style.FrameBorderSize = 1.0f;

    // Xcode 亮色调色板
    const ImVec4 WindowBg      = rgb(0xF5, 0xF5, 0xF7);        // #F5F5F7 面板/窗口
    const ImVec4 ContentBg     = rgb(0xFF, 0xFF, 0xFF);        // #FFFFFF 主内容区
    const ImVec4 InsetBg       = rgb(0xFA, 0xFA, 0xFA);        // #FAFAFA 内嵌输入区
    const ImVec4 PopupBg       = rgb(0xFF, 0xFF, 0xFF);
    const ImVec4 Accent        = rgb(0x0A, 0x84, 0xFF);        // #0A84FF
    const ImVec4 AccentHi      = rgb(0x40, 0x9C, 0xFF);        // #409CFF
    const ImVec4 Border        = rgb(0xD1, 0xD1, 0xD6);        // 细描边 #D1D1D6
    const ImVec4 Separator     = rgb(0xD1, 0xD1, 0xD6, 0.7f);
    const ImVec4 TextPrimary   = rgb(0x1D, 0x1D, 0x1F);        // #1D1D1F
    const ImVec4 TextDisabled  = rgb(0x1D, 0x1D, 0x1F, 0.4f);
    const ImVec4 FrameHover    = rgb(0xF2, 0xF2, 0xF7);
    const ImVec4 FrameActive   = rgb(0xE5, 0xE5, 0xEA);
    const ImVec4 ButtonBg      = rgb(0xFF, 0xFF, 0xFF);
    const ImVec4 ButtonHover   = rgb(0xF2, 0xF2, 0xF7);

    // ---- 文本 ----
    c[ImGuiCol_Text]                  = TextPrimary;
    c[ImGuiCol_TextDisabled]          = TextDisabled;

    // ---- 背景 ----
    c[ImGuiCol_WindowBg]              = WindowBg;
    c[ImGuiCol_ChildBg]               = ContentBg;
    c[ImGuiCol_PopupBg]               = PopupBg;
    c[ImGuiCol_MenuBarBg]             = WindowBg;
    c[ImGuiCol_DockingEmptyBg]        = WindowBg;

    // ---- 边框 ----
    c[ImGuiCol_Border]                = Border;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);

    // ---- 输入框 / 帧控件 ----
    c[ImGuiCol_FrameBg]               = InsetBg;
    c[ImGuiCol_FrameBgHovered]        = FrameHover;
    c[ImGuiCol_FrameBgActive]         = FrameActive;

    // ---- 标题栏 ----
    c[ImGuiCol_TitleBg]               = rgb(0xEC, 0xEC, 0xF0);
    c[ImGuiCol_TitleBgActive]         = WindowBg;
    c[ImGuiCol_TitleBgCollapsed]      = rgb(0xEC, 0xEC, 0xF0);

    // ---- 滚动条 ----
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0.05f);
    c[ImGuiCol_ScrollbarGrab]         = rgb(0xAE, 0xAE, 0xB2);
    c[ImGuiCol_ScrollbarGrabHovered]  = rgb(0x8E, 0x8E, 0x93);
    c[ImGuiCol_ScrollbarGrabActive]   = rgb(0x6E, 0x6E, 0x73);

    // ---- 强调控件 ----
    c[ImGuiCol_CheckMark]             = Accent;
    c[ImGuiCol_SliderGrab]            = Accent;
    c[ImGuiCol_SliderGrabActive]      = AccentHi;
    c[ImGuiCol_NavHighlight]          = AccentHi;
    c[ImGuiCol_TextSelectedBg]        = ImVec4(Accent.x, Accent.y, Accent.z, 0.3f);

    // ---- 按钮 ----
    c[ImGuiCol_Button]                = ButtonBg;
    c[ImGuiCol_ButtonHovered]         = ButtonHover;
    c[ImGuiCol_ButtonActive]          = Accent;

    // ---- Header 系列 ----
    c[ImGuiCol_Header]                = rgb(0xE5, 0xE5, 0xEA);
    c[ImGuiCol_HeaderHovered]         = rgb(0xF2, 0xF2, 0xF7);
    c[ImGuiCol_HeaderActive]          = ImVec4(Accent.x, Accent.y, Accent.z, 0.5f);

    // ---- 分隔线 ----
    c[ImGuiCol_Separator]             = Separator;
    c[ImGuiCol_SeparatorHovered]      = Accent;
    c[ImGuiCol_SeparatorActive]       = AccentHi;

    // ---- 角落调整手柄 ----
    c[ImGuiCol_ResizeGrip]            = rgb(0xAE, 0xAE, 0xB2, 0.4f);
    c[ImGuiCol_ResizeGripHovered]     = rgb(0x8E, 0x8E, 0x93, 0.6f);
    c[ImGuiCol_ResizeGripActive]      = Accent;

    // ---- 标签页 ----
    c[ImGuiCol_Tab]                   = rgb(0xEC, 0xEC, 0xF0);
    c[ImGuiCol_TabHovered]            = rgb(0xF9, 0xF9, 0xFB);
    c[ImGuiCol_TabActive]             = rgb(0xFF, 0xFF, 0xFF);
    c[ImGuiCol_TabUnfocused]          = rgb(0xEC, 0xEC, 0xF0);
    c[ImGuiCol_TabUnfocusedActive]    = rgb(0xF5, 0xF5, 0xF7);

    // ---- 表格 ----
    c[ImGuiCol_TableHeaderBg]         = rgb(0xF5, 0xF5, 0xF7);
    c[ImGuiCol_TableBorderStrong]     = Border;
    c[ImGuiCol_TableBorderLight]      = rgb(0xD1, 0xD1, 0xD6, 0.5f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = rgb(0x00, 0x00, 0x00, 0.02f);

    // ---- 拖拽 / 停靠 ----
    c[ImGuiCol_DragDropTarget]        = AccentHi;
    c[ImGuiCol_DockingPreview]        = ImVec4(Accent.x, Accent.y, Accent.z, 0.7f);

    // ---- 其他 ----
    c[ImGuiCol_PlotLines]             = Accent;
    c[ImGuiCol_PlotLinesHovered]      = AccentHi;
    c[ImGuiCol_PlotHistogram]         = Accent;
    c[ImGuiCol_PlotHistogramHovered]  = AccentHi;
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.3f);
}

} // namespace gryce_engine::editor
