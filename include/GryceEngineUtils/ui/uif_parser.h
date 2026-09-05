#pragma once

// GryceEngineUtils::ui::uif_parser.h — .uif 标记语言解析器
//
// .uif (UI Format) 是基于 XML 的 UI 标记语言，用于描述控件树结构。
// 支持控件类型：
//   Window, Panel, Text, Button, Image, List, Slider,
//   TextInput, CheckBox, ProgressBar, ScrollView, Divider,
//   ComboBox, RadioButton, SpinBox
//
// 支持的属性分类：
//   - 布局属性: layout, padding, spacing, align, width, height, margin
//   - 样式属性: style, color, bgcolor, font-size, border-radius, border-width, border-color
//   - 事件属性: onClick, onChange, onFocus, onBlur
//   - 数据绑定: text="{key}", src="{key}"
//   - 通用属性: id, title, text, src, min, max, value, checked, enabled, visible

#include <string>
#include <vector>

// pugixml 使用 header-only 模式以避免与 Assimp 内置的 pugixml 重复链接
#ifndef PUGIXML_HEADER_ONLY
#define PUGIXML_HEADER_ONLY
#endif
#include "pugixml/pugixml.hpp"

namespace GryceEngineUtils::ui {

// 解析结果结构
struct UIFParseResult {
    bool success = false;            // 解析是否成功
    pugi::xml_document doc;          // 解析后的 XML 文档
    std::string error_message;       // 错误信息（解析失败时）
    int error_line = 0;              // 错误行号
    std::string file_path;           // 源文件路径（用于调试）
};

// .uif 文件验证信息
struct UIFValidationInfo {
    bool valid = true;
    std::vector<std::string> warnings; // 非致命警告
    std::vector<std::string> errors;   // 致命错误
    std::vector<std::string> control_types_used; // 使用的控件类型列表
    int control_count = 0;             // 控件总数
};

// UIParser 类
// 职责：解析 .uif 文件或字符串，返回可用的 XML DOM 文档
class UIParser {
public:
    UIParser() = default;
    ~UIParser() = default;

    UIParser(const UIParser&) = delete;
    UIParser& operator=(const UIParser&) = delete;

    // 从文件解析 .uif
    // file_path: .uif 文件的路径
    // 返回 UIFParseResult，包含解析后的文档和状态信息
    UIFParseResult parse_file(const std::string& file_path);

    // 从字符串解析 .uif
    // content: 包含 .uif 标记的字符串
    // source_name: 源名称（用于错误报告，可选）
    // 返回 UIFParseResult，包含解析后的文档和状态信息
    UIFParseResult parse_string(const std::string& content, const std::string& source_name = "");

    // 验证文档结构
    // 检查控件类型是否合法、属性是否合规等
    // doc: 已解析的 XML 文档
    // 返回验证信息
    UIFValidationInfo validate(const pugi::xml_document& doc) const;

    // 获取已知控件类型列表
    static const std::vector<std::string>& known_control_types();

    // 获取控件支持的属性列表
    static const std::vector<std::string>& supported_attributes(const std::string& control_type);

    // 设置是否严格模式（严格模式下未知属性会报错）
    void set_strict_mode(bool strict) { strict_mode_ = strict; }
    bool strict_mode() const { return strict_mode_; }

private:
    bool strict_mode_ = false;

    // 递归验证节点
    void validate_node(const pugi::xml_node& node, UIFValidationInfo& info) const;

    // 检查控件类型是否合法
    bool is_known_control(const std::string& type) const;

    // 检查属性是否合法
    bool is_valid_attribute(const std::string& control_type, const std::string& attr) const;
};

} // namespace GryceEngineUtils::ui