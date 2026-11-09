#pragma once

#include <algorithm>

#include "components/node2d.h"
#include "math/math.h"
#include "ui/ui_animation.h"
#include "ui/ui_theme.h"

namespace gryce_engine::ui {
struct UIRect;
struct UIEvent;
class UIDrawList;
} // namespace gryce_engine::ui

namespace gryce_engine::components {

using ui::UIThemeColor;

enum class UILayoutMode : uint8_t {
    Manual = 0,
    Column = 1,
    Row = 2,
};

// ---------------------------------------------------------------------------
// NodeUI — 所有 UI 节点的公共基类
//
// 继承 Node2D 以复用 2D 变换、层级和绘制顺序，但 UI 的尺寸、输入门控和裁剪
// 规则独立于游戏 2D 组件。控件树、布局和输入由 ui::UIRuntime 驱动。
// ---------------------------------------------------------------------------
class NodeUI : public Node2D {
public:
    math::Vector2f size = math::Vector2f::zero();
    math::Vector2f min_size = math::Vector2f::zero();

    // 归一化锚点相对父内容区。Manifest 示例：
    //   anchor_min=(1,0), anchor_max=(1,0) 把节点锚到右上角。
    math::Vector2f anchor_min = math::Vector2f::zero();
    math::Vector2f anchor_max = math::Vector2f::zero();

    UILayoutMode layout_mode = UILayoutMode::Manual;
    float padding = 0.0f;
    float gap = 0.0f;

    // 不接收输入不会影响渲染；父节点禁用时子节点由组件树继承该状态。
    bool receives_input = true;
    bool focusable = false;
    bool clip_contents = false;

    UIThemeColor background_role = UIThemeColor::Custom;
    UIThemeColor background_hover_role = UIThemeColor::Custom;
    UIThemeColor background_pressed_role = UIThemeColor::Custom;
    UIThemeColor foreground_role = UIThemeColor::Custom;
    UIThemeColor accent_role = UIThemeColor::Custom;
    UIThemeColor border_role = UIThemeColor::Custom;
    float hover_duration = 0.12f;
    float press_duration = 0.08f;

    // ---- Fluent 的圆角与层次 -------------------------------------------
    // 控件属于哪一档圆角。具体像素由主题给，控件不写裸数值。
    ui::UIRadiusTier radius_tier = ui::UIRadiusTier::Medium;
    // corner_radius < 0 表示"按 radius_tier 取"，>= 0 时用绝对值覆盖。
    float corner_radius = -1.0f;
    // 静止态投影扩散半径（像素）。0 = 不投影，此时不产生任何额外绘制命令。
    float rest_elevation = 0.0f;
    // hover 时额外叠加的扩散半径，用来做"浮起"动效。
    float hover_elevation = 0.0f;
    // 按下时的缩小量（0.02 = 缩到 98%）。0 = 不做缩放。
    float press_scale = 0.0f;

    NodeUI() {
        opacity_tween.reset(1.0f);
        offset_tween.reset(math::Vector2f::zero());
        scale_tween.reset(math::Vector2f::one());
        hover_tween.reset(0.0f);
        press_tween.reset(0.0f);
        focus_tween.reset(0.0f);
    }

    NodeDomain node_domain() const noexcept override { return NodeDomain::UI; }

    const char* type() const override { return "NodeUI"; }

    void collect_type_views(std::vector<ComponentTypeView>& out) const override {
        Node2D::collect_type_views(out);
        out.push_back({std::type_index(typeid(NodeUI)),
                       const_cast<void*>(static_cast<const void*>(this))});
    }

    // 默认不处理事件。控件按需覆写并返回 true 以阻止事件继续冒泡。
    virtual bool on_ui_event(const ui::UIEvent& event) {
        (void)event;
        return false;
    }

    // 是否在**所有普通节点之后**再绘制一遍（浮层/菜单/对话框/提示条）。
    // 绘制顺序默认是树的深度优先序，浮层的矩形往往伸到父容器之外，先画就会
    // 被后面的兄弟节点盖住——把这类节点收集到第二遍画，才和真正的窗口层次
    // 一致。裁剪仍按各自祖先链计算，不受影响。
    virtual bool draws_on_top() const { return false; }

