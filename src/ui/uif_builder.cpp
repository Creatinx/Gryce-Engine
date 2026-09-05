#include "GryceEngineUtils/ui/uif_builder.h"

#include <cstring>

#include "GryceEngineUtils/ui/factory.h"
#include "GryceEngineUtils/ui/panel.h"

namespace GryceEngineUtils::ui {

// ============================================================================
// .uif → Widget 类型名映射表
// ============================================================================
// .uif 使用符合直觉的控件名，与现有 Widget 类名可能不同。
static const std::unordered_map<std::string, std::string> s_default_type_aliases = {
    {"Window",      "Panel"},      // Window 映射为 Panel（根容器）
    {"Text",        "Label"},      // Text 映射为 Label
    {"List",        "Panel"},      // List 映射为 Panel（垂直布局）
    // 以下名称与现有 Widget 类型名一致，无需映射
    // {"Panel",     "Panel"},
    // {"Button",    "Button"},
    // {"Image",     "Image"},
    // {"Slider",    "Slider"},
    // {"TextInput", "TextInput"},
    // {"CheckBox",  "CheckBox"},
    // {"ProgressBar", "ProgressBar"},
    // {"ScrollView", "ScrollView"},
    // {"Divider",   "Divider"},
    // {"ComboBox",  "ComboBox"},
    // {"RadioButton", "RadioButton"},
    // {"SpinBox",   "SpinBox"},
};

// ============================================================================
// 静态方法
// ============================================================================
const std::unordered_map<std::string, std::string>& UIWidgetBuilder::type_aliases() {
    return s_default_type_aliases;
}

// ============================================================================
// 构造/析构
// ============================================================================
UIWidgetBuilder::~UIWidgetBuilder() {
    // 如果构建失败且启用了清理，释放所有已创建的控件
    // 注意：构建成功后，root 由调用方管理
}

// ============================================================================
// build — 从 XML DOM 构建控件树
// ============================================================================
UIBuildResult UIWidgetBuilder::build(const pugi::xml_document& doc) {
    UIBuildResult result;
    created_widgets_.clear();

    pugi::xml_node root_node = doc.document_element();
    if (!root_node) {
        result.success = false;
        result.error_message = "Document has no root element";
        return result;
    }

    // 递归构建
    int count = 0;
    Widget* root = build_node(root_node, count);
    if (!root) {
        result.success = false;
        result.error_message = "Failed to build root widget from <" + std::string(root_node.name()) + ">";
        if (cleanup_on_failure_) {
            for (auto* w : created_widgets_) delete w;
        }
        created_widgets_.clear();
        return result;
    }

    result.success = true;
    result.root = root;
    result.widget_count = count;
    return result;
}

// ============================================================================
// build_node — 递归构建单个控件节点
// ============================================================================
Widget* UIWidgetBuilder::build_node(const pugi::xml_node& xml_node, int& count) {
    const char* uif_name = xml_node.name();
    if (!uif_name || !*uif_name) return nullptr;

    // 解析类型名（.uif 名 → Widget 类型名）
    std::string widget_type = resolve_type_name(uif_name);
    if (widget_type.empty()) {
        return nullptr;
    }

    // 通过工厂创建控件
    Widget* widget = WidgetFactory::instance().create(widget_type.c_str());
    if (!widget) {
        return nullptr;
    }

    count++;
    created_widgets_.push_back(widget);

    // 属性注入循环：遍历所有属性，调用 set_property
    for (const auto& attr : xml_node.attributes()) {
        const char* key = attr.name();
        const char* value = attr.value();
        widget->set_property(key, value);
    }

    // 特殊处理：List 类型（由 Panel 实现）默认垂直布局
    if (std::strcmp(uif_name, "List") == 0) {
        if (auto* panel = dynamic_cast<Panel*>(widget)) {
            panel->set_layout(LayoutType::Vertical);
            panel->set_spacing(2.0f);
        }
    }

    // 递归构建子节点
    for (const auto& child : xml_node.children()) {
        // 跳过纯文本节点（XML 中的空白符）
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            continue;
        }

        Widget* child_widget = build_node(child, count);
        if (child_widget) {
            widget->add_child(child_widget);
        }
    }

    return widget;
}

// ============================================================================
// resolve_type_name — 解析 .uif 类型名到 Widget 类型名
// ============================================================================
std::string UIWidgetBuilder::resolve_type_name(const char* uif_name) const {
    if (!uif_name || !*uif_name) return "";

    // 1. 检查用户自定义映射
    auto custom_it = custom_aliases_.find(uif_name);
    if (custom_it != custom_aliases_.end()) {
        return custom_it->second;
    }

    // 2. 检查默认映射
    auto default_it = s_default_type_aliases.find(uif_name);
    if (default_it != s_default_type_aliases.end()) {
        // 检查映射后的类型是否已注册
        if (WidgetFactory::instance().is_registered(default_it->second.c_str())) {
            return default_it->second;
        }
        return "";
    }

    // 3. 直接使用 .uif 名作为 Widget 类型名
    if (WidgetFactory::instance().is_registered(uif_name)) {
        return uif_name;
    }

    return "";
}

// ============================================================================
// register_type_alias
// ============================================================================
void UIWidgetBuilder::register_type_alias(const char* uif_name, const char* widget_type) {
    if (uif_name && widget_type) {
        custom_aliases_[uif_name] = widget_type;
    }
}

} // namespace GryceEngineUtils::ui