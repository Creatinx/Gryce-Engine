// GryceEngineUtils::ui::dsl::semantic_analyzer.cpp
#include "GryceEngineUtils/ui/dsl/semantic_analyzer.h"

#include <algorithm>
#include <sstream>

namespace GryceEngineUtils::ui::dsl {

// ============================================================================
// SemanticError
// ============================================================================
std::string SemanticError::toString() const {
    std::ostringstream oss;
    oss << line << ":" << column << " - " << message;
    if (!hint.empty()) {
        oss << "\nHint: " << hint;
    }
    return oss.str();
}

// ============================================================================
// 辅助（匿名命名空间）
// ============================================================================
namespace {

// 属性名 -> 期望值类型。继承自 uif_parser.cpp 的白名单并按字面量语义归类：
//   尺寸/数值 -> Number（允许 Identifier 表示 auto 等关键字）
//   布尔     -> Boolean（允许 Identifier 表示数据绑定）
//   其余     -> String（允许 Identifier 表示枚举/样式名）
enum class Expect { Number, Boolean, Any };

Expect expect_for(const std::string& attr) {
    static const std::vector<std::string> numeric = {
        "width", "height", "margin", "padding", "spacing", "font-size",
        "min", "max", "value", "step"
    };
    if (std::find(numeric.begin(), numeric.end(), attr) != numeric.end()) {
        return Expect::Number;
    }
    if (attr == "checked" || attr == "visible" || attr == "enabled") {
        return Expect::Boolean;
    }
    return Expect::Any;
}

const char* kind_name(ASTValue::Kind k) {
    switch (k) {
        case ASTValue::Kind::String:     return "string";
        case ASTValue::Kind::Number:     return "number";
        case ASTValue::Kind::Boolean:    return "boolean";
        case ASTValue::Kind::Identifier: return "identifier";
        case ASTValue::Kind::Array:      return "array";
    }
    return "unknown";
}

bool value_matches_kind(const ASTValue& v, Expect e) {
    switch (e) {
        case Expect::Number:
            return v.is(ASTValue::Kind::Number) || v.is(ASTValue::Kind::Identifier);
        case Expect::Boolean:
            return v.is(ASTValue::Kind::Boolean) || v.is(ASTValue::Kind::Identifier);
        case Expect::Any:
            return v.is(ASTValue::Kind::String) || v.is(ASTValue::Kind::Identifier);
    }
    return true;
}

// Levenshtein 编辑距离（用于拼写建议）
std::size_t edit_distance(const std::string& a, const std::string& b) {
    const std::size_t n = a.size(), m = b.size();
    std::vector<std::size_t> prev(m + 1), cur(m + 1);
    for (std::size_t j = 0; j <= m; ++j) {
        prev[j] = j;
    }
    for (std::size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= m; ++j) {
            cur[j] = std::min({
                prev[j] + 1,
                cur[j - 1] + 1,
                prev[j - 1] + (a[i - 1] == b[j - 1] ? 0u : 1u)
            });
        }
        prev.swap(cur);
    }
    return prev[m];
}

std::vector<std::string> str_list(std::initializer_list<const char*> l) {
    std::vector<std::string> out;
    for (const char* s : l) {
        out.emplace_back(s);
    }
    return out;
}

} // namespace

// ============================================================================
// 内置控件 schema（对齐 uif_parser.cpp 的 15 控件白名单）
// ============================================================================
const std::vector<ControlSchema>& SemanticAnalyzer::builtin_schemas() {
    static const std::vector<ControlSchema> schemas = [] {
        std::vector<ControlSchema> s;
        auto add = [&s](const char* type,
                        const std::vector<std::string>& attrs,
                        const std::vector<std::string>& required) {
            ControlSchema c;
            c.typeName = type;
            for (const auto& a : attrs) {
                c.allowed.emplace(a, ASTValue::Kind::String);
            }
            c.required = required;
            s.push_back(std::move(c));
        };

        add("Window", str_list({"id", "title", "layout", "padding", "spacing",
                                "align", "width", "height", "margin", "style",
                                "bgcolor", "visible", "enabled"}), {});
        add("Panel", str_list({"id", "layout", "padding", "spacing", "align",
                               "width", "height", "margin", "style", "bgcolor",
                               "visible", "enabled"}), {});
        add("Text", str_list({"id", "text", "color", "font-size", "bgcolor",
                              "align", "width", "height", "margin", "style",
                              "visible"}), {});
        add("Button", str_list({"id", "text", "style", "color", "font-size",
                                "bgcolor", "width", "height", "margin",
                                "padding", "onClick", "visible", "enabled"}),
            str_list({"text"}));
        add("Image", str_list({"id", "src", "width", "height", "margin",
                               "visible"}), str_list({"src"}));
        add("List", str_list({"id", "width", "height", "margin", "padding",
                              "spacing", "visible", "enabled"}), {});
        add("Slider", str_list({"id", "min", "max", "value", "width", "height",
                                "margin", "onChange", "visible", "enabled"}), {});
        add("TextInput", str_list({"id", "text", "placeholder", "width",
                                   "height", "margin", "onChange", "visible",
                                   "enabled"}), {});
        add("CheckBox", str_list({"id", "text", "checked", "width", "margin",
                                  "onChange", "visible", "enabled"}), {});
        add("ProgressBar", str_list({"id", "value", "min", "max", "width",
                                     "height", "margin", "visible"}), {});
        add("ScrollView", str_list({"id", "width", "height", "margin",
                                    "padding", "visible"}), {});
        add("Divider", str_list({"id", "width", "height", "margin", "color",
                                 "visible"}), {});
        add("ComboBox", str_list({"id", "width", "height", "margin",
                                  "onChange", "visible", "enabled"}), {});
        add("RadioButton", str_list({"id", "text", "checked", "group", "width",
                                     "margin", "onChange", "visible",
                                     "enabled"}), {});
        add("SpinBox", str_list({"id", "value", "min", "max", "step", "width",
                                 "margin", "onChange", "visible", "enabled"}), {});
        return s;
    }();
    return schemas;
}

