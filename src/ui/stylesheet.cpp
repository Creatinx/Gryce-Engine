#include "GryceEngineUtils/ui/stylesheet.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace GryceEngineUtils::ui {

namespace {

// 样式属性位掩码：记录规则里显式设置了哪些字段
enum StyleField : uint32_t {
    F_BG           = 1u << 0,
    F_BG_HOVER     = 1u << 1,
    F_BG_PRESSED   = 1u << 2,
    F_COLOR        = 1u << 3,
    F_FONT_SIZE    = 1u << 4,
    F_RADIUS       = 1u << 5,
    F_BORDER_W     = 1u << 6,
    F_BORDER_COLOR = 1u << 7,
    F_PADDING      = 1u << 8,
    F_MARGIN       = 1u << 9,
    F_OPACITY      = 1u << 10,
    F_FONT_FAMILY  = 1u << 11,
    F_TEXT_ALIGN   = 1u << 12,
    F_SHADOW       = 1u << 13,
};

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parse_rgb_list(const std::string& inner, float* out, int count, float* alpha_out = nullptr) {
    std::string s = inner;
    for (auto& c : s) {
        if (c == ',' || c == '(' || c == ')') c = ' ';
    }
    std::istringstream ss(s);
    float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int got = 0;
    while (got < 4 && (ss >> v[got])) ++got;
    if (got < count) return false;
    for (int i = 0; i < count; ++i) {
        out[i] = v[i];
    }
    if (alpha_out && got >= 4) *alpha_out = v[3];
    return true;
}

} // namespace

std::vector<std::string> StyleSheet::split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            parts.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(trim(cur));
    return parts;
}

std::string StyleSheet::trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

bool StyleSheet::parse_color(const std::string& raw, Color& out) {
    std::string text = lowercase(trim(raw));
    if (text.empty()) return false;

    if (text[0] == '#') {
        const std::string hex = text.substr(1);
        auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return 0;
        };
        if (hex.size() == 3 || hex.size() == 4) {
            const float r = static_cast<float>(hex_val(hex[0]) * 17) / 255.0f;
            const float g = static_cast<float>(hex_val(hex[1]) * 17) / 255.0f;
            const float b = static_cast<float>(hex_val(hex[2]) * 17) / 255.0f;
            const float a = hex.size() == 4 ? static_cast<float>(hex_val(hex[3]) * 17) / 255.0f : 1.0f;
            out = Color(r, g, b, a);
            return true;
        }
        if (hex.size() == 6 || hex.size() == 8) {
            auto byte = [&](size_t i) -> int {
                return hex_val(hex[i]) * 16 + hex_val(hex[i + 1]);
            };
            const float r = static_cast<float>(byte(0)) / 255.0f;
            const float g = static_cast<float>(byte(2)) / 255.0f;
            const float b = static_cast<float>(byte(4)) / 255.0f;
            const float a = hex.size() == 8 ? static_cast<float>(byte(6)) / 255.0f : 1.0f;
            out = Color(r, g, b, a);
            return true;
        }
        return false;
    }

    if (text.rfind("rgb", 0) == 0) {
        const size_t open = text.find('(');
        const size_t close = text.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close <= open) return false;
        float v[4] = {0, 0, 0, 1};
        const bool has_alpha = text.rfind("rgba", 0) == 0;
        if (!parse_rgb_list(text.substr(open + 1, close - open - 1), v, 3, &v[3])) return false;
        out = Color(v[0], v[1], v[2], has_alpha ? v[3] : 1.0f);
        return true;
    }

    if (text == "white") out = Color::white();
    else if (text == "black") out = Color::black();
    else if (text == "red") out = Color::red();
    else if (text == "green") out = Color::green();
    else if (text == "blue") out = Color::blue();
    else if (text == "yellow") out = Color::yellow();
    else if (text == "cyan") out = Color::cyan();
    else if (text == "magenta") out = Color::magenta();
    else if (text == "gray" || text == "grey") out = Color::gray(0.5f);
    else if (text == "orange") out = Color::orange();
    else if (text == "transparent") out = Color(0, 0, 0, 0);
    else return false;
    return true;
}

float StyleSheet::parse_length(const std::string& text, float fallback) {
    std::string s = trim(text);
    if (s.empty()) return fallback;
    // 去掉 px 后缀
    if (s.size() >= 2 && (s.compare(s.size() - 2, 2, "px") == 0)) {
        s = s.substr(0, s.size() - 2);
    }
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    return end == s.c_str() ? fallback : v;
}

bool StyleSheet::SimpleSelector::matches(const Widget* w) const {
    if (!w) return false;
    if (!type.empty() && type != w->type_name()) return false;
    if (!id.empty() && id != w->style_id() && id != w->id()) return false;
    if (!cls.empty() && cls != w->style_class()) return false;
    switch (pseudo) {
        case Pseudo::Hover: return w->hovered();
        case Pseudo::Pressed: return w->pressed();
        case Pseudo::Focused: return w->focused();
        case Pseudo::Disabled: return !w->enabled();
        case Pseudo::None: break;
    }
    return true;
}

