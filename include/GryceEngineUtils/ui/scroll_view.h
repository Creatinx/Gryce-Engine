#pragma once

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

class ScrollView : public Widget {
public:
    explicit ScrollView(const char* id);

    void set_content(Widget* content);
    Widget* content() const { return content_; }
    void scroll_to(float x, float y);
    void set_scroll(float x, float y);
    float scroll_x() const { return scroll_x_; }
    float scroll_y() const { return scroll_y_; }

    // 供 UIManager 拖拽滚动
    void start_drag(float screen_x, float screen_y);
    void drag_to(float screen_x, float screen_y);
    void end_drag() { dragging_ = false; }

    // 滚动条自动隐藏
    void set_auto_hide_scrollbar(bool auto_hide) { auto_hide_scrollbar_ = auto_hide; }
    bool auto_hide_scrollbar() const { return auto_hide_scrollbar_; }

    void draw(Renderer* renderer) override;
    const char* type_name() const override { return "ScrollView"; }

    // 内容被裁剪区域内的命中测试（命中内容才滚动）
    bool hit_test(float x, float y) const override;

private:
    Widget* content_ = nullptr;
    float scroll_x_ = 0.0f;
    float scroll_y_ = 0.0f;
    bool dragging_ = false;
    float drag_last_y_ = 0.0f;
    bool auto_hide_scrollbar_ = true;
    bool mouse_over_ = false;
};

} // namespace GryceEngineUtils::ui
