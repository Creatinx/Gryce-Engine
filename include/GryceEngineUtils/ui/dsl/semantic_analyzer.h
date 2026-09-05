#pragma once

// GryceEngineUtils::ui::dsl::semantic_analyzer.h — .uif (DSL) 语义检查器
//
// 对 Parser 产出的 AST 做语义校验（区别于 Syntax 错误）：
//   - UnknownControl：控件类型未在 schema 注册（含 Levenshtein 拼写建议）
//   - UnknownAttribute：属性名不在该控件白名单
//   - InvalidType：属性值类型不匹配（如 width 期望 number）
//   - MissingAttribute：必需属性缺失
//
// 控件 schema 与真实引擎控件库对齐（src/ui/uif_parser.cpp 的白名单）。
// 语义错误全部收集而非中断，供 ErrorCollector / 报错格式化统一输出。

#include <string>
#include <unordered_map>
#include <vector>

#include "GryceEngineUtils/ui/dsl/ast.h"

namespace GryceEngineUtils::ui::dsl {

// ---------------------------------------------------------------------------
// 控件 schema
// ---------------------------------------------------------------------------
struct ControlSchema {
    std::string typeName;
    std::unordered_map<std::string, ASTValue::Kind> allowed;  // 期望属性值类型
    std::vector<std::string> required;                        // 必需属性
};

// ---------------------------------------------------------------------------
// 语义错误
// ---------------------------------------------------------------------------
struct SemanticError {
    int line = 0;            // 1-based
    int column = 0;          // 1-based
    std::string message;
    std::string hint;

    std::string toString() const;
};

// ---------------------------------------------------------------------------
// SemanticAnalyzer
// ---------------------------------------------------------------------------
class SemanticAnalyzer {
public:
    SemanticAnalyzer();

    // 校验整个 AST（根节点可含多个子控件）；成功返回 true，否则收集错误
    bool analyze(ASTNode& root);
    bool analyze(const std::vector<ASTNode>& roots);

    bool hasErrors() const { return !m_errors.empty(); }
    const std::vector<SemanticError>& errors() const { return m_errors; }

    // 注册的控件类型集合（供错误报告/脚本用）
    static const std::vector<ControlSchema>& builtin_schemas();
    static bool is_known_control(const std::string& type);

private:
    void visit(const ASTNode& node);
    void checkAttributes(const ASTNode& node, const ControlSchema& schema);
    const ControlSchema* find_schema(const std::string& type) const;

    // 在注册控件中寻找编辑距离 <= 2 的建议
    std::string suggest_control(const std::string& type) const;

    void addError(int line, int column, std::string message,
                  std::string hint = "");

    const std::vector<ControlSchema>& m_schemas;
    std::vector<SemanticError> m_errors;
};

} // namespace GryceEngineUtils::ui::dsl