#pragma once

#include <string>
#include <vector>

#include "GryceEngineUtils/ui/label.h"
#include "GryceEngineUtils/ui/panel.h"
#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

// ComboBox — 下拉选择框
class ComboBox : public Widget {
public:
    explicit ComboBox(const char* id = "combobox");

    Signal<void(int)> on_selected;        // 选中索引
    Signal<void(const char*)> on_text_selected; // 选中文本

    void add_item(const char* text);
    void clear_items();
    void set_selected_index(int index);
    void toggle_popup();
    bool is_open() const { return open_; }
    bool hit_test(float x, float y) const override;
    int selected_index() const { return selected_; }
    const std::string& selected_text() const;

    void draw(Renderer* renderer) override;
    void on_global_click(Widget* clicked) override;
    const char* type_name() const override { return "ComboBox"; }
    float preferred_height(float width) const override;
    bool set_property(const char* key, const char* value) override;

private:
    void relayout_popup();

    std::vector<std::string> items_;
    int selected_ = -1;
    bool open_ = false;
    Panel* popup_ = nullptr;
    int hovered_item_ = -1;
};

} // namespace GryceEngineUtils::ui
