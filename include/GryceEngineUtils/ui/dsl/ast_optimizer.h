#pragma once

// GryceEngineUtils::ui::dsl::ast_optimizer.h — .uif (DSL) AST 优化器
//
// 在语义分析通过之后、构建控件树之前，对 AST 做无损优化：
//   - foldConstants：样式/结构无关的常量规整（当前 DSL 属性为字面量，
//     主要保留字段归一化，为未来表达式留接口）
//   - removeEmptyNodes：移除没有属性且没有子节点的空控件
//   - deduplicateAttributes：冗余属性去重（保留最后一个，与解析器一致）
//   - expandStyles：展开 class/style 引用的样式块为内联属性
//
// 保证优化前后语义等价，不引入、不丢失任何控件。

#include "GryceEngineUtils/ui/dsl/ast.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace GryceEngineUtils::ui::dsl {

class ASTOptimizer {
public:
    ASTOptimizer() = default;

    // 注册一个样式块：className -> 内联属性集合
    void register_style(const std::string& className,
                        const std::unordered_map<std::string, ASTValue>& props);

    void optimize(ASTNode& root);
    void optimize(std::vector<ASTNode>& roots);

    // 优化过程中产生的告警（如重复属性被合并）
    const std::vector<std::string>& warnings() const { return m_warnings; }
    void clear_warnings() { m_warnings.clear(); }

private:
    void visit(ASTNode& n);
    void foldConstants(ASTNode& n);
    void removeEmptyNodes(ASTNode& n);
    void deduplicateAttributes(ASTNode& n);
    void expandStyles(ASTNode& n);      // 复用样式块（等效 stylesheet 语义）
    void visitChildren(ASTNode& n);

    std::unordered_map<std::string, std::unordered_map<std::string, ASTValue>> m_styles;
    std::vector<std::string> m_warnings;
};

} // namespace GryceEngineUtils::ui::dsl