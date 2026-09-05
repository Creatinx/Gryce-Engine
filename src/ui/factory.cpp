#include "GryceEngineUtils/ui/factory.h"

#include "GryceEngineUtils/ui/ui.h"

namespace GryceEngineUtils::ui {

WidgetFactory& WidgetFactory::instance() {
    static WidgetFactory factory;
    return factory;
}

WidgetFactory::WidgetFactory() {
    register_builtins();
}

void WidgetFactory::register_builtins() {
    register_type("Label", []() { return static_cast<Widget*>(new Label("label", "")); });
    register_type("Button", []() { return static_cast<Widget*>(new Button("button", "Button")); });
    register_type("Panel", []() { return static_cast<Widget*>(new Panel("panel")); });
    register_type("Image", []() { return static_cast<Widget*>(new Image("image")); });
    register_type("TextInput", []() { return static_cast<Widget*>(new TextInput("textinput")); });
    register_type("Slider", []() { return static_cast<Widget*>(new Slider("slider")); });
    register_type("CheckBox", []() { return static_cast<Widget*>(new CheckBox("checkbox", "Check")); });
    register_type("ProgressBar", []() { return static_cast<Widget*>(new ProgressBar("progress")); });
    register_type("ScrollView", []() { return static_cast<Widget*>(new ScrollView("scroll")); });
    register_type("ToolBar", []() { return static_cast<Widget*>(new ToolBar("toolbar")); });
    register_type("Divider", []() { return static_cast<Widget*>(new Divider("divider")); });
    register_type("MenuBar", []() { return static_cast<Widget*>(new MenuBar("menubar")); });
    register_type("MenuItem", []() { return static_cast<Widget*>(new MenuItem("menuitem", "Item")); });
    register_type("ComboBox", []() { return static_cast<Widget*>(new ComboBox("combobox")); });
    register_type("RadioButton", []() { return static_cast<Widget*>(new RadioButton("radio", "Option")); });
    register_type("SpinBox", []() { return static_cast<Widget*>(new SpinBox("spinbox")); });
}

void WidgetFactory::register_type(const char* type, Creator creator) {
    if (type && creator) {
        creators_[type] = std::move(creator);
    }
}

Widget* WidgetFactory::create(const char* type) const {
    auto it = creators_.find(type ? type : "");
    return it != creators_.end() ? it->second() : nullptr;
}

bool WidgetFactory::is_registered(const char* type) const {
    return creators_.find(type ? type : "") != creators_.end();
}

std::vector<std::string> WidgetFactory::registered_types() const {
    std::vector<std::string> types;
    types.reserve(creators_.size());
    for (const auto& kv : creators_) {
        types.push_back(kv.first);
    }
    return types;
}

} // namespace GryceEngineUtils::ui