bool StyleSheet::CompoundSelector::matches(const Widget* w) const {
    if (chain.empty() || !w) return false;
    if (!chain.back().matches(w)) return false;

    const Widget* cur = w;
    for (int i = static_cast<int>(chain.size()) - 2; i >= 0; --i) {
        const SimpleSelector& sel = chain[static_cast<size_t>(i)];
        bool found = false;
        if (child_only) {
            // 相邻子选择器：父节点必须直接匹配
            if (cur->parent() && sel.matches(cur->parent())) {
                cur = cur->parent();
                found = true;
            }
        } else {
            // 后代选择器：向上查找任意匹配祖先
            const Widget* anc = cur->parent();
            while (anc) {
                if (sel.matches(anc)) {
                    cur = anc;
                    found = true;
                    break;
                }
                anc = anc->parent();
            }
        }
        if (!found) return false;
    }
    return true;
}

bool StyleSheet::parse(const char* css_text) {
    if (!css_text) return false;
    std::string css(css_text);

    // 去掉注释
    while (true) {
        const size_t b = css.find("/*");
        if (b == std::string::npos) break;
        const size_t e = css.find("*/", b + 2);
        if (e == std::string::npos) {
            css.erase(b);
            break;
        }
        css.erase(b, e - b + 2);
    }

    size_t pos = 0;
    bool ok = true;
    while (pos < css.size()) {
        const size_t brace_open = css.find('{', pos);
        if (brace_open == std::string::npos) break;
        const size_t brace_close = css.find('}', brace_open);
        if (brace_close == std::string::npos) {
            ok = false;
            break;
        }
        const std::string selector_text = css.substr(pos, brace_open - pos);
        const std::string body = css.substr(brace_open + 1, brace_close - brace_open - 1);
        ok = parse_rule(selector_text, body) && ok;
        pos = brace_close + 1;
    }
    return ok;
}

