#pragma once
#include <algorithm>
#include <cstring>
#include <cstdio>
#include "imgui.h"
#include "imgui_internal.h"
#include "fluent_widget.h"

// ---------------------------------------------------------------------------
// FluentButton — Fluent 风格按钮
// 状态决定背景色插值，accent=true 时使用强调色填充。
// ---------------------------------------------------------------------------
class FluentButton : public FluentWidget
{
public:
    bool accent = false; // 主操作按钮（强调色背景）
    float width  = 0.0f; // 可选固定宽度（0 = 按文本自适应）
    float height = 0.0f; // 可选固定高度（0 = 按文本自适应）

    FluentButton(bool accent = false) : accent(accent) {}

protected:
    ImVec2 Measure(const char* label) override
    {
        ImGuiStyle& s = ImGui::GetStyle();
        float h = (height > 0) ? height : (s.FramePadding.y * 2.0f + label_size.y);
        float w = (width  > 0) ? width  : (label_size.x + s.FramePadding.x * 2.0f);
        if (w < 0) w = 0;
        return ImVec2(w, h);
    }

    void Render(const char* label, const FluentTheme& t) override
    {
        const ImVec4* bg = accent ? &t.accent
                                   : (state.held   ? &t.surface_active
                                   : state.hovered ? &t.surface_hovered
                                                   : &t.surface);
        const ImVec4* bg_hover = accent ? &t.accent_hovered : &t.surface_hovered;
        float anim_t = Anim("hover", state.hovered || state.held ? 1.0f : 0.0f, t.anim_speed);

        ImU32 bg_col = InterpCol(accent ? t.accent : t.surface,
                                 state.held ? (accent ? t.accent_active : t.surface_active)
                                            : *bg_hover,
                                 anim_t);
        (void)bg;

        FillRect(bb, bg_col, t.rounding);

        ImU32 txt_col = accent ? U32(ImVec4(1, 1, 1, 1))
                               : U32(state.disabled ? t.text_disabled : t.text);
        TextCentered(bb, txt_col, label);
    }
};

// ---------------------------------------------------------------------------
// FluentToggle — Fluent 风格开关
// ---------------------------------------------------------------------------
class FluentToggle : public FluentWidget
{
public:
    bool* value = nullptr;
    FluentToggle(bool* v) : value(v) {}

protected:
    ImVec2 Measure(const char* label) override
    {
        float h = 22.0f;
        float w = h * 1.8f;
        float lw = label_size.x > 0 ? (8.0f + label_size.x) : 0.0f;
        return ImVec2(w + lw, h);
    }

    void Render(const char* label, const FluentTheme& t) override
    {
        float h  = bb.GetHeight();
        float w  = h * 1.8f;
        float rd = h * 0.5f;

        ImRect track(bb.Min, ImVec2(bb.Min.x + w, bb.Max.y));

        // 动画进度
        float prog = Anim("t", *value ? 1.0f : 0.0f, t.anim_speed);

        // 轨道颜色：从 surface 渐变到 accent
        ImU32 track_col = InterpCol(
            state.held ? t.surface_active : (state.hovered ? t.surface_hovered : t.surface),
            state.held ? t.accent_active  : (state.hovered ? t.accent_hovered  : t.accent),
            prog
        );
        FillRect(track, track_col, rd);

        // 圆形滑块
        float cx = ImLerp(track.Min.x + rd, track.Max.x - rd, prog);
        float cy = (track.Min.y + track.Max.y) * 0.5f;

        draw->AddCircleFilled(ImVec2(cx, cy + 1.0f), rd - 3.0f, IM_COL32(0, 0, 0, 50));
        draw->AddCircleFilled(ImVec2(cx, cy),        rd - 3.0f, IM_COL32(255, 255, 255, 255));

        // 标签
        if (label_size.x > 0.0f)
        {
            Text(
                ImVec2(track.Max.x + 8.0f, bb.Min.y + (h - label_size.y) * 0.5f),
                U32(state.disabled ? t.text_disabled : t.text),
                label
            );
        }
    }

    bool  IsToggleable() const override { return true; }
    bool* GetToggleValue()     override { return value; }
};

