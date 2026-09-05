#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

// RadioButton — 单选按钮（同组互斥）
class RadioButton : public Widget {
public:
    RadioButton(const char* id, const char* label);
    ~RadioButton() override;

    Signal<void(bool)> on_toggled;

    void set_group(const char* group);
    const std::string& group() const { return group_; }
    void set_checked(bool checked);
    void toggle();
    bool is_checked() const { return checked_; }
    bool checked() const { return checked_; } // 别名
    void set_label(const char* label);
    const std::string& label() const { return label_; }
    void set_text(const char* t) { set_label(t); }
    const std::string& text() const { return label_; }

    void draw(Renderer* renderer) override;
    const char* type_name() const override { return "RadioButton"; }
    float preferred_height(float width) const override;

private:
    void uncheck_others();

    bool checked_ = false;
    std::string label_;
    std::string group_;
    std::shared_ptr<bool> group_lifetime_ = std::make_shared<bool>(true);
};

} // namespace GryceEngineUtils::ui