bool StyleSheet::load_from_file(const char* path) {
    if (!path) return false;
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

bool StyleSheet::parse_rule(const std::string& selector_text, const std::string& body) {
    Rule rule;
    for (const auto& sel_part : split(selector_text, ',')) {
        if (sel_part.empty()) continue;
        CompoundSelector compound;
        compound.child_only = true; // 相邻元素用 '>'，其余按后代

        // 以 '>' 切分后，非 '>' 相邻的片段再按空白切分
        std::vector<std::string> tokens;
        std::string cur;
        bool last_was_child = false;
        bool any_child_op = false;
        for (size_t i = 0; i <= sel_part.size(); ++i) {
            const char c = i < sel_part.size() ? sel_part[i] : ' ';
            if (c == '>') {
                if (!cur.empty()) {
                    tokens.push_back(trim(cur));
                    cur.clear();
                    if (!last_was_child) any_child_op = true;
                }
                last_was_child = true;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) {
                    tokens.push_back(trim(cur));
                    cur.clear();
                    last_was_child = false;
                }
            } else {
                cur.push_back(c);
            }
        }
        if (tokens.empty()) continue;

        for (const auto& tok : tokens) {
            SimpleSelector sel;
            std::string t = trim(tok);
            size_t i = 0;
            // 标签
            while (i < t.size() && t[i] != '#' && t[i] != '.' && t[i] != ':') ++i;
            sel.type = t.substr(0, i);
            while (i < t.size()) {
                if (t[i] == '#') {
                    size_t j = i + 1;
                    while (j < t.size() && t[j] != '.' && t[j] != ':') ++j;
                    sel.id = t.substr(i + 1, j - i - 1);
                    i = j;
                } else if (t[i] == '.') {
                    size_t j = i + 1;
                    while (j < t.size() && t[j] != '#' && t[j] != ':') ++j;
                    sel.cls = t.substr(i + 1, j - i - 1);
                    i = j;
                } else if (t[i] == ':') {
                    const std::string pseudo = lowercase(t.substr(i + 1));
                    if (pseudo == "hover") sel.pseudo = Pseudo::Hover;
                    else if (pseudo == "pressed") sel.pseudo = Pseudo::Pressed;
                    else if (pseudo == "focus" || pseudo == "focused") sel.pseudo = Pseudo::Focused;
                    else if (pseudo == "disabled") sel.pseudo = Pseudo::Disabled;
                    break;
                } else {
                    ++i;
                }
            }
            compound.chain.push_back(std::move(sel));
        }
        // 一旦出现 '>' 就要求严格父子；纯空格分隔为后代
        if (!any_child_op) compound.child_only = false;
        rule.selectors.push_back(std::move(compound));
    }
    if (rule.selectors.empty()) return false;

    // 解析声明
    for (const auto& decl : split(body, ';')) {
        if (decl.empty()) continue;
        const size_t colon = decl.find(':');
        if (colon == std::string::npos) continue;
        const std::string prop = lowercase(trim(decl.substr(0, colon)));
        const std::string value = trim(decl.substr(colon + 1));
        if (value.empty()) continue;

        uint32_t& mask = rule.mask;
        Style& s = rule.style;
        if (prop == "background") {
            if (parse_color(value, s.background)) { s.has_background = true; mask |= F_BG; }
        } else if (prop == "background-hover") {
            if (parse_color(value, s.background_hover)) mask |= F_BG_HOVER;
        } else if (prop == "background-pressed") {
            if (parse_color(value, s.background_pressed)) mask |= F_BG_PRESSED;
        } else if (prop == "color") {
            if (parse_color(value, s.color)) mask |= F_COLOR;
        } else if (prop == "font-size") {
            s.font_size = parse_length(value, s.font_size);
            mask |= F_FONT_SIZE;
        } else if (prop == "border-radius") {
            s.border_radius = parse_length(value, s.border_radius);
            mask |= F_RADIUS;
        } else if (prop == "border") {
            // border: 1px solid #333;
            const auto parts = split(value, ' ');
            for (const auto& p : parts) {
                if (p.find('#') == 0 || p.rfind("rgb", 0) == 0) {
                    Color c;
                    if (parse_color(p, c)) { s.border_color = c; mask |= F_BORDER_COLOR; }
                } else {
                    const float v = parse_length(p, -1.0f);
                    if (v >= 0.0f) { s.border_width = v; mask |= F_BORDER_W; }
                }
            }
        } else if (prop == "padding") {
            const auto vals = split(value, ' ');
            if (vals.size() == 1) {
                const float v = parse_length(vals[0], 0);
                s.padding = Padding{v, v, v, v};
            } else if (vals.size() == 2) {
                const float v = parse_length(vals[0], 0);
                const float h = parse_length(vals[1], 0);
                s.padding = Padding{h, v, h, v};
            } else if (vals.size() == 4) {
                s.padding = Padding{parse_length(vals[0], 0), parse_length(vals[1], 0),
                                    parse_length(vals[2], 0), parse_length(vals[3], 0)};
            }
            mask |= F_PADDING;
        } else if (prop == "margin") {
            const auto vals = split(value, ' ');
            if (vals.size() == 1) {
                const float v = parse_length(vals[0], 0);
                s.margin = Margin{v, v, v, v};
            } else if (vals.size() == 2) {
                const float v = parse_length(vals[0], 0);
                const float h = parse_length(vals[1], 0);
                s.margin = Margin{h, v, h, v};
            } else if (vals.size() == 4) {
                s.margin = Margin{parse_length(vals[0], 0), parse_length(vals[1], 0),
                                  parse_length(vals[2], 0), parse_length(vals[3], 0)};
            }
            mask |= F_MARGIN;
        } else if (prop == "opacity") {
            const float v = parse_length(value, 1.0f);
            s.opacity = std::clamp(v, 0.0f, 1.0f);
            mask |= F_OPACITY;
        } else if (prop == "font-family") {
            s.font_family = value;
            mask |= F_FONT_FAMILY;
        } else if (prop == "text-align") {
            const std::string a = lowercase(value);
            if (a == "center") s.text_align = TextAlign::Center;
            else if (a == "right") s.text_align = TextAlign::Right;
            else s.text_align = TextAlign::Left;
            mask |= F_TEXT_ALIGN;
        } else if (prop == "box-shadow") {
            // box-shadow: 0 2px 4px rgba(0,0,0,0.3);
            const auto parts = split(value, ' ');
            float vals[3] = {0.0f, 2.0f, 4.0f};
            int got = 0;
            Color c(0, 0, 0, 0.3f);
            for (const auto& p : parts) {
                Color pc;
                if (parse_color(p, pc)) {
                    c = pc;
                } else if (got < 3) {
                    vals[got++] = parse_length(p, 0.0f);
                }
            }
            s.box_shadow = {true, vals[0], vals[1], vals[2], c};
            mask |= F_SHADOW;
        }
    }

    rules_.push_back(std::move(rule));
    return true;
}

Style StyleSheet::resolve(const Widget* widget) const {
    Style result = widget ? widget->style_ : Style{};
    for (const auto& rule : rules_) {
        bool matched = false;
        for (const auto& sel : rule.selectors) {
            if (sel.matches(widget)) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;
        const Style& s = rule.style;
        const uint32_t mask = rule.mask;
        if (mask & F_BG) { result.background = s.background; result.has_background = true; }
        if (mask & F_BG_HOVER) result.background_hover = s.background_hover;
        if (mask & F_BG_PRESSED) result.background_pressed = s.background_pressed;
        if (mask & F_COLOR) result.color = s.color;
        if (mask & F_FONT_SIZE) result.font_size = s.font_size;
        if (mask & F_RADIUS) result.border_radius = s.border_radius;
        if (mask & F_BORDER_W) result.border_width = s.border_width;
        if (mask & F_BORDER_COLOR) result.border_color = s.border_color;
        if (mask & F_PADDING) result.padding = s.padding;
        if (mask & F_MARGIN) result.margin = s.margin;
        if (mask & F_OPACITY) result.opacity = s.opacity;
        if (mask & F_FONT_FAMILY) result.font_family = s.font_family;
        if (mask & F_TEXT_ALIGN) result.text_align = s.text_align;
        if (mask & F_SHADOW) result.box_shadow = s.box_shadow;
    }
    return result;
}

} // namespace GryceEngineUtils::ui