// ---------------------------------------------------------------------------
// FluentCheckbox — Fluent 风格复选框（方框 + 对勾）
// ---------------------------------------------------------------------------
class FluentCheckbox : public FluentWidget
{
public:
    bool* value = nullptr;
    FluentCheckbox(bool* v) : value(v) {}

protected:
    ImVec2 Measure(const char* label) override
    {
        float h = std::max(20.0f, label_size.y + 4.0f);
        float lw = label_size.x > 0 ? (12.0f + label_size.x) : 0.0f;
        return ImVec2(h + lw, h);
    }

    void Render(const char* label, const FluentTheme& t) override
    {
        float h = bb.GetHeight();
        ImRect box(bb.Min, ImVec2(bb.Min.x + h, bb.Max.y));
        float rd = 4.0f;
        float prog = Anim("t", *value ? 1.0f : 0.0f, t.anim_speed);

        ImU32 box_col = InterpCol(
            state.held ? t.surface_active : (state.hovered ? t.surface_hovered : t.surface),
            state.held ? t.accent_active  : (state.hovered ? t.accent_hovered  : t.accent),
            prog);
        FillRect(box, box_col, rd);
        StrokeRect(box, U32(t.border), rd, 1.0f);

        // 对勾：从中心向右下，随动画进度过渡到完全显示
        float cx = box.Min.x + h * 0.5f;
        float cy = box.Min.y + h * 0.5f;
        float arm = h * 0.22f * prog;
        if (arm > 0.01f)
        {
            draw->AddLine(ImVec2(cx - arm, cy), ImVec2(cx, cy + arm),
                          IM_COL32(255, 255, 255, 255), 2.0f);
            draw->AddLine(ImVec2(cx, cy + arm), ImVec2(cx + arm * 1.6f, cy - arm),
                          IM_COL32(255, 255, 255, 255), 2.0f);
        }

        if (label_size.x > 0.0f)
        {
            Text(ImVec2(box.Max.x + 8.0f, bb.Min.y + (h - label_size.y) * 0.5f),
                 U32(state.disabled ? t.text_disabled : t.text), label);
        }
    }

    bool  IsToggleable() const override { return true; }
    bool* GetToggleValue()     override { return value; }
};

// ---------------------------------------------------------------------------
// FluentSlider — Fluent 风格滑块（含数值标签）
// 与拖动式反射字段配合：提供 ValueChanged() 只在值跨帧变化时为 true，
// 避免每帧写入撤销命令栈。
// ---------------------------------------------------------------------------
class FluentSlider : public FluentWidget
{
public:
    float*      value  = nullptr;
    float       min_v  = 0.0f;
    float       max_v  = 1.0f;
    const char* format = "%.2f";
    float       width  = 160.0f;

    FluentSlider(float* v, float mn, float mx, const char* fmt = "%.2f", float w = 160.0f)
        : value(v), min_v(mn), max_v(mx), format(fmt), width(w) {}

    // 本帧值是否相对上一帧发生变化（用于触发一次性写入）。
    bool ValueChanged() const { return value_changed_; }

protected:
    bool holding_ = false;
    bool value_changed_ = false;

    ImVec2 Measure(const char* label) override
    {
        float h = 20.0f;
        float lw = label_size.x > 0 ? (label_size.x + 10.0f) : 0.0f;
        return ImVec2(lw + width, h);
    }

    void Render(const char* label, const FluentTheme& t) override
    {
        float pad_x = label_size.x > 0 ? (label_size.x + 10.0f) : 0.0f;
        ImRect track(ImVec2(bb.Min.x + pad_x, bb.Min.y), ImVec2(bb.Max.x, bb.Max.y));
        float cy = (track.Min.y + track.Max.y) * 0.5f;
        float th = 4.0f;

        // 交互：拖动
        if (!state.disabled)
        {
            bool hovered_track = ImGui::IsMouseHoveringRect(track.Min, track.Max);
            bool active = (state.held || ImGui::IsItemActive());
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                float val_t = (ImGui::GetIO().MousePos.x - track.Min.x) / track.GetWidth();
                val_t = ImClamp(val_t, 0.0f, 1.0f);
                *value = min_v + (max_v - min_v) * val_t;
                ImGui::MarkItemEdited(id);
                holding_ = true;
            }
            else
            {
                holding_ = false;
            }
            (void)hovered_track;
        }

