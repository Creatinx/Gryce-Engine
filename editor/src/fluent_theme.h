#pragma once
#include "imgui.h"

// ---------------------------------------------------------------------------
// FluentTheme — Fluent 风格主题色板，集中管理暗色/亮色配色。
// ---------------------------------------------------------------------------
struct FluentTheme
{
    // 文字
    ImVec4 text            = ImVec4(0.91f, 0.91f, 0.91f, 1.0f);
    ImVec4 text_disabled   = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);

    // 背景
    ImVec4 window_bg       = ImVec4(0.12f, 0.12f, 0.12f, 1.0f);
    ImVec4 child_bg        = ImVec4(0.12f, 0.12f, 0.12f, 1.0f);
    ImVec4 popup_bg        = ImVec4(0.17f, 0.17f, 0.17f, 1.0f);
    ImVec4 border          = ImVec4(0.24f, 0.24f, 0.24f, 1.0f);

    // 表面（按钮、输入框底）
    ImVec4 surface         = ImVec4(0.18f, 0.18f, 0.18f, 1.0f);
    ImVec4 surface_hovered = ImVec4(0.22f, 0.22f, 0.22f, 1.0f);
    ImVec4 surface_active  = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);

    // 强调色
    ImVec4 accent          = ImVec4(0.00f, 0.47f, 0.83f, 1.0f);
    ImVec4 accent_hovered  = ImVec4(0.10f, 0.53f, 0.85f, 1.0f);
    ImVec4 accent_active   = ImVec4(0.00f, 0.42f, 0.75f, 1.0f);

    // 危险色
    ImVec4 danger          = ImVec4(0.77f, 0.17f, 0.11f, 1.0f);
    ImVec4 danger_hovered  = ImVec4(0.82f, 0.20f, 0.22f, 1.0f);
    ImVec4 danger_active   = ImVec4(0.66f, 0.13f, 0.13f, 1.0f);

    // 通用尺寸
    float  rounding        = 6.0f;
    float  border_size     = 1.0f;
    float  padding_x       = 12.0f;
    float  padding_y       = 6.0f;
    float  height_default  = 30.0f;
    float  anim_speed      = 12.0f;

    static FluentTheme Dark()  { return FluentTheme(); }
    static FluentTheme Light()
    {
        FluentTheme t;
        t.text            = ImVec4(0.10f, 0.10f, 0.10f, 1.0f);
        t.text_disabled   = ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
        t.window_bg       = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
        t.child_bg        = ImVec4(0.95f, 0.95f, 0.95f, 1.0f);
        t.popup_bg        = ImVec4(1.00f, 1.00f, 1.00f, 1.0f);
        t.border          = ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
        t.surface         = ImVec4(0.98f, 0.98f, 0.98f, 1.0f);
        t.surface_hovered = ImVec4(0.93f, 0.93f, 0.93f, 1.0f);
        t.surface_active  = ImVec4(0.88f, 0.88f, 0.88f, 1.0f);
        return t;
    }
};

// ---------------------------------------------------------------------------
// 全局当前主题访问器：编辑器在切换主题时同步更新它，各面板统一取用它，
// 保证 Toolbar / Inspector / Settings 等所有面板配色一致。
// ---------------------------------------------------------------------------
inline FluentTheme& fluent_theme() {
    static FluentTheme t = FluentTheme::Dark();
    return t;
}