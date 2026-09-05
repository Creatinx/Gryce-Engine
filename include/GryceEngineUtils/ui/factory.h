#pragma once

// GryceEngineUtils::ui::factory.h — 快速创建组件
//
// WidgetFactory：按类型名注册/创建控件的工厂。
// 内置控件自动注册，用户可用 register_type 注册自定义控件：
//
//   WidgetFactory::instance().register_type("MyWidget", []() { return new MyWidget(); });
//   auto* w = WidgetFactory::instance().create("MyWidget");

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

class WidgetFactory {
public:
    using Creator = std::function<Widget*()>;

    static WidgetFactory& instance();

    // 注册类型（覆盖同名）
    void register_type(const char* type, Creator creator);
    // 按类型创建控件；未注册返回 nullptr
    Widget* create(const char* type) const;
    bool is_registered(const char* type) const;
    std::vector<std::string> registered_types() const;

private:
    WidgetFactory();
    void register_builtins();

    std::unordered_map<std::string, Creator> creators_;
};

} // namespace GryceEngineUtils::ui
