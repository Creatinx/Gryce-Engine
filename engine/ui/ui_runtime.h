#pragma once

#include <unordered_map>
#include <vector>

#include "export.h"
#include "ui/ui_drawlist.h"
#include "ui/ui_geometry.h"
#include "ui/ui_input.h"

namespace gryce_engine::ui {

// Phase 1 的 UI 运行时：不拥有 NodeUI，不持有 ECS 存储，不提交 GPU 命令。
// 每次 update 从 NodeUI 组件树重新计算布局、输入命中与交互状态，再生成
// 无后端依赖的 UIDrawList。
class GRYCE_API UIRuntime {
public:
    struct Shortcut {
        int key = 0;
        int modifiers = 0;
        int command = 0;
    };

    void set_root(components::NodeUI* root);
    components::NodeUI* root() const { return root_; }

    void set_viewport(const UIRect& viewport) {
        if (viewport.x != viewport_.x || viewport.y != viewport_.y ||
            viewport.w != viewport_.w || viewport.h != viewport_.h) {
            viewport_ = viewport;
            invalidate();
        }
    }
    const UIRect& viewport() const { return viewport_; }
    void set_layout_scale(float scale) {
        const float clamped = scale > 0.0f ? scale : 1.0f;
        if (clamped != layout_scale_) {
            layout_scale_ = clamped;
            invalidate();
        }
    }

    void update(const UIInputFrame& input);
    void build_draw_list(UIDrawList& out) const;
    void clear();
    void invalidate() const { draw_cache_valid_ = false; }

    const UIRect* rect_for(const components::NodeUI* node) const;
    components::NodeUI* hovered() const { return hovered_; }
    components::NodeUI* active() const { return active_; }
    components::NodeUI* focused() const { return focused_; }

    bool wants_mouse() const { return hovered_ != nullptr || active_ != nullptr; }
    bool wants_keyboard() const { return focused_ != nullptr; }
    void add_shortcut(int key, int modifiers, int command);
    int take_shortcut_command() {
        const int command = shortcut_command_;
        shortcut_command_ = 0;
        return command;
    }

private:
    void layout_all();
    void layout_node(components::NodeUI* node, const UIRect& rect);
    UIRect layout_manual_child(const UIRect& parent_content,
                               components::NodeUI* child) const;

    components::NodeUI* hit_test(components::NodeUI* node,
                                 float x, float y,
                                 const UIRect& parent_clip) const;
    components::NodeUI* focus_target(components::NodeUI* node) const;
    void set_focused(components::NodeUI* next, math::Vector2f position, int modifiers);
    std::vector<components::NodeUI*> focus_order() const;
    bool dispatch(UIEvent event);
    bool advance_animations(components::NodeUI* node, float dt);
    void draw_node(components::NodeUI* node, const UIRect& clip,
                   UIDrawList& out, float opacity = 1.0f,
                   bool top_layer = false) const;

    components::NodeUI* root_ = nullptr;
    UIRect viewport_;
    std::unordered_map<const components::NodeUI*, UIRect> rects_;

    components::NodeUI* hovered_ = nullptr;
    components::NodeUI* active_ = nullptr;
    components::NodeUI* focused_ = nullptr;
    math::Vector2f previous_pointer_;
    bool pointer_initialized_ = false;
    uint64_t last_input_serial_ = 0;
    mutable bool draw_cache_valid_ = false;
    mutable UIDrawList cached_draw_;
    float layout_scale_ = 1.0f;
    uint32_t theme_revision_ = 0;
    std::vector<Shortcut> shortcuts_;
    int shortcut_command_ = 0;
    components::NodeUI* drag_source_ = nullptr;
    components::NodeUI* drop_target_ = nullptr;
    math::Vector2f drag_start_pointer_;
    std::string drag_payload_;
};

} // namespace gryce_engine::ui
