// engine/ui/ui_render_bridge.cpp
// 渲染桥实现：命令缓冲 → UiRenderer 调用。语义见 ui_render_bridge.h 头部。

#include "ui/ui_render_bridge.h"

namespace gryce_ui {

void UiRenderBridge::render(const UiDrawList& list, UiRenderer& renderer) {
    draws_    = 0;
    switches_ = 0;
    // 裁剪栈随命令流重建（不读 drawlist 的 clips_：那是构建期的活状态），
    // 见头文件「线程模型」。
    stack_depth_   = 0;
    applied_depth_ = kNoClipApplied;

    const int count = list.command_count();
    for (int i = 0; i < count; ++i) {
        const UiCommand& c = list.command(i);
        // 每条命令自带"执行后生效的裁剪深度"：深度变了才切 scissor。
        // PushClip/PopClip 与绘制命令走同一条逻辑（前者记的是压入后/弹出后
        // 的深度），不需要特判。
        if (c.clip != applied_depth_) {
            if (applied_depth_ == kNoClipApplied || c.clip > applied_depth_) {
                // 深度增加（首条命令也走这里）：触发命令必为 PushClip，生效
                // 矩形就在命令自身上（drawlist 下发的是相交后的矩形）。
                if (c.cmd == UiCmd::PushClip && stack_depth_ < kMaxClips) {
                    clip_stack_[stack_depth_].x = c.x;
                    clip_stack_[stack_depth_].y = c.y;
                    clip_stack_[stack_depth_].w = c.w;
                    clip_stack_[stack_depth_].h = c.h;
                    ++stack_depth_;
                    renderer.ui_set_scissor(c.x, c.y, c.w, c.h);
                } else {
                    // 不该发生（深度只可能由 push/pop 各改 1）。防御性复位，
                    // 并把桥的栈对齐到命令声称的深度，后续命令不错位。
                    stack_depth_ = (c.clip <= kMaxClips) ? static_cast<int>(c.clip) : kMaxClips;
                    renderer.ui_reset_scissor();
                }
            } else {
                // 深度减少：触发命令是 PopClip，父矩形在桥自己的栈里
                stack_depth_ = (c.clip >= 0 && c.clip <= kMaxClips)
                                   ? static_cast<int>(c.clip) : 0;
                if (stack_depth_ <= 0) {
                    renderer.ui_reset_scissor();
                } else {
                    const UiClipRect& r = clip_stack_[stack_depth_ - 1];
                    renderer.ui_set_scissor(r.x, r.y, r.w, r.h);
                }
            }
            ++switches_;
            applied_depth_ = c.clip;
        }
        switch (c.cmd) {
            case UiCmd::Rect:
                renderer.ui_draw_rect(c.x, c.y, c.w, c.h, c.color);
                ++draws_;
                break;
            case UiCmd::Text:
                renderer.ui_draw_text(c.x, c.y, list.text_at(c.text_off), c.font_size, c.color);
                ++draws_;
                break;
            case UiCmd::Sprite:
                renderer.ui_draw_sprite(c.x, c.y, c.w, c.h, c.texture,
                                        c.u0, c.v0, c.u1, c.v1, c.color);
                ++draws_;
                break;
            case UiCmd::PushClip:
            case UiCmd::PopClip:
                // 裁剪命令不产生绘制调用；它们的作用已经体现在上面的深度切换里
                break;
            default:
                break;
        }
    }
}

} // namespace gryce_ui
