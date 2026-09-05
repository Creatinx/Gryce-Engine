#pragma once

// GryceEngineUtils::ui::stylesheet.h — 样式表（CSS 子集，运行时解析）
//
// 支持选择器：标签 / #id / .class / :hover / :pressed / 子选择器 > / 后代选择器
// 支持属性：background, background-hover, background-pressed, color,
// font-size, border-radius, border, padding, margin, opacity,
// font-family, text-align, box-shadow

#include <string>
#include <vector>

#include "GryceEngineUtils/ui/widget.h"

namespace GryceEngineUtils::ui {

class Widget;

class StyleSheet {
public:
    StyleSheet() = default;

    // 解析 CSS 文本（可多次调用，追加规则）
    bool parse(const char* css_text);
    bool parse(const std::string& css_text) { return parse(css_text.c_str()); }
    bool load_from_file(const char* path);

    // 计算某个 Widget 的最终样式（按规则顺序级联，伪类优先）
    Style resolve(const Widget* widget) const;

    void clear() { rules_.clear(); }
    bool empty() const { return rules_.empty(); }

private:
    enum class Pseudo { None, Hover, Pressed, Focused, Disabled };

    struct SimpleSelector {
        std::string type;      // 标签（空 = 任意）
        std::string id;        // #id
        std::string cls;       // .class
        Pseudo pseudo = Pseudo::None;

        bool matches(const Widget* w) const;
    };

    struct CompoundSelector {
        std::vector<SimpleSelector> chain; // 从祖先到自身
        bool child_only = false;           // 相邻元素用 '>' 连接
        bool matches(const Widget* w) const;
    };

    struct Rule {
        std::vector<CompoundSelector> selectors;
        Style style;
        uint32_t mask = 0; // 显式设置的属性位掩码
    };

    bool parse_rule(const std::string& selector_text, const std::string& body);
    void parse_declaration(const std::string& line, Style& out) const;

    static bool parse_color(const std::string& text, Color& out);
    static float parse_length(const std::string& text, float fallback);
    static std::vector<std::string> split(const std::string& s, char sep);
    static std::string trim(const std::string& s);

    std::vector<Rule> rules_;
};

} // namespace GryceEngineUtils::ui
