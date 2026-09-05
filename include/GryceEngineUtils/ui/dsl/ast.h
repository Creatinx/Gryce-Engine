#pragma once

// GryceEngineUtils::ui::dsl::ast.h — .uif (DSL) 抽象语法树
//
// AST 是 DSL 解析的中间表示，由 Parser 从 Lexer 的 Token 流构建，
// 供 SemanticAnalyzer / ASTOptimizer / UIBuilder(DSL) 消费。
// 其生命周期只存在于一次 parse 调用期间，由调用方持有；控件树由
// UIBuilder 另行构建并归 UIManager 管理。

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace GryceEngineUtils::ui::dsl {

// ---------------------------------------------------------------------------
// ASTValue — 属性值的字面量
// ---------------------------------------------------------------------------
struct ASTValue {
    enum class Kind { String, Number, Boolean, Identifier, Array };

    Kind kind = Kind::String;
    std::string stringValue;                 // String
    double numberValue = 0.0;                // Number
    bool boolValue = false;                  // Boolean
    std::vector<ASTValue> arrayValue;        // Array

    int line = 0;                            // 1-based
    int column = 0;                          // 1-based

    // 便捷构造
    static ASTValue makeString(const std::string& v) {
        ASTValue a; a.kind = Kind::String; a.stringValue = v; return a;
    }
    static ASTValue makeNumber(double v) {
        ASTValue a; a.kind = Kind::Number; a.numberValue = v; return a;
    }
    static ASTValue makeBoolean(bool v) {
        ASTValue a; a.kind = Kind::Boolean; a.boolValue = v; return a;
    }
    static ASTValue makeIdentifier(const std::string& v) {
        ASTValue a; a.kind = Kind::Identifier; a.stringValue = v; return a;
    }

    bool is(Kind k) const { return kind == k; }
};

// ---------------------------------------------------------------------------
// ASTNode — 单个控件节点
// ---------------------------------------------------------------------------
struct ASTNode {
    std::string type;                                    // 控件类型名
    std::unordered_map<std::string, ASTValue> attributes;
    std::vector<ASTNode> children;
    int line = 0;                                        // 1-based
    int column = 0;                                      // 1-based

    bool hasAttribute(const std::string& key) const {
        return attributes.count(key) != 0;
    }

    // 读取属性值；不存在或类型不符时返回 false
    bool getString(const std::string& key, std::string& out) const;
    bool getNumber(const std::string& key, double& out) const;
    bool getBool(const std::string& key, bool& out) const;
};

inline bool ASTNode::getString(const std::string& key, std::string& out) const {
    auto it = attributes.find(key);
    if (it == attributes.end() || !it->second.is(ASTValue::Kind::String)) {
        return false;
    }
    out = it->second.stringValue;
    return true;
}

inline bool ASTNode::getNumber(const std::string& key, double& out) const {
    auto it = attributes.find(key);
    if (it == attributes.end() || !it->second.is(ASTValue::Kind::Number)) {
        return false;
    }
    out = it->second.numberValue;
    return true;
}

inline bool ASTNode::getBool(const std::string& key, bool& out) const {
    auto it = attributes.find(key);
    if (it == attributes.end() || !it->second.is(ASTValue::Kind::Boolean)) {
        return false;
    }
    out = it->second.boolValue;
    return true;
}

} // namespace GryceEngineUtils::ui::dsl