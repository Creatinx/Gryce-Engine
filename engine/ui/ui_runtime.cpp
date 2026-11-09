#include "ui/ui_runtime.h"

#include <algorithm>
#include <functional>
#include <vector>

#include "components/node_ui.h"
#include "ui/ui_extended_widgets.h"

namespace gryce_engine::ui {

namespace {

std::vector<components::NodeUI*> ui_children(const components::NodeUI* node) {
    std::vector<components::NodeUI*> children;
    if (!node) return children;
    for (components::Component* child : node->component_children()) {
        if (auto* ui = dynamic_cast<components::NodeUI*>(child)) {
            children.push_back(ui);
        }
    }
    std::stable_sort(children.begin(), children.end(),
                     [](const components::NodeUI* a, const components::NodeUI* b) {
                         return a->z_index < b->z_index;
                     });
    return children;
}

} // namespace

void UIRuntime::set_root(components::NodeUI* root) {
    if (root_ == root) return;
    root_ = root;
    hovered_ = nullptr;
    active_ = nullptr;
    focused_ = nullptr;
    drag_source_ = nullptr;
    drop_target_ = nullptr;
    drag_payload_.clear();
    rects_.clear();
    pointer_initialized_ = false;
    last_input_serial_ = 0;
    layout_scale_ = 1.0f;
    theme_revision_ = 0;
    draw_cache_valid_ = false;
}

void UIRuntime::clear() {
    root_ = nullptr;
    hovered_ = nullptr;
    active_ = nullptr;
    focused_ = nullptr;
    drag_source_ = nullptr;
    drop_target_ = nullptr;
    drag_payload_.clear();
    rects_.clear();
    pointer_initialized_ = false;
    last_input_serial_ = 0;
    layout_scale_ = 1.0f;
    theme_revision_ = 0;
    draw_cache_valid_ = false;
}

const UIRect* UIRuntime::rect_for(const components::NodeUI* node) const {
    const auto it = rects_.find(node);
    return it == rects_.end() ? nullptr : &it->second;
}

void UIRuntime::update(const UIInputFrame& input) {
    const uint32_t current_theme_revision = ui_theme_revision();
    const bool theme_changed = current_theme_revision != theme_revision_;
    theme_revision_ = current_theme_revision;
    const bool animated = advance_animations(root_, input.delta_time);
    if (animated || theme_changed) invalidate();
    layout_all();
    // 同一输入帧（frame_serial 相同）必须整体去重：动画/主题变化已经在上方面
    // 前跑过（advance_animations + layout_all），这里只跳过"输入事件重放"。
    // 此前用 !animated && !theme_changed 当闸门，会导致按下触发 press 补间后，
    // 重放同一 serial 帧时因 animated==true 而漏过去重、把 pointer_down 又派发
    // 一次（真实用法每帧 serial 都不同，所以这个闸门只在回放场景生效，但回放
    // 场景不该重复触发输入事件）。
    if (input.frame_serial != 0 && input.frame_serial == last_input_serial_) {
        return;
    }
    if (input.frame_serial != 0) last_input_serial_ = input.frame_serial;
    bool input_activity = false;
    if (!root_) {
        hovered_ = nullptr;
        active_ = nullptr;
        focused_ = nullptr;
        return;
    }

    if (focused_ && !focused_->is_visible_in_tree()) {
        UIEvent lost;
        lost.type = UIEventType::FocusLost;
        lost.target = focused_;
        dispatch(lost);
        focused_->set_focused_state(false);
        focused_ = nullptr;
    }
    if (active_ && !active_->is_visible_in_tree()) {
        active_->set_pressed_state(false);
        active_ = nullptr;
    }

    components::NodeUI* hit = hit_test(root_, input.pointer.x, input.pointer.y,
                                       viewport_);

    if (!pointer_initialized_ || input.pointer.x != previous_pointer_.x ||
        input.pointer.y != previous_pointer_.y || hit != hovered_) {
        input_activity = true;
        if (hovered_ != hit) {
            if (hovered_) {
                hovered_->set_hovered_state(false);
                UIEvent leave;
                leave.type = UIEventType::PointerLeave;
                leave.target = hovered_;
                leave.position = input.pointer;
                dispatch(leave);
            }
            if (hit) {
                hit->set_hovered_state(true);
                UIEvent enter;
                enter.type = UIEventType::PointerEnter;
                enter.target = hit;
                enter.position = input.pointer;
                dispatch(enter);
            }
        }

        components::NodeUI* move_target = active_ ? active_ : hit;
        if (move_target) {
            UIEvent move;
            move.type = UIEventType::PointerMove;
            move.target = move_target;
            move.position = input.pointer;
            dispatch(move);
        }

        if (active_ && !drag_source_) {
            const std::string payload = active_->drag_payload();
            const float dx = input.pointer.x - drag_start_pointer_.x;
            const float dy = input.pointer.y - drag_start_pointer_.y;
            if (!payload.empty() && dx * dx + dy * dy >= 16.0f) {
                drag_source_ = active_;
                drag_payload_ = payload;
                UIEvent drag_start;
                drag_start.type = UIEventType::DragStart;
                drag_start.target = drag_source_;
                drag_start.position = input.pointer;
                drag_start.payload = drag_payload_;
                dispatch(drag_start);
            }
        }
        if (drag_source_ && hit && hit != drop_target_) {
            UIEvent drag_over;
            drag_over.type = UIEventType::DragOver;
            drag_over.target = hit;
            drag_over.position = input.pointer;
            drag_over.payload = drag_payload_;
            dispatch(drag_over);
        }
        drop_target_ = drag_source_ ? hit : nullptr;
        hovered_ = hit;
        previous_pointer_ = input.pointer;
        pointer_initialized_ = true;
    }

    if ((input.wheel.x != 0.0f || input.wheel.y != 0.0f) && hit) {
        input_activity = true;
        UIEvent wheel;
        wheel.type = UIEventType::Wheel;
        wheel.target = hit;
        wheel.position = input.pointer;
        wheel.wheel = input.wheel;
        wheel.modifiers = input.modifiers;
        dispatch(wheel);
    }

    for (int button = 0; button < 3; ++button) {
        if (input.pointer_pressed[button]) {
            input_activity = true;
            active_ = hit;
            drag_source_ = nullptr;
            drop_target_ = nullptr;
            drag_payload_.clear();
            drag_start_pointer_ = input.pointer;
            components::NodeUI* next_focus = focus_target(hit);
            set_focused(next_focus, input.pointer, input.modifiers);
            if (hit) {
                hit->set_pressed_state(true);
                UIEvent down;
                down.type = UIEventType::PointerDown;
                down.target = hit;
                down.position = input.pointer;
                down.mouse_button = button;
                down.modifiers = input.modifiers;
                dispatch(down);
            }
        }

        if (input.pointer_released[button]) {
            input_activity = true;
            const bool was_dragging = drag_source_ != nullptr;
            components::NodeUI* release_target = active_ ? active_ : hit;
            components::NodeUI* pressed_target = active_;
            if (release_target) {
                UIEvent up;
                up.type = UIEventType::PointerUp;
                up.target = release_target;
                up.position = input.pointer;
                up.mouse_button = button;
                up.modifiers = input.modifiers;
                dispatch(up);
            }
            if (active_ && active_ == hit) {
                if (!was_dragging) {
                    UIEvent click;
                    click.type = UIEventType::Click;
                    click.target = active_;
                    click.position = input.pointer;
                    click.mouse_button = button;
                    click.modifiers = input.modifiers;
                    dispatch(click);
                }
            }
            if (was_dragging) {
                if (hit) {
                    UIEvent drop;
                    drop.type = UIEventType::Drop;
                    drop.target = hit;
                    drop.position = input.pointer;
                    drop.payload = drag_payload_;
                    dispatch(drop);
                }
                UIEvent drag_end;
                drag_end.type = UIEventType::DragEnd;
                drag_end.target = drag_source_;
                drag_end.position = input.pointer;
                drag_end.payload = drag_payload_;
                dispatch(drag_end);
            }
            if (pressed_target) pressed_target->set_pressed_state(false);
            active_ = nullptr;
            drag_source_ = nullptr;
            drop_target_ = nullptr;
            drag_payload_.clear();
        }
    }

    for (const UIKeyEvent& key : input.key_down) {
        input_activity = true;
        bool handled = false;
        if (focused_) {
            if (key.key == static_cast<int>(UIKey::Tab)) {
                std::vector<components::NodeUI*> order = focus_order();
                if (!order.empty()) {
                    std::size_t index = 0;
                    for (std::size_t i = 0; i < order.size(); ++i) {
                        if (order[i] == focused_) {
                            index = i;
                            break;
                        }
                    }
                    const bool reverse = (input.modifiers & (1 << 1)) != 0;
                    index = reverse
                                ? (index + order.size() - 1) % order.size()
                                : (index + 1) % order.size();
                    set_focused(order[index], input.pointer, input.modifiers);
                }
                handled = true;
            } else {
                UIEvent event;
                event.type = UIEventType::KeyDown;
                event.target = focused_;
                event.position = input.pointer;
                event.key = key.key;
                event.repeat = key.repeat;
                event.modifiers = input.modifiers;
                handled = dispatch(event);
            }
        }

        if (!handled) {
            for (const Shortcut& shortcut : shortcuts_) {
                if (shortcut.key == key.key && shortcut.modifiers == input.modifiers) {
                    shortcut_command_ = shortcut.command;
                    break;
                }
            }
        }
    }

    if (focused_) {
        for (int key : input.key_up) {
            input_activity = true;
            UIEvent event;
            event.type = UIEventType::KeyUp;
            event.target = focused_;
            event.position = input.pointer;
            event.key = key;
            event.modifiers = input.modifiers;
            dispatch(event);
        }
        for (uint32_t codepoint : input.characters) {
            input_activity = true;
            UIEvent event;
            event.type = UIEventType::Character;
            event.target = focused_;
            event.position = input.pointer;
            event.codepoint = codepoint;
            event.modifiers = input.modifiers;
            dispatch(event);
        }
        if (!input.preedit.empty()) {
            input_activity = true;
            UIEvent event;
            event.type = UIEventType::Preedit;
            event.target = focused_;
            event.position = input.pointer;
            event.preedit = input.preedit;
            dispatch(event);
        }
    }
    if (input_activity) invalidate();
}

std::vector<components::NodeUI*> UIRuntime::focus_order() const {
    std::vector<components::NodeUI*> order;
    std::function<void(components::NodeUI*)> visit = [&](components::NodeUI* node) {
        if (!node || !node->is_visible_in_tree()) return;
        if (node->focusable) order.push_back(node);
        for (components::NodeUI* child : ui_children(node)) visit(child);
    };
    visit(root_);
    return order;
}

void UIRuntime::set_focused(components::NodeUI* next, math::Vector2f position,
                            int modifiers) {
    if (focused_ == next) return;
    if (focused_) {
        // 焦点框的淡出由控件自己的 tween 完成，这里只翻转状态。
        focused_->set_focused_state(false);
        UIEvent lost;
        lost.type = UIEventType::FocusLost;
        lost.target = focused_;
        lost.position = position;
        lost.modifiers = modifiers;
        dispatch(lost);
    }
    focused_ = next;
    if (focused_) {
        focused_->set_focused_state(true);
        UIEvent gained;
        gained.type = UIEventType::FocusGained;
        gained.target = focused_;
        gained.position = position;
        gained.modifiers = modifiers;
        dispatch(gained);
    }
}

bool UIRuntime::advance_animations(components::NodeUI* node, float dt) {
    if (!node) return false;
    bool changed = node->advance_animations(dt);
    for (components::Component* child : node->component_children()) {
        if (auto* ui = dynamic_cast<components::NodeUI*>(child)) {
            changed = advance_animations(ui, dt) || changed;
        }
    }
    return changed;
}

void UIRuntime::layout_all() {
    rects_.clear();
    if (!root_ || viewport_.empty() || !root_->is_visible_in_tree()) return;

    const float width = root_->size.x > 0.0f
                            ? std::max(root_->size.x, root_->min_size.x) * layout_scale_
                            : viewport_.w;
    const float height = root_->size.y > 0.0f
                             ? std::max(root_->size.y, root_->min_size.y) * layout_scale_
                             : viewport_.h;
    const UIRect root_rect{
        viewport_.x + viewport_.w * root_->anchor_min.x +
            root_->local_position().x * layout_scale_,
        viewport_.y + viewport_.h * root_->anchor_min.y +
            root_->local_position().y * layout_scale_,
        width,
        height,
    };
    layout_node(root_, root_rect);
}

UIRect UIRuntime::layout_manual_child(const UIRect& parent_content,
                                      components::NodeUI* child) const {
    const float left = parent_content.x + parent_content.w * child->anchor_min.x;
    const float top = parent_content.y + parent_content.h * child->anchor_min.y;
    const float anchor_w =
        std::max(0.0f, (child->anchor_max.x - child->anchor_min.x) * parent_content.w);
    const float anchor_h =
        std::max(0.0f, (child->anchor_max.y - child->anchor_min.y) * parent_content.h);
    const float width = std::max(
        child->min_size.x * layout_scale_,
        child->size.x > 0.0f ? child->size.x * layout_scale_ : anchor_w);
    const float height = std::max(
        child->min_size.y * layout_scale_,
        child->size.y > 0.0f ? child->size.y * layout_scale_ : anchor_h);
    return {
        left + child->local_position().x * layout_scale_,
        top + child->local_position().y * layout_scale_,
        width,
        height,
    };
}

void UIRuntime::layout_node(components::NodeUI* node, const UIRect& rect) {
    if (!node || !node->is_visible_in_tree()) return;
    const math::Vector2f offset = node->visual_offset();
    const math::Vector2f scale = node->visual_scale();
    // 按下微缩不在这里做：rects_ 同时是命中测试的来源，布局期收缩会让指针在
    // 控件边缘附近反复进出，hover 抖动。按压反馈由控件在绘制期自行内缩。
    const UIRect animated_rect{
        rect.x + offset.x * layout_scale_,
        rect.y + offset.y * layout_scale_,
        std::max(0.0f, rect.w * scale.x),
        std::max(0.0f, rect.h * scale.y),
    };
    const UIRect clamped{
        animated_rect.x, animated_rect.y,
        std::max(node->min_size.x * layout_scale_, animated_rect.w),
        std::max(node->min_size.y * layout_scale_, animated_rect.h),
    };
    rects_[node] = clamped;

    const UIRect padded =
        clamped.inset(std::max(0.0f, node->padding) * layout_scale_);
    const math::Vector2f scroll = node->content_offset() * layout_scale_;
    const UIRect content{padded.x + scroll.x, padded.y + scroll.y,
                         padded.w, padded.h};
    if (content.empty()) return;

    const std::vector<components::NodeUI*> children = ui_children(node);
    if (auto* dock = dynamic_cast<components::UIDockHost*>(node)) {
        const float gap = std::max(0.0f, dock->gap) * layout_scale_;
        const float left = std::min(dock->left_width * layout_scale_, content.w * 0.45f);
        const float right = std::min(dock->right_width * layout_scale_, content.w * 0.45f);
        const float top = std::min(dock->top_height * layout_scale_, content.h * 0.45f);
        const float bottom = std::min(dock->bottom_height * layout_scale_, content.h * 0.45f);
        const float center_x = content.x + left + (left > 0.0f ? gap : 0.0f);
        const float center_y = content.y + top + (top > 0.0f ? gap : 0.0f);
        const float center_w = std::max(
            0.0f, content.w - left - right -
                      ((left > 0.0f ? gap : 0.0f) + (right > 0.0f ? gap : 0.0f)));
        const float center_h = std::max(
            0.0f, content.h - top - bottom -
                      ((top > 0.0f ? gap : 0.0f) + (bottom > 0.0f ? gap : 0.0f)));
        const float middle_y = content.y + top + (top > 0.0f ? gap : 0.0f);
        const float middle_h = std::max(
            0.0f, content.h - top - bottom -
                      ((top > 0.0f ? gap : 0.0f) + (bottom > 0.0f ? gap : 0.0f)));

        for (components::NodeUI* child : children) {
            auto* panel = dynamic_cast<components::UIDockPanel*>(child);
            if (!panel) continue;
            UIRect area;
            switch (panel->side) {
                case components::UIDockSide::Top:
                    area = {content.x, content.y, content.w, top};
                    break;
                case components::UIDockSide::Bottom:
                    area = {content.x, content.bottom() - bottom, content.w, bottom};
                    break;
                case components::UIDockSide::Left:
                    area = {content.x, middle_y, left, middle_h};
                    break;
                case components::UIDockSide::Right:
                    area = {content.right() - right, middle_y, right, middle_h};
                    break;
                case components::UIDockSide::Center:
                default:
                    area = {center_x, center_y, center_w, center_h};
                    break;
            }
            layout_node(child, area);
        }
        return;
    }
    if (node->layout_mode == components::UILayoutMode::Manual) {
        for (components::NodeUI* child : children) {
            if (!child->is_visible_in_tree()) continue;
            layout_node(child, layout_manual_child(content, child));
        }
        return;
    }

    float cursor = node->layout_mode == components::UILayoutMode::Column
                       ? content.y
                       : content.x;
    for (components::NodeUI* child : children) {
        if (!child->is_visible_in_tree()) continue;
        UIRect child_rect;
        if (node->layout_mode == components::UILayoutMode::Column) {
            child_rect.w = std::max(
                child->min_size.x * layout_scale_,
                child->size.x > 0.0f ? child->size.x * layout_scale_ : content.w);
            child_rect.h = std::max(
                child->min_size.y * layout_scale_,
                child->size.y > 0.0f ? child->size.y * layout_scale_ : 0.0f);
            child_rect.x = content.x + child->local_position().x * layout_scale_;
            child_rect.y = cursor + child->local_position().y * layout_scale_;
            cursor += child_rect.h + std::max(0.0f, node->gap) * layout_scale_;
        } else {
            child_rect.w = std::max(
                child->min_size.x * layout_scale_,
                child->size.x > 0.0f ? child->size.x * layout_scale_ : 0.0f);
            child_rect.h = std::max(
                child->min_size.y * layout_scale_,
                child->size.y > 0.0f ? child->size.y * layout_scale_ : content.h);
            child_rect.x = cursor + child->local_position().x * layout_scale_;
            child_rect.y = content.y + child->local_position().y * layout_scale_;
            cursor += child_rect.w + std::max(0.0f, node->gap) * layout_scale_;
        }
        layout_node(child, child_rect);
    }
}

components::NodeUI* UIRuntime::focus_target(components::NodeUI* node) const {
    for (components::NodeUI* cur = node; cur;) {
        if (cur->focusable) return cur;
        cur = dynamic_cast<components::NodeUI*>(cur->component_parent());
    }
    return nullptr;
}

components::NodeUI* UIRuntime::hit_test(components::NodeUI* node,
                                        float x, float y,
                                        const UIRect& parent_clip) const {
    if (!node || !node->is_visible_in_tree()) return nullptr;
    const auto it = rects_.find(node);
    if (it == rects_.end()) return nullptr;
    const UIRect& rect = it->second;

    UIRect clip = parent_clip;
    if (node->clip_contents) clip = clip.intersect(rect);
    if (clip.empty() || !clip.contains(x, y) || !rect.contains(x, y)) return nullptr;

    std::vector<components::NodeUI*> children = ui_children(node);
    for (auto child = children.rbegin(); child != children.rend(); ++child) {
        if (components::NodeUI* hit = hit_test(*child, x, y, clip)) {
            return hit;
        }
    }
    return node->receives_input ? node : nullptr;
}

bool UIRuntime::dispatch(UIEvent event) {
    for (components::NodeUI* current = event.target; current;) {
        event.current_target = current;
        const auto rect = rects_.find(current);
        if (rect != rects_.end()) event.layout_rect = rect->second;
        if (auto* parent = dynamic_cast<components::NodeUI*>(current->component_parent())) {
            const auto parent_rect = rects_.find(parent);
            if (parent_rect != rects_.end()) event.parent_rect = parent_rect->second;
        }
        if (current->on_ui_event(event)) return true;
        current = dynamic_cast<components::NodeUI*>(current->component_parent());
    }
    return false;
}

void UIRuntime::add_shortcut(int key, int modifiers, int command) {
    for (const Shortcut& shortcut : shortcuts_) {
        if (shortcut.key == key && shortcut.modifiers == modifiers) return;
    }
    shortcuts_.push_back({key, modifiers, command});
}

void UIRuntime::draw_node(components::NodeUI* node, const UIRect& clip,
                          UIDrawList& out, float opacity, bool top_layer) const {
    if (!node || !node->is_visible_in_tree()) return;
    const auto it = rects_.find(node);
    if (it == rects_.end()) return;
    const UIRect& rect = it->second;

    const float node_opacity = opacity * node->visual_opacity();
    if (node_opacity <= 0.001f) return;
    if (node->draws_on_top() == top_layer) {
        UIDrawList local;
        node->add_draw_commands(rect, local);
        out.append(local, node_opacity);
    }

    UIRect child_clip = clip;
    bool pushed = false;
    // 置顶层的浮层不受祖先裁剪：菜单/对话框/提示条本来就该压在窗口边缘上。
    if (!top_layer && node->clip_contents) {
        child_clip = clip.intersect(rect);
        if (child_clip.empty()) return;
        out.push_clip(child_clip);
        pushed = true;
    }

    for (components::NodeUI* child : ui_children(node)) {
        draw_node(child, child_clip, out, node_opacity, top_layer);
    }
    if (pushed) out.pop_clip();
}

void UIRuntime::build_draw_list(UIDrawList& out) const {
    if (draw_cache_valid_) {
        out = cached_draw_;
        return;
    }
    cached_draw_.reset();
    if (root_ && !viewport_.empty()) {
        // 两遍：普通节点按树序，浮层（draws_on_top）最后统一叠上去。第一遍结束
        // 的位置记成层起点，后端据此在浮层之前把已积累的批次提交掉。
        draw_node(root_, viewport_, cached_draw_, 1.0f, false);
        const std::size_t layer_start = cached_draw_.size();
        draw_node(root_, viewport_, cached_draw_, 1.0f, true);
        if (cached_draw_.size() > layer_start) {
            cached_draw_.set_top_layer_start(layer_start);
        }
    }
    draw_cache_valid_ = true;
    out = cached_draw_;
}

} // namespace gryce_engine::ui
