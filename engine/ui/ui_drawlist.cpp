// engine/ui/ui_drawlist.cpp
// UIDrawList 实现：命令缓冲 + 裁剪栈 + 字符串池。语义见 ui_drawlist.h 头部。

#include "ui/ui_drawlist.h"

#include <cstring>

namespace gryce_ui {

void UiDrawList::reset() {
    count_      = 0;
    clip_depth_ = 0;
    text_used_  = 0;
    dropped_    = 0;
    culled_     = 0;
}

bool UiDrawList::cull_active() const {
    if (clip_depth_ == 0) return false;
    const UiClipRect& c = clips_[clip_depth_];
    return c.w <= 0.0f || c.h <= 0.0f;
}

void UiDrawList::intersect(float x, float y, float w, float h,
                           float* ox, float* oy, float* ow, float* oh) const {
    if (clip_depth_ == 0) {
        *ox = x; *oy = y; *ow = w; *oh = h;
        return;
    }
    const UiClipRect& p = clips_[clip_depth_];
    const float x1 = (x > p.x) ? x : p.x;
    const float y1 = (y > p.y) ? y : p.y;
    const float x2 = ((x + w) < (p.x + p.w)) ? (x + w) : (p.x + p.w);
    const float y2 = ((y + h) < (p.y + p.h)) ? (y + h) : (p.y + p.h);
    *ox = x1;
    *oy = y1;
    *ow = (x2 > x1) ? (x2 - x1) : 0.0f;
    *oh = (y2 > y1) ? (y2 - y1) : 0.0f;
}

bool UiDrawList::emit(const UiCommand& c) {
    if (count_ >= kMaxCommands) {
        ++dropped_;
        return false;
    }
    commands_[count_] = c;
    commands_[count_].clip = static_cast<uint32_t>(clip_depth_);
    ++count_;
    return true;
}

void UiDrawList::push_clip(float x, float y, float w, float h) {
    if (clip_depth_ >= kMaxClips) {
        // 栈满拒绝：pop 只按 depth 弹，未成功的 push 不会让后续 pop 失衡
        ++dropped_;
        return;
    }
    float ox, oy, ow, oh;
    intersect(x, y, w, h, &ox, &oy, &ow, &oh);
    ++clip_depth_;
    clips_[clip_depth_].x = ox;
    clips_[clip_depth_].y = oy;
    clips_[clip_depth_].w = ow;
    clips_[clip_depth_].h = oh;

    UiCommand c{};
    c.cmd = UiCmd::PushClip;
    emit(c);
}

void UiDrawList::pop_clip() {
    if (clip_depth_ <= 0) return;  // 空栈无操作
    --clip_depth_;

    UiCommand c{};
    c.cmd = UiCmd::PopClip;
    emit(c);
}

void UiDrawList::add_rect(float x, float y, float w, float h, uint32_t rgba) {
    if (cull_active()) {
        ++culled_;
        return;
    }
    UiCommand c{};
    c.cmd   = UiCmd::Rect;
    c.x     = x;
    c.y     = y;
    c.w     = w;
    c.h     = h;
    c.color = rgba;
    emit(c);
}

void UiDrawList::add_text(float x, float y, const char* text, float font_size, uint32_t rgba) {
    if (!text || text[0] == '\0') return;  // 空文本不产生命令
    const size_t len = std::strlen(text);
    if (cull_active()) {
        ++culled_;
        return;
    }
    if (text_used_ + len + 1 > static_cast<size_t>(kTextPoolSize)) {
        // 池放不下整条：丢弃并计数，不截断
        ++dropped_;
        return;
    }
    UiCommand c{};
    c.cmd       = UiCmd::Text;
    c.x         = x;
    c.y         = y;
    c.color     = rgba;
    c.font_size = font_size;
    c.text_off  = static_cast<uint32_t>(text_used_);
    c.text_len  = static_cast<uint32_t>(len);
    std::memcpy(text_pool_ + text_used_, text, len + 1);
    text_used_ += static_cast<int>(len) + 1;
    emit(c);
}

void UiDrawList::add_sprite(float x, float y, float w, float h, uint64_t texture,
                            float u0, float v0, float u1, float v1, uint32_t rgba) {
    if (cull_active()) {
        ++culled_;
        return;
    }
    UiCommand c{};
    c.cmd      = UiCmd::Sprite;
    c.x        = x;
    c.y        = y;
    c.w        = w;
    c.h        = h;
    c.texture  = texture;
    c.u0       = u0;
    c.v0       = v0;
    c.u1       = u1;
    c.v1       = v1;
    c.color    = rgba;
    emit(c);
}

const char* UiDrawList::text_at(uint32_t off) const {
    return text_pool_ + off;
}

const UiClipRect& UiDrawList::clip_at(int index) const {
    if (index < 1 || index > clip_depth_) return clips_[0];
    return clips_[index];
}

} // namespace gryce_ui
