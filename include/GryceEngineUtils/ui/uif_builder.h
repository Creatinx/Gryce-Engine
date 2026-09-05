#pragma once

// GryceEngineUtils::ui::uif_builder.h — .uif → 控件树构建器
//
// UIWidgetBuilder 将 UIParser 解析出的 XML DOM 转换为现有的控件树。
// 负责：
//   1. 将 .uif 控件类型名映射到现有 Widget 类型名（如 Text → Label）
//   2. 递归遍历 DOM 树，通过 WidgetFactory 创建控件实例
//   3. 属性注入循环：遍历 DOM 节点的所有属性，调用 set_property
//   4. 返回构建完成的控件树根节点

#include <string>
#include <unordered_map>
#include <vector>

// pugixml 使用 header-only 模式以避免与 Assimp 内置的 pugixml 重复链接
#ifndef PUGIXML_HEADER_ONLY
#define PUGIXML_HEADER_ONLY
#endif
#include "pugixml/pugixml.hpp"

#include "GryceEngineUtils/ui/dsl/ast.h"
#include "GryceEngineUtils/ui/dsl/semantic_analyzer.h"
#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

// 构建结果
struct UIBuildResult {
    bool success = false;
    Widget* root = nullptr;        // 构建完成的控件树根节点
    std::string error_message;
    int widget_count = 0;          // 创建的控件总数
};

// UIWidgetBuilder 类
class UIWidgetBuilder {
public:
    UIWidgetBuilder() = default;
    ~UIWidgetBuilder();

    UIWidgetBuilder(const UIWidgetBuilder&) = delete;
    UIWidgetBuilder& operator=(const UIWidgetBuilder&) = delete;

    // 从解析后的 XML DOM 构建控件树
    // doc: 已由 UIParser 解析的 XML 文档
    // 返回构建结果，包含根控件指针
    UIBuildResult build(const pugi::xml_document& doc);

    // 从 DSL AST 构建控件树（阶段四入口之一）。
    // roots: 由 Parser 产出（可经 SemanticAnalyzer / ASTOptimizer）的 AST 节点
    UIBuildResult build(const std::vector<dsl::ASTNode>& roots);

    // 全流程便捷入口：DSL 源码 -> Lexer -> Parser -> SemanticAnalyzer ->
    // ASTOptimizer -> UIBuilder，产出控件树。
    // 成功时返回 result.success == true。
    static UIBuildResult build_from_dsl_source(
        const std::string& source,
        std::vector<dsl::SemanticError>* semantic_errors = nullptr);

    // 获取 .uif → Widget 类型名映射表
    static const std::unordered_map<std::string, std::string>& type_aliases();

    // 注册自定义类型映射
    void register_type_alias(const char* uif_name, const char* widget_type);

    // 设置是否在构建失败时释放已创建的控件
    void set_cleanup_on_failure(bool cleanup) { cleanup_on_failure_ = cleanup; }
    bool cleanup_on_failure() const { return cleanup_on_failure_; }

private:
    bool cleanup_on_failure_ = true;

    // 递归构建控件树（XML）
    Widget* build_node(const pugi::xml_node& xml_node, int& count);

    // 递归构建控件树（DSL AST）
    Widget* build_ast_node(const dsl::ASTNode& node, int& count);

    // 将 DSL ASTValue 转成 set_property 用的字符串
    static std::string ast_value_to_string(const dsl::ASTValue& v);

    // 将 .uif 类型名映射到 Widget 类型名
    std::string resolve_type_name(const char* uif_name) const;

    // 用户自定义类型映射
    std::unordered_map<std::string, std::string> custom_aliases_;

    // 已创建的控件列表（用于失败时清理）
    std::vector<Widget*> created_widgets_;
};

} // namespace GryceEngineUtils::ui