bool SemanticAnalyzer::is_known_control(const std::string& type) {
    for (const auto& s : builtin_schemas()) {
        if (s.typeName == type) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// SemanticAnalyzer
// ============================================================================
SemanticAnalyzer::SemanticAnalyzer()
    : m_schemas(builtin_schemas()) {}

void SemanticAnalyzer::addError(int line, int column, std::string message,
                                std::string hint) {
    SemanticError e;
    e.line = line;
    e.column = column;
    e.message = std::move(message);
    e.hint = std::move(hint);
    m_errors.push_back(std::move(e));
}

const ControlSchema* SemanticAnalyzer::find_schema(const std::string& type) const {
    for (const auto& s : m_schemas) {
        if (s.typeName == type) {
            return &s;
        }
    }
    return nullptr;
}

std::string SemanticAnalyzer::suggest_control(const std::string& type) const {
    const ControlSchema* best = nullptr;
    std::size_t bestD = std::string::npos;
    for (const auto& s : m_schemas) {
        std::size_t d = edit_distance(type, s.typeName);
        if (d <= 2 && d < bestD) {
            bestD = d;
            best = &s;
        }
    }
    return best ? best->typeName : std::string();
}

void SemanticAnalyzer::checkAttributes(const ASTNode& node, const ControlSchema& schema) {
    for (const auto& kv : node.attributes) {
        const std::string& name = kv.first;
        const ASTValue& value = kv.second;

        auto it = schema.allowed.find(name);
        if (it == schema.allowed.end()) {
            addError(value.line, value.column,
                     "Unknown attribute '" + name + "' for " + node.type + ".",
                     "Valid attributes: see control documentation / schema.");
            continue;
        }

        if (!value_matches_kind(value, expect_for(name))) {
            addError(value.line, value.column,
                     "Attribute '" + name + "' of " + node.type +
                         " expects " +
                         (expect_for(name) == Expect::Number ? "number" :
                          expect_for(name) == Expect::Boolean ? "boolean" : "string") +
                         ", got " + kind_name(value.kind) + ".");
        }
    }
}

void SemanticAnalyzer::visit(const ASTNode& node) {
    const ControlSchema* schema = find_schema(node.type);
    if (!schema) {
        std::string hint;
        std::string sug = suggest_control(node.type);
        if (!sug.empty()) {
            hint = "Did you mean '" + sug + "'?";
        } else {
            hint = "Control type is not registered.";
        }
        addError(node.line, node.column,
                 "Unknown control '" + node.type + "'.", hint);
        // 未知控件：仍检查其子节点（跳过属性校验）
        for (const auto& child : node.children) {
            visit(child);
        }
        return;
    }

    checkAttributes(node, *schema);

    // 必需属性检查
    for (const auto& req : schema->required) {
        if (!node.hasAttribute(req)) {
            addError(node.line, node.column,
                     node.type + " missing required attribute '" + req + "'.");
        }
    }

    for (const auto& child : node.children) {
        visit(child);
    }
}

bool SemanticAnalyzer::analyze(ASTNode& root) {
    m_errors.clear();
    visit(root);
    return m_errors.empty();
}

bool SemanticAnalyzer::analyze(const std::vector<ASTNode>& roots) {
    m_errors.clear();
    for (const auto& root : roots) {
        visit(root);
    }
    return m_errors.empty();
}

} // namespace GryceEngineUtils::ui::dsl