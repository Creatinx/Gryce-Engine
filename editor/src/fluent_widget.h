#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include "fluent_theme.h"

// ---------------------------------------------------------------------------
// WidgetState — 组件通用交互状态
// ---------------------------------------------------------------------------
struct WidgetState
{
    bool hovered  = false;
    bool held     = false;
    bool focused  = false;
    bool disabled = false;
    bool pressed  = false;
    bool toggled  = false;
};

// ---------------------------------------------------------------------------
// FluentWidget — Fluent 组件基类
// 抽出所有组件共用的 boilerplate（布局 / 状态 / 动画 / 绘制工具），
// 子类只需实现 Measure（测量）与 Render（绘制）。
// ---------------------------------------------------------------------------
class FluentWidget
{
public:
    virtual ~FluentWidget() = default;

    // 唯一对外入口
    bool Draw(const char* label, const FluentTheme& theme)
    {
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window->SkipItems) return false;

        draw       = window->DrawList;
        label_size = ImGui::CalcTextSize(label);

        // 1. 测量
        ImVec2 size = Measure(label);
        ImVec2 pos  = window->DC.CursorPos;
        bb = ImRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));

        // 2. 布局 + ID
        ImGui::ItemSize(bb, ImGui::GetStyle().FramePadding.y);
        id = window->GetID(label);
        if (!ImGui::ItemAdd(bb, id)) return false;

        // 3. 状态
        bool hovered, held;
        bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, 0);

        state.hovered  = hovered;
        state.held     = held;
        state.pressed  = pressed;
        state.focused  = ImGui::IsItemFocused();
        state.disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;

        // 4. 切换类控件
        if (pressed && IsToggleable())
        {
            bool* v = GetToggleValue();
            if (v) { *v = !(*v); ImGui::MarkItemEdited(id); state.toggled = true; }
        }

        // 5. 渲染
        ImGui::RenderNavHighlight(bb, id);
        Render(label, theme);

        return pressed;
    }

protected:
    // ========== 子类必须实现 ==========
    virtual ImVec2 Measure(const char* label) = 0;
    virtual void   Render(const char* label, const FluentTheme& theme) = 0;

    // ========== 子类可选重写 ==========
    virtual bool  IsToggleable() const { return false; }
    virtual bool* GetToggleValue()     { return nullptr; }
    virtual void  PostDrawState(const FluentTheme&) {}

    // ========== 生命周期内可用 ==========
    ImGuiID       id         = 0;
    ImRect        bb;
    WidgetState   state;
    ImDrawList*   draw       = nullptr;
    ImVec2        label_size = ImVec2(0, 0);

    // ========== 绘制工具 ==========
    void FillRect(const ImRect& r, ImU32 col, float rounding = -1.0f)
    {
        float rd = (rounding < 0) ? ImGui::GetStyle().FrameRounding : rounding;
        draw->AddRectFilled(r.Min, r.Max, col, rd);
    }

    void StrokeRect(const ImRect& r, ImU32 col, float rounding = -1.0f, float thickness = 1.0f)
    {
        float rd = (rounding < 0) ? ImGui::GetStyle().FrameRounding : rounding;
        draw->AddRect(r.Min, r.Max, col, rd, 0, thickness);
    }

    void Shadow(const ImRect& r, float rounding, float size, ImU32 col)
    {
        // ImGui 旧版无 AddShadowRect，用带透明度的圆角矩形衬底近似阴影。
        ImRect sr = r;
        sr.Min.y += size * 0.5f;
        draw->AddRectFilled(sr.Min, sr.Max, col, rounding);
    }

    void Text(const ImVec2& pos, ImU32 col, const char* txt)
    {
        draw->AddText(pos, col, txt);
    }

    void TextCentered(const ImRect& r, ImU32 col, const char* txt)
    {
        ImVec2 ts = ImGui::CalcTextSize(txt);
        float w = (r.Max.x - r.Min.x);
        float h = (r.Max.y - r.Min.y);
        ImVec2 p(r.Min.x + (w - ts.x) * 0.5f, r.Min.y + (h - ts.y) * 0.5f);
        p.x = (float)(int)p.x;
        p.y = (float)(int)p.y;
        draw->AddText(p, col, txt);
    }

    // ========== 颜色工具 ==========
    static ImU32 U32(const ImVec4& c) { return ImGui::GetColorU32(c); }

    ImU32 InterpCol(const ImVec4& a, const ImVec4& b, float t)
    {
        ImVec4 r(
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t
        );
        return ImGui::GetColorU32(r);
    }

    // 常用状态选择器：非激活 -> 激活 的颜色插值
    ImU32 StateColor(const FluentTheme& t,
                     const ImVec4& base,
                     const ImVec4& hovered,
                     const ImVec4& active,
                     float anim_t)
    {
        if (state.disabled) return U32(t.text_disabled);
        ImVec4 target = state.held   ? active
                      : state.hovered ? hovered
                      : base;
        return U32(target);
    }

    // ========== 动画 ==========
    float Anim(const char* key, float target, float speed = -1.0f)
    {
        if (speed < 0) speed = 12.0f;
        ImGuiStorage* storage = ImGui::GetStateStorage();
        ImGuiID k = ImHashStr(key, 0, id);
        float current = storage->GetFloat(k, target);
        float dt = ImGui::GetIO().DeltaTime;
        if (current != target)
        {
            float step = speed * dt;
            if (current < target) current = ImMin(current + step, target);
            else                  current = ImMax(current - step, target);
            storage->SetFloat(k, current);
        }
        return current;
    }
};