        // 跨帧变化检测：用 ImGui 状态存储记忆上一帧值，
        // 对象即使每帧重建也能正确判定（首帧记录基线，无变化）。
        {
            ImGuiStorage* store = ImGui::GetStateStorage();
            ImGuiID k = ImHashStr("flslv", 0, id);
            float last = store->GetFloat(k, *value); // 首帧默认 = 当前值
            value_changed_ = (last != *value);
            store->SetFloat(k, *value);
        }

        // 进度
        float prog = ImClamp((*value - min_v) / (max_v - min_v), 0.0f, 1.0f);

        // 轨道
        ImU32 track_col = U32(state.disabled ? t.text_disabled : t.surface);
        FillRect(ImRect(ImVec2(track.Min.x, cy - th * 0.5f),
                        ImVec2(track.Max.x, cy + th * 0.5f)), track_col, th * 0.5f);

        float fill_w = track.GetWidth() * prog;
        if (fill_w > 1.0f)
            FillRect(ImRect(ImVec2(track.Min.x, cy - th * 0.5f),
                            ImVec2(track.Min.x + fill_w, cy + th * 0.5f)),
                     state.held ? U32(t.accent_active) : U32(t.accent), th * 0.5f);

        // 滑块圆点
        float cx = track.Min.x + track.GetWidth() * prog;
        float thumb_r = 7.0f;
        float hover = Anim("hover", (state.hovered || holding_) ? 1.0f : 0.0f, t.anim_speed);
        draw->AddCircleFilled(ImVec2(cx, cy + 1.0f), thumb_r + 12.0f * hover, IM_COL32(0, 0, 0, 30));
        draw->AddCircleFilled(ImVec2(cx, cy), thumb_r, U32(t.surface_active));
        StrokeRect(ImRect(ImVec2(cx - thumb_r, cy - thumb_r), ImVec2(cx + thumb_r, cy + thumb_r)),
                   state.held ? U32(t.accent) : U32(t.border), thumb_r, 1.5f);

        // 标签 + 数值
        if (label_size.x > 0.0f)
        {
            Text(bb.Min, U32(state.disabled ? t.text_disabled : t.text), label);
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), format, *value);
        ImVec2 vs = ImGui::CalcTextSize(buf);
        Text(ImVec2(track.Max.x - vs.x, cy - vs.y * 0.5f),
             U32(state.disabled ? t.text_disabled : t.text), buf);
    }
};

// ---------------------------------------------------------------------------
// FluentInputText — Fluent 风格单行输入框
// ---------------------------------------------------------------------------
class FluentInputText : public FluentWidget
{
public:
    char* buf       = nullptr;
    int   buf_size  = 256;
    float width     = 160.0f;
    bool  password  = false;

    FluentInputText(char* b, int sz, float w = 160.0f, bool pwd = false)
        : buf(b), buf_size(sz), width(w), password(pwd) {}

protected:
    ImVec2 Measure(const char* label) override
    {
        float h = std::max(28.0f, label_size.y + 10.0f);
        float lw = label_size.x > 0 ? (label_size.x + 10.0f) : 0.0f;
        return ImVec2(lw + width, h);
    }

    void Render(const char* label, const FluentTheme& t) override
    {
        float pad_x = label_size.x > 0 ? (label_size.x + 10.0f) : 0.0f;
        ImRect field(ImVec2(bb.Min.x + pad_x, bb.Min.y), bb.Max);

        bool focused = state.focused;
        float focus_anim = Anim("focus", focused ? 1.0f : 0.0f, t.anim_speed);

        // 背景
        ImU32 bg = U32(state.disabled ? t.text_disabled
                    : focused ? ImVec4(t.surface.x, t.surface.y, t.surface.z, 1.0f)
                    : t.surface);
        bg = InterpCol(t.surface,
                       ImVec4(t.surface.x + 0.05f, t.surface.y + 0.05f, t.surface.z + 0.05f, 1.0f),
                       focus_anim);
        FillRect(field, bg, t.rounding);

        // 文本
        const char* text = password ? "***" : buf;
        ImVec2 ts = ImGui::CalcTextSize(text);
        ImVec2 tp(field.Min.x + 8.0f, field.Min.y + (field.GetHeight() - ts.y) * 0.5f);
        Text(tp, U32(state.disabled ? t.text_disabled : t.text), text);

        // 底线 / 焦点指示
        StrokeRect(field, U32(ImVec4(t.border.x, t.border.y, t.border.z,
                                     state.focused ? 1.0f : 0.7f)),
                   t.rounding, 1.0f);
        if (focused)
        {
            float line_y = field.Max.y - 1.0f;
            draw->AddLine(ImVec2(field.Min.x, line_y), ImVec2(field.Max.x, line_y),
                          U32(t.accent), 2.0f);
        }

        // 交互：透传输入
        ImGui::SetCursorScreenPos(field.Min);
        ImGui::PushID(id);
        if (!state.disabled)
        {
            char edit_buf[512];
            std::strncpy(edit_buf, buf, sizeof(edit_buf) - 1);
            edit_buf[sizeof(edit_buf) - 1] = '\0';
            if (ImGui::InputText("##edit", edit_buf, sizeof(edit_buf),
                                 password ? ImGuiInputTextFlags_Password : 0,
                                 nullptr, nullptr))
            {
                std::strncpy(buf, edit_buf, buf_size - 1);
                buf[buf_size - 1] = '\0';
                ImGui::MarkItemEdited(id);
            }
        }
        ImGui::PopID();
    }
};