    // 模型层绘制。GPU 提交由后续 UIRenderPass 负责。
    virtual void add_draw_commands(const ui::UIRect& rect,
                                   ui::UIDrawList& draw_list) const {
        (void)rect;
        (void)draw_list;
    }

    virtual math::Vector2f content_offset() const {
        return math::Vector2f::zero();
    }

    virtual std::string drag_payload() const { return {}; }

    uint32_t themed_color(UIThemeColor role, uint32_t fallback) const {
        return role == UIThemeColor::Custom ? fallback : ui::ui_theme_color(role);
    }

    void set_hovered_state(bool hovered) {
        if (hovered_ == hovered) return;
        hovered_ = hovered;
        hover_tween.start(hover_tween.value, hovered ? 1.0f : 0.0f,
                          hovered ? hover_duration : hover_duration * 0.75f,
                          ui::UIEasing::OutCubic);
    }

    void set_pressed_state(bool pressed) {
        if (pressed_ == pressed) return;
        pressed_ = pressed;
        press_tween.start(press_tween.value, pressed ? 1.0f : 0.0f,
                          pressed ? press_duration : press_duration * 0.7f,
                          ui::UIEasing::OutCubic);
    }

    // 键盘/焦点态。焦点框的淡入淡出走 focus_tween，时长取主题的 fast 档。
    void set_focused_state(bool focused) {
        if (focused_ == focused) return;
        focused_ = focused;
        const float seconds = std::max(0.001f, ui::ui_theme_metric(
            ui::UIThemeMetric::MotionFast));
        focus_tween.start(focus_tween.value, focused ? 1.0f : 0.0f, seconds,
                          ui::UIEasing::OutCubic);
    }

    bool is_focused() const { return focused_; }
    float focus_amount() const { return focus_tween.value; }

    void animate_opacity(float target, float seconds,
                         ui::UIEasing easing = ui::UIEasing::OutCubic,
                         float delay = 0.0f) {
        opacity_tween.start(opacity_tween.value, std::clamp(target, 0.0f, 1.0f),
                            seconds, easing, delay);
    }

    void animate_offset(const math::Vector2f& target, float seconds,
                        ui::UIEasing easing = ui::UIEasing::OutCubic,
                        float delay = 0.0f) {
        offset_tween.start(offset_tween.value, target, seconds, easing, delay);
    }

    void animate_scale(const math::Vector2f& target, float seconds,
                       ui::UIEasing easing = ui::UIEasing::OutCubic,
                       float delay = 0.0f) {
        scale_tween.start(scale_tween.value, target, seconds, easing, delay);
    }

    virtual bool advance_animations(float dt) {
        bool changed = false;
        changed = opacity_tween.advance(dt) || changed;
        changed = offset_tween.advance(dt) || changed;
        changed = scale_tween.advance(dt) || changed;
        changed = hover_tween.advance(dt) || changed;
        changed = press_tween.advance(dt) || changed;
        changed = focus_tween.advance(dt) || changed;
        return changed;
    }

    bool has_active_animation() const {
        return opacity_tween.active || offset_tween.active ||
               scale_tween.active || hover_tween.active || press_tween.active ||
               focus_tween.active;
    }

    float visual_opacity() const { return opacity_tween.value; }
    const math::Vector2f& visual_offset() const { return offset_tween.value; }
    const math::Vector2f& visual_scale() const { return scale_tween.value; }
    float hover_amount() const { return hover_tween.value; }
    float press_amount() const { return press_tween.value; }

    void serialize(nlohmann::json& out) const override {
        serialize_node_ui_base(out);
    }

