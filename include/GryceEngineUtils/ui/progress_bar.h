#pragma once

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

class ProgressBar : public Widget {
public:
    explicit ProgressBar(const char* id);

    void set_progress(float p); // 0~1
    float progress() const { return progress_; }
    void set_color(const Color& fill, const Color& bg);
    void set_show_text(bool show);
    bool show_text_label() const { return show_text_; }
    void set_show_text_label(bool show) { show_text_ = show; }

    // 不确定模式（动画条纹）
    void set_indeterminate(bool ind) { indeterminate_ = ind; }
    bool indeterminate() const { return indeterminate_; }

    void draw(Renderer* renderer) override;
    void update(float dt) override;
    const char* type_name() const override { return "ProgressBar"; }
    float preferred_height(float width) const override;
    bool set_property(const char* key, const char* value) override;

private:
    float progress_ = 0.0f;
    Color fill_color_ = Color(0.25f, 0.6f, 1.0f, 1.0f);
    Color bg_color_ = Color(0.1f, 0.1f, 0.12f, 1.0f);
    bool show_text_ = true;
    bool indeterminate_ = false;
    float anim_offset_ = 0.0f;
};

} // namespace GryceEngineUtils::ui
