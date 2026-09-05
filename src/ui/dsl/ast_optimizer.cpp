// GryceEngineUtils::ui::dsl::ast_optimizer.cpp
#include "GryceEngineUtils/ui/dsl/ast_optimizer.h"

namespace GryceEngineUtils::ui::dsl {

void ASTOptimizer::register_style(
    const std::string& className,
    const std::unordered_map<std::string, ASTValue>& props) {
    m_styles[className] = props;
}

// ============================================================================
// 常量规整：当前 DSL 属性值是字面量，无需实际折叠；
// 保留该遍以确保与未来表达式支持保持一致的接口（语义等价 no-op）。
// ============================================================================
void ASTOptimizer::foldConstants(ASTNode&) {
    // 无操作：DSL 字面量已原子化，常量化由 Lexer/Parser 完成。
}

// ============================================================================
// 空节点移除：移除没有属性且没有子节点的叶子控件。
// 该操作无损语义（空控件在渲染层面与不存在等价）。
// ============================================================================
void ASTOptimizer::removeEmptyNodes(ASTNode& n) {
    auto& children = n.children;
    for (std::size_t i = 0; i < children.size();) {
        // 先递归处理子节点的子树
        removeEmptyNodes(children[i]);
        if (children[i].attributes.empty() && children[i].children.empty()) {
            children.erase(children.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

// ============================================================================
// 重复属性去重：AST 的属性容器是 unordered_map，重复键已在 Parser 阶段
// 收敛为"保留最后一个"。此处复核并提供告警信息（优化前后语义等价）。
// ============================================================================
void ASTOptimizer::deduplicateAttributes(ASTNode& n) {
    // 结构上无需再删除（map 天然唯一键）；如需跟踪重复可在此告警。
    // 当前保持 no-op，保留接口以兼容未来使用有序/多值存储的实现。
}

// ============================================================================
// 样式展开：把 class/style 属性引用的样式块属性内联为节点属性，
// 且不覆盖节点上显式设置的属性。展开后移除样式引用属性。
// ============================================================================
void ASTOptimizer::expandStyles(ASTNode& n) {
    static const char* kStyleKeys[] = {"class", "style"};
    for (const char* key : kStyleKeys) {
        auto it = n.attributes.find(key);
        if (it == n.attributes.end()) {
            continue;
        }
        const ASTValue& v = it->second;
        std::string className = v.is(ASTValue::Kind::Identifier)
                                    ? v.stringValue
                                    : (v.is(ASTValue::Kind::String) ? v.stringValue : std::string());
        auto styleIt = m_styles.find(className);
        if (styleIt != m_styles.end()) {
            for (const auto& kv : styleIt->second) {
                // 显式属性优先，不覆盖
                if (!n.hasAttribute(kv.first)) {
                    n.attributes[kv.first] = kv.second;
                }
            }
        }
        // 移除样式引用属性
        n.attributes.erase(it);
    }
}

void ASTOptimizer::visitChildren(ASTNode& n) {
    for (auto& child : n.children) {
        visit(child);
    }
}

void ASTOptimizer::visit(ASTNode& n) {
    foldConstants(n);
    expandStyles(n);
    deduplicateAttributes(n);
    visitChildren(n);
    removeEmptyNodes(n);
}

void ASTOptimizer::optimize(ASTNode& root) {
    visit(root);
}

void ASTOptimizer::optimize(std::vector<ASTNode>& roots) {
    for (auto& root : roots) {
        visit(root);
    }
    // 移除顶层空控件（与 removeEmptyNodes 语义一致）
    for (std::size_t i = 0; i < roots.size();) {
        if (roots[i].attributes.empty() && roots[i].children.empty()) {
            roots.erase(roots.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

} // namespace GryceEngineUtils::ui::dsl