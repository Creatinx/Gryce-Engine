#include "GryceEngineUtils/ui/uif_parser.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <unordered_map>

namespace GryceEngineUtils::ui {

// ============================================================================
// 已知控件类型
// ============================================================================
static const std::vector<std::string> s_known_controls = {
    "Window",   "Panel",    "Text",      "Button",   "Image",
    "List",     "Slider",   "TextInput", "CheckBox", "ProgressBar",
    "ScrollView", "Divider", "ComboBox", "RadioButton", "SpinBox"
};

// 每种控件支持的属性
static const std::unordered_map<std::string, std::vector<std::string>> s_control_attributes = {
    {"Window",      {"id", "title", "layout", "padding", "spacing", "align", "width", "height", "margin", "style", "bgcolor", "visible", "enabled"}},
    {"Panel",       {"id", "layout", "padding", "spacing", "align", "width", "height", "margin", "style", "bgcolor", "visible", "enabled"}},
    {"Text",        {"id", "text", "color", "font-size", "bgcolor", "align", "width", "height", "margin", "style", "visible"}},
    {"Button",      {"id", "text", "style", "color", "font-size", "bgcolor", "width", "height", "margin", "padding", "onClick", "visible", "enabled"}},
    {"Image",       {"id", "src", "width", "height", "margin", "visible"}},
    {"List",        {"id", "width", "height", "margin", "padding", "spacing", "visible", "enabled"}},
    {"Slider",      {"id", "min", "max", "value", "width", "height", "margin", "onChange", "visible", "enabled"}},
    {"TextInput",   {"id", "text", "placeholder", "width", "height", "margin", "onChange", "visible", "enabled"}},
    {"CheckBox",    {"id", "text", "checked", "width", "margin", "onChange", "visible", "enabled"}},
    {"ProgressBar", {"id", "value", "min", "max", "width", "height", "margin", "visible"}},
    {"ScrollView",  {"id", "width", "height", "margin", "padding", "visible"}},
    {"Divider",     {"id", "width", "height", "margin", "color", "visible"}},
    {"ComboBox",    {"id", "width", "height", "margin", "onChange", "visible", "enabled"}},
    {"RadioButton", {"id", "text", "checked", "group", "width", "margin", "onChange", "visible", "enabled"}},
    {"SpinBox",     {"id", "value", "min", "max", "step", "width", "margin", "onChange", "visible", "enabled"}}
};

// ============================================================================
// 静态方法
// ============================================================================
const std::vector<std::string>& UIParser::known_control_types() {
    return s_known_controls;
}

const std::vector<std::string>& UIParser::supported_attributes(const std::string& control_type) {
    static const std::vector<std::string> empty;
    auto it = s_control_attributes.find(control_type);
    return it != s_control_attributes.end() ? it->second : empty;
}

// ============================================================================
// parse_file
// ============================================================================
UIFParseResult UIParser::parse_file(const std::string& file_path) {
    UIFParseResult result;
    result.file_path = file_path;

    // 检查文件是否存在
    if (!std::filesystem::exists(file_path)) {
        result.success = false;
        result.error_message = "File not found: " + file_path;
        return result;
    }

    // 检查文件扩展名
    std::string ext = std::filesystem::path(file_path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".uif") {
        result.success = false;
        result.error_message = "Expected .uif file extension, got: " + ext;
        return result;
    }

    // 使用 pugixml 加载文件
    pugi::xml_parse_result parse_result = result.doc.load_file(file_path.c_str(),
        pugi::parse_default | pugi::parse_trim_pcdata);

    if (!parse_result) {
        result.success = false;
        result.error_message = parse_result.description();
        result.error_line = parse_result.offset; // pugixml 用 offset 偏移量
        return result;
    }

    result.success = true;
    return result;
}

// ============================================================================
// parse_string
// ============================================================================
UIFParseResult UIParser::parse_string(const std::string& content, const std::string& source_name) {
    UIFParseResult result;
    result.file_path = source_name;

    // 使用 pugixml 解析内存字符串
    pugi::xml_parse_result parse_result = result.doc.load_string(content.c_str(),
        pugi::parse_default | pugi::parse_trim_pcdata);

    if (!parse_result) {
        result.success = false;
        result.error_message = parse_result.description();
        result.error_line = parse_result.offset;
        return result;
    }

    result.success = true;
    return result;
}

// ============================================================================
// validate
// ============================================================================
UIFValidationInfo UIParser::validate(const pugi::xml_document& doc) const {
    UIFValidationInfo info;

    // 获取根节点
    pugi::xml_node root = doc.document_element();
    if (!root) {
        info.valid = false;
        info.errors.push_back("Document has no root element");
        return info;
    }

    // 根节点必须是已知控件
    std::string root_name = root.name();
    if (!is_known_control(root_name)) {
        info.valid = false;
        info.errors.push_back("Unknown root control type: '" + root_name + "'");
    }

    // 递归验证
    validate_node(root, info);

    return info;
}

// ============================================================================
// validate_node（递归）
// ============================================================================
void UIParser::validate_node(const pugi::xml_node& node, UIFValidationInfo& info) const {
    std::string type = node.name();
    if (type.empty()) {
        info.errors.push_back("Empty node name (possibly a text node outside of valid structure)");
        info.valid = false;
        return;
    }

    info.control_count++;

    // 记录控件类型
    if (std::find(info.control_types_used.begin(), info.control_types_used.end(), type)
        == info.control_types_used.end()) {
        info.control_types_used.push_back(type);
    }

    // 检查控件类型是否合法
    if (!is_known_control(type)) {
        info.errors.push_back("Unknown control type: '" + type + "'");
        info.valid = false;
        return; // 未知类型，不继续检查属性
    }

    // 检查属性
    std::vector<std::string> valid_attrs = supported_attributes(type);
    for (const auto& attr : node.attributes()) {
        std::string attr_name = attr.name();

        // 数据绑定属性以 {} 结尾的值是合法的
        // 检查是否是已知属性
        bool known = std::find(valid_attrs.begin(), valid_attrs.end(), attr_name) != valid_attrs.end();
        if (!known && strict_mode_) {
            info.warnings.push_back("Unknown attribute '" + attr_name + "' on <" + type + ">");
        }

        // 检查 id 属性是否为空
        if (attr_name == "id" && std::strlen(attr.value()) == 0) {
            info.warnings.push_back("<" + type + "> has empty id attribute");
        }
    }

    // 递归验证子节点
    for (const auto& child : node.children()) {
        validate_node(child, info);
    }
}

// ============================================================================
// 辅助方法
// ============================================================================
bool UIParser::is_known_control(const std::string& type) const {
    return std::find(s_known_controls.begin(), s_known_controls.end(), type)
        != s_known_controls.end();
}

bool UIParser::is_valid_attribute(const std::string& control_type, const std::string& attr) const {
    auto it = s_control_attributes.find(control_type);
    if (it == s_control_attributes.end()) return false;
    return std::find(it->second.begin(), it->second.end(), attr) != it->second.end();
}

} // namespace GryceEngineUtils::ui