// ---------------------------------------------------------------------------
// FluentCombo — Fluent 风格下拉选择（透明原生命中列表项，自定义列表背景）
// ---------------------------------------------------------------------------
class FluentCombo : public FluentWidget
{
public:
    const char* const* items = nullptr;
    int                count = 0;
    int*               cur   = nullptr;

    FluentCombo(const char* const* it, int c, int* current)
        : items(it), count(c), cur(current) {}

protected:
    ImVec2 Measure(const char* label) override
    {
        float h = std::max(28.0f, label_size.y + 10.0f);
        float lw = label_size.x > 0 ? (label_size.x + 10.0f) : 0.0f;
        float w = 160.0f;
        return ImVec2(lw + w, h);
    }

    void Render(const char* label, const FluentTheme& t) override
    {
        float pad_x = label_size.x > 0 ? (label_size.x + 10.0f) : 0.0f;
        ImRect field(ImVec2(bb.Min.x + pad_x, bb.Min.y), bb.Max);
        float focus_anim = Anim("focus", state.focused ? 1.0f : 0.0f, t.anim_speed);

        FillRect(field,
                 InterpCol(t.surface,
                           ImVec4(t.surface.x + 0.05f, t.surface.y + 0.05f, t.surface.z + 0.05f, 1.0f),
                           focus_anim),
                 t.rounding);
        StrokeRect(field, U32(t.border), t.rounding, 1.0f);

        // 当前项文本
        const char* cur_text = (cur && *cur >= 0 && *cur < count) ? items[*cur] : "";
        ImVec2 ts = ImGui::CalcTextSize(cur_text);
        Text(ImVec2(field.Min.x + 8.0f, field.Min.y + (field.GetHeight() - ts.y) * 0.5f),
             U32(state.disabled ? t.text_disabled : t.text), cur_text);

        // 下拉箭头
        float cy = (field.Min.y + field.Max.y) * 0.5f;
        draw->AddTriangleFilled(ImVec2(field.Max.x - 18.0f, cy - 2.0f),
                                ImVec2(field.Max.x - 9.0f, cy - 2.0f),
                                ImVec2(field.Max.x - 13.5f, cy + 3.0f),
                                U32(state.disabled ? t.text_disabled : t.text));

        if (label_size.x > 0.0f)
            Text(bb.Min, U32(state.disabled ? t.text_disabled : t.text), label);

        // 透传原生命选区
        if (!state.disabled)
        {
            ImGui::SetCursorScreenPos(field.Min);
            ImGui::PushID(id);
            ImGui::SetNextItemWidth(field.GetWidth());
            if (ImGui::BeginCombo("##combo", "", ImGuiComboFlags_NoPreview | ImGuiComboFlags_HeightLargest))
            {
                // 配色：覆盖 ItemsBoxBg 以匹配主题
                ImGui::PushStyleColor(ImGuiCol_PopupBg, t.popup_bg);
                for (int i = 0; i < count; ++i)
                {
                    if (ImGui::Selectable(items[i], cur && *cur == i))
                    {
                        *cur = i;
                        ImGui::MarkItemEdited(id);
                    }
                }
                ImGui::PopStyleColor();
                ImGui::EndCombo();
            }
            ImGui::PopID();
        }
    }
};