    void deserialize(const nlohmann::json& in) override {
        deserialize_node_ui_base(in);
    }

protected:
    void serialize_node_ui_base(nlohmann::json& out) const {
        Node2D::serialize_node2d_base(out);
        out["size"] = {size.x, size.y};
        out["min_size"] = {min_size.x, min_size.y};
        out["anchor_min"] = {anchor_min.x, anchor_min.y};
        out["anchor_max"] = {anchor_max.x, anchor_max.y};
        out["layout_mode"] = static_cast<int>(layout_mode);
        out["padding"] = padding;
        out["gap"] = gap;
        out["receives_input"] = receives_input;
        out["focusable"] = focusable;
        out["clip_contents"] = clip_contents;
        out["background_role"] = static_cast<int>(background_role);
        out["background_hover_role"] = static_cast<int>(background_hover_role);
        out["background_pressed_role"] = static_cast<int>(background_pressed_role);
        out["foreground_role"] = static_cast<int>(foreground_role);
        out["accent_role"] = static_cast<int>(accent_role);
        out["border_role"] = static_cast<int>(border_role);
        out["hover_duration"] = hover_duration;
        out["press_duration"] = press_duration;
        out["corner_radius"] = corner_radius;
        out["radius_tier"] = static_cast<int>(radius_tier);
        out["rest_elevation"] = rest_elevation;
        out["hover_elevation"] = hover_elevation;
        out["press_scale"] = press_scale;
    }

    void deserialize_node_ui_base(const nlohmann::json& in) {
        Node2D::deserialize_node2d_base(in);
        const auto s = in.value("size", std::vector<float>{0.0f, 0.0f});
        if (s.size() >= 2) size = math::Vector2f(s[0], s[1]);
        const auto minimum = in.value("min_size", std::vector<float>{0.0f, 0.0f});
        if (minimum.size() >= 2) min_size = math::Vector2f(minimum[0], minimum[1]);
        const auto amin = in.value("anchor_min", std::vector<float>{0.0f, 0.0f});
        if (amin.size() >= 2) anchor_min = math::Vector2f(amin[0], amin[1]);
        const auto amax = in.value("anchor_max", std::vector<float>{0.0f, 0.0f});
        if (amax.size() >= 2) anchor_max = math::Vector2f(amax[0], amax[1]);
        layout_mode = static_cast<UILayoutMode>(
            in.value("layout_mode", static_cast<int>(UILayoutMode::Manual)));
        padding = in.value("padding", 0.0f);
        gap = in.value("gap", 0.0f);
        receives_input = in.value("receives_input", true);
        focusable = in.value("focusable", false);
        clip_contents = in.value("clip_contents", false);
        background_role = static_cast<UIThemeColor>(
            in.value("background_role", static_cast<int>(UIThemeColor::Custom)));
        background_hover_role = static_cast<UIThemeColor>(
            in.value("background_hover_role", static_cast<int>(UIThemeColor::Custom)));
        background_pressed_role = static_cast<UIThemeColor>(
            in.value("background_pressed_role", static_cast<int>(UIThemeColor::Custom)));
        foreground_role = static_cast<UIThemeColor>(
            in.value("foreground_role", static_cast<int>(UIThemeColor::Custom)));
        accent_role = static_cast<UIThemeColor>(
            in.value("accent_role", static_cast<int>(UIThemeColor::Custom)));
        border_role = static_cast<UIThemeColor>(
            in.value("border_role", static_cast<int>(UIThemeColor::Custom)));
        hover_duration = std::max(0.0f, in.value("hover_duration", 0.12f));
        press_duration = std::max(0.0f, in.value("press_duration", 0.08f));
        corner_radius = in.value("corner_radius", -1.0f);
        radius_tier = static_cast<ui::UIRadiusTier>(
            in.value("radius_tier", static_cast<int>(ui::UIRadiusTier::Medium)));
        rest_elevation = std::max(0.0f, in.value("rest_elevation", 0.0f));
        hover_elevation = std::max(0.0f, in.value("hover_elevation", 0.0f));
        press_scale = std::max(0.0f, in.value("press_scale", 0.0f));
    }

private:
    bool hovered_ = false;
    bool pressed_ = false;
    bool focused_ = false;
    ui::UIFloatTween opacity_tween;
    ui::UIVector2Tween offset_tween;
    ui::UIVector2Tween scale_tween;
    ui::UIFloatTween hover_tween;
    ui::UIFloatTween press_tween;
    ui::UIFloatTween focus_tween;
};

} // namespace gryce_engine::components
