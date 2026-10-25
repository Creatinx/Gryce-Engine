// ===========================================================================
// engine/ui/ui_theme.cpp — 主题 token 的实现与 ImGui 落地
//
// token 表本身与 ImGui 无关（无 ImGui 的构建也编译本文件，apply() 退化
// 为 no-op）；只有 apply() 一个函数住在 #ifdef 里。
//
// 预设数据只到"锚色"为止（每套 17 个值），其余 40 个颜色 token 由
// derive_extended() 按固定规则从锚色算出。派生规则集中在一个函数里，
// 好处有两个：新预设不用抄 57 个值；"换预设后哪些 token 必须相等"
// 可以直接写单测（CHECK_MARK == ACCENT、SEPARATOR == BORDER ...）。
// ===========================================================================

#include "ui/ui_theme.h"

#include <cctype>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#ifdef GRYCE_HAS_IMGUI
#include <imgui.h>
#endif

namespace gryce_ui {

namespace {

// 下面定义在本文件后半段；load_theme_json 更早使用它们。
Theme& mutable_theme();
void   derive_extended(Theme& t);

// ---- 颜色小工具（0xRRGGBBAA） ----

uint8_t chan(uint32_t c, int shift) {
    return static_cast<uint8_t>((c >> shift) & 0xFFu);
}

uint32_t with_alpha(uint32_t rgba, uint8_t alpha) {
    return (rgba & 0xFFFFFF00u) | alpha;
}

// 逐通道线性混合（含 alpha），t 钳到 [0,1]。
uint32_t mix(uint32_t a, uint32_t b, float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float ia = 1.0f - t;
    auto ch = [&](int shift) {
        const float v = static_cast<float>(chan(a, shift)) * ia
                      + static_cast<float>(chan(b, shift)) * t;
        long iv = static_cast<long>(v + 0.5f);
        if (iv < 0) iv = 0;
        if (iv > 255) iv = 255;
        return static_cast<uint32_t>(iv);
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

// RGB 整体缩放（提亮/压暗），alpha 不变。
uint32_t shade(uint32_t rgba, float mul) {
    auto ch = [&](int shift) {
        const float v = static_cast<float>(chan(rgba, shift)) * mul;
        long iv = static_cast<long>(v + 0.5f);
        if (iv < 0) iv = 0;
        if (iv > 255) iv = 255;
        return static_cast<uint32_t>(iv);
    };
    return (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | chan(rgba, 0);
}

// ---- 预设锚色（每套 17 个，顺序 = ThemeAnchor 枚举） ----
//
// Dark / Light 是 Fluent（Win11）风格的现代扁平预设：低对比分层、控制件
// 4px 圆角、强调色只在"可点"与"已选"上出现。更早的高对比配色由
// HighContrast 预设承担；Midnight / Nord 是氛围向备选。

const uint32_t kAnchorsDark[THEME_ANCHOR_COUNT] = {
    0x202020FF,  // WINDOW_BG   Fluent 深色底（Mica 近似值）
    0x2C2C2CFF,  // CHILD_BG    卡片层：比底色亮一档
    0x383838FF,  // BORDER      ≈ 白色 10% 叠在底上，分隔线几乎不可见
    0xFFFFFFFF,  // TEXT
    0x9D9D9DFF,  // TEXT_DIM    ≈ 白色 60%
    0x4CC2FFFF,  // ACCENT      Win11 深色主题的亮蓝
    0x62C7FFFF,  // ACCENT_HOVER
    0x33A3E6FF,  // ACCENT_ACTIVE
    0x323232FF,  // BUTTON      ≈ 白色 7% 叠在底上：低对比的"次级填充"
    0x3A3A3AFF,  // BUTTON_HOVER
    0x2A2A2AFF,  // BUTTON_ACTIVE  按下反而更暗（Fluent 的触感）
    0x262626FF,  // HEADER      标题栏/菜单栏：比底色亮一点点
    0x2E2E2EFF,  // HEADER_HOVER
    0x2F596EFF,  // SELECTION   强调色 35% 混进底色
    0x6CCB5FFF,  // OK
    0xE7B958FF,  // WARN
    0xE74856FF,  // ERROR
};

const uint32_t kAnchorsLight[THEME_ANCHOR_COUNT] = {
    0xF3F3F3FF,  // WINDOW_BG   Fluent 浅色底（Mica 近似值）
    0xFFFFFFFF,  // CHILD_BG    卡片：纯白浮在灰底上
    0xDBDBDBFF,  // BORDER      ≈ 黑色 12%
    0x1B1B1BFF,  // TEXT
    0x5C5C5CFF,  // TEXT_DIM    ≈ 黑色 60%
    0x005FB8FF,  // ACCENT      Win11 浅色主题的深蓝
    0x1A73C7FF,  // ACCENT_HOVER
    0x0054A6FF,  // ACCENT_ACTIVE
    0xFBFBFBFF,  // BUTTON      近白：浅色下"次级填充"比底色更亮
    0xF4F4F4FF,  // BUTTON_HOVER
    0xEAEAEAFF,  // BUTTON_ACTIVE
    0xEEEEF0FF,  // HEADER
    0xE4E4E6FF,  // HEADER_HOVER
    0xCCE4F7FF,  // SELECTION   Fluent 浅色选区蓝
    0x0F7B0FFF,  // OK
    0xB98200FF,  // WARN
    0xC42B1CFF,  // ERROR
};

// 深蓝夜航：底色比默认深色更蓝更沉，强调色换成柔和的天空蓝。
const uint32_t kAnchorsMidnight[THEME_ANCHOR_COUNT] = {
    0x141822FF,  // WINDOW_BG
    0x0F131CFF,  // CHILD_BG
    0x2A3346FF,  // BORDER
    0xDCE3F0FF,  // TEXT
    0x8A94A8FF,  // TEXT_DIM
    0x6EA8FEFF,  // ACCENT
    0x8FC0FFFF,  // ACCENT_HOVER
    0x4E8BD8FF,  // ACCENT_ACTIVE
    0x1E2634FF,  // BUTTON
    0x283044FF,  // BUTTON_HOVER
    0x323B52FF,  // BUTTON_ACTIVE
    0x1A2130FF,  // HEADER
    0x232C3EFF,  // HEADER_HOVER
    0x2C4A6EFF,  // SELECTION
    0x71C97AFF,  // OK
    0xE3C568FF,  // WARN
    0xE07B7BFF,  // ERROR
};

// 北极青灰：Nord 调色板的深色半，强调色用 frost 青。
const uint32_t kAnchorsNord[THEME_ANCHOR_COUNT] = {
    0x2E3440FF,  // WINDOW_BG
    0x2B303BFF,  // CHILD_BG
    0x434C5EFF,  // BORDER
    0xECEFF4FF,  // TEXT
    0xA6B0C0FF,  // TEXT_DIM
    0x88C0D0FF,  // ACCENT
    0x8FBCBBFF,  // ACCENT_HOVER
    0x5E81ACFF,  // ACCENT_ACTIVE
    0x3B4252FF,  // BUTTON
    0x434C5EFF,  // BUTTON_HOVER
    0x4C566AFF,  // BUTTON_ACTIVE
    0x3B4252FF,  // HEADER
    0x434C5EFF,  // HEADER_HOVER
    0x5E81ACFF,  // SELECTION
    0xA3BE8CFF,  // OK
    0xEBCB8BFF,  // WARN
    0xBF616AFF,  // ERROR
};

// 纯黑高对比：给投影/户外场景的"看得清"预设，语义色全部拉到高饱和。
const uint32_t kAnchorsHighContrast[THEME_ANCHOR_COUNT] = {
    0x000000FF,  // WINDOW_BG
    0x0A0A0AFF,  // CHILD_BG
    0xE0E0E0FF,  // BORDER
    0xFFFFFFFF,  // TEXT
    0xC0C0C0FF,  // TEXT_DIM
    0x00B0FFFF,  // ACCENT
    0x40D0FFFF,  // ACCENT_HOVER
    0x0080C0FF,  // ACCENT_ACTIVE
    0x1A1A1AFF,  // BUTTON
    0x333333FF,  // BUTTON_HOVER
    0x4D4D4DFF,  // BUTTON_ACTIVE
    0x141414FF,  // HEADER
    0x242424FF,  // HEADER_HOVER
    0x004060FF,  // SELECTION
    0x00FF90FF,  // OK
    0xFFD000FF,  // WARN
    0xFF4040FF,  // ERROR
};

const char* const kPresetNames[THEME_PRESET_COUNT] = {
    "Dark", "Light", "Midnight", "Nord", "HighContrast",
};

// 度量与预设无关（所有主题共用同一套几何；圆角/滚动条是"美化"的一部分，
// 不分主题——浅色下同样的圆角同样成立）。圆角取 Fluent 档位：窗口/弹层/
// 卡片 8px，控制件 4px——Win11 的"大面板圆、小控件方"的层次感。
const float kMetrics[THEME_METRIC_COUNT] = {
    8.0f,   // PAD_X
    5.0f,   // PAD_Y
    22.0f,  // ROW_H
    4.0f,   // ITEM_SPACING
    18.0f,  // INDENT
    1.0f,   // BORDER_W
    8.0f,   // WINDOW_ROUNDING
    8.0f,   // CHILD_ROUNDING
    4.0f,   // FRAME_ROUNDING
    4.0f,   // GRAB_ROUNDING
    8.0f,   // POPUP_ROUNDING
    4.0f,   // TAB_ROUNDING
    12.0f,  // SCROLLBAR_SIZE
    8.0f,   // GRAB_MIN_SIZE
};

const uint32_t* anchors_of(int preset) {
    switch (preset) {
        case THEME_PRESET_LIGHT:        return kAnchorsLight;
        case THEME_PRESET_MIDNIGHT:     return kAnchorsMidnight;
        case THEME_PRESET_NORD:         return kAnchorsNord;
        case THEME_PRESET_HIGH_CONTRAST:return kAnchorsHighContrast;
        default:                        return kAnchorsDark;
    }
}

// ---- JSON 主题加载 ----
//
// 名字表把枚举翻译成主题文件里的 snake_case 名（anchors 用锚色槽位 = 颜色
// 表前 17 项；colors 覆盖派生槽位）。解析对类型不敏感：颜色不是字符串、
// 度量不是数字，一律跳过该项——坏主题文件最多"少了点效果"，不能让引擎崩。

struct TokenName { const char* name; int token; };

const TokenName kAnchorNames[] = {
    {"window_bg", THEME_COLOR_WINDOW_BG},
    {"child_bg", THEME_COLOR_CHILD_BG},
    {"border", THEME_COLOR_BORDER},
    {"text", THEME_COLOR_TEXT},
    {"text_dim", THEME_COLOR_TEXT_DIM},
    {"accent", THEME_COLOR_ACCENT},
    {"accent_hover", THEME_COLOR_ACCENT_HOVER},
    {"accent_active", THEME_COLOR_ACCENT_ACTIVE},
    {"button", THEME_COLOR_BUTTON},
    {"button_hover", THEME_COLOR_BUTTON_HOVER},
    {"button_active", THEME_COLOR_BUTTON_ACTIVE},
    {"header", THEME_COLOR_HEADER},
    {"header_hover", THEME_COLOR_HEADER_HOVER},
    {"selection", THEME_COLOR_SELECTION},
    {"ok", THEME_COLOR_OK},
    {"warn", THEME_COLOR_WARN},
    {"error", THEME_COLOR_ERROR},
};

const TokenName kDerivedNames[] = {
    {"popup_bg", THEME_COLOR_POPUP_BG},
    {"title_bg", THEME_COLOR_TITLE_BG},
    {"title_bg_active", THEME_COLOR_TITLE_BG_ACTIVE},
    {"title_bg_collapsed", THEME_COLOR_TITLE_BG_COLLAPSED},
    {"menubar_bg", THEME_COLOR_MENUBAR_BG},
    {"scrollbar_bg", THEME_COLOR_SCROLLBAR_BG},
    {"scrollbar_grab", THEME_COLOR_SCROLLBAR_GRAB},
    {"scrollbar_grab_hovered", THEME_COLOR_SCROLLBAR_GRAB_HOVERED},
    {"scrollbar_grab_active", THEME_COLOR_SCROLLBAR_GRAB_ACTIVE},
    {"separator", THEME_COLOR_SEPARATOR},
    {"separator_hovered", THEME_COLOR_SEPARATOR_HOVERED},
    {"separator_active", THEME_COLOR_SEPARATOR_ACTIVE},
    {"resize_grip", THEME_COLOR_RESIZE_GRIP},
    {"resize_grip_hovered", THEME_COLOR_RESIZE_GRIP_HOVERED},
    {"resize_grip_active", THEME_COLOR_RESIZE_GRIP_ACTIVE},
    {"check_mark", THEME_COLOR_CHECK_MARK},
    {"slider_grab", THEME_COLOR_SLIDER_GRAB},
    {"slider_grab_active", THEME_COLOR_SLIDER_GRAB_ACTIVE},
    {"header_active", THEME_COLOR_HEADER_ACTIVE},
    {"plot_lines", THEME_COLOR_PLOT_LINES},
    {"plot_lines_hovered", THEME_COLOR_PLOT_LINES_HOVERED},
    {"plot_histogram", THEME_COLOR_PLOT_HISTOGRAM},
    {"plot_histogram_hovered", THEME_COLOR_PLOT_HISTOGRAM_HOVERED},
    {"table_header_bg", THEME_COLOR_TABLE_HEADER_BG},
    {"table_border_strong", THEME_COLOR_TABLE_BORDER_STRONG},
    {"table_border_light", THEME_COLOR_TABLE_BORDER_LIGHT},
    {"table_row_bg", THEME_COLOR_TABLE_ROW_BG},
    {"table_row_bg_alt", THEME_COLOR_TABLE_ROW_BG_ALT},
    {"drag_drop_target", THEME_COLOR_DRAG_DROP_TARGET},
    {"nav_highlight", THEME_COLOR_NAV_HIGHLIGHT},
    {"nav_windowing_highlight", THEME_COLOR_NAV_WINDOWING_HIGHLIGHT},
    {"nav_windowing_dim_bg", THEME_COLOR_NAV_WINDOWING_DIM_BG},
    {"modal_window_dim_bg", THEME_COLOR_MODAL_WINDOW_DIM_BG},
    {"border_shadow", THEME_COLOR_BORDER_SHADOW},
    {"tab", THEME_COLOR_TAB},
    {"tab_hovered", THEME_COLOR_TAB_HOVERED},
    {"tab_selected", THEME_COLOR_TAB_SELECTED},
    {"tab_selected_overline", THEME_COLOR_TAB_SELECTED_OVERLINE},
    {"tab_dimmed", THEME_COLOR_TAB_DIMMED},
    {"tab_dimmed_selected", THEME_COLOR_TAB_DIMMED_SELECTED},
    {"control_border", THEME_COLOR_CONTROL_BORDER},
};

const TokenName kMetricNames[] = {
    {"pad_x", THEME_METRIC_PAD_X},
    {"pad_y", THEME_METRIC_PAD_Y},
    {"row_h", THEME_METRIC_ROW_H},
    {"item_spacing", THEME_METRIC_ITEM_SPACING},
    {"indent", THEME_METRIC_INDENT},
    {"border_w", THEME_METRIC_BORDER_W},
    {"window_rounding", THEME_METRIC_WINDOW_ROUNDING},
    {"child_rounding", THEME_METRIC_CHILD_ROUNDING},
    {"frame_rounding", THEME_METRIC_FRAME_ROUNDING},
    {"grab_rounding", THEME_METRIC_GRAB_ROUNDING},
    {"popup_rounding", THEME_METRIC_POPUP_ROUNDING},
    {"tab_rounding", THEME_METRIC_TAB_ROUNDING},
    {"scrollbar_size", THEME_METRIC_SCROLLBAR_SIZE},
    {"grab_min_size", THEME_METRIC_GRAB_MIN_SIZE},
};

// AnimEase（ui_anim.h）枚举序：Linear..EaseOutBounce。
const char* const kEaseNames[] = {
    "linear", "in_quad", "out_quad", "inout_quad", "in_cubic", "out_cubic",
    "inout_cubic", "out_back", "inout_back", "out_elastic", "out_bounce",
};
constexpr int kEaseNameCount = static_cast<int>(sizeof(kEaseNames) / sizeof(kEaseNames[0]));

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "#RRGGBB" / "#RRGGBBAA"（# 可省略；缺 alpha 按不透明）。其余写法一律 false。
bool parse_color(const std::string& s, uint32_t* out) {
    size_t i = (!s.empty() && s[0] == '#') ? 1 : 0;
    const size_t n = s.size() - i;
    if (n != 6 && n != 8) return false;
    uint32_t v = 0;
    for (; i < s.size(); ++i) {
        const int d = hex_digit(s[i]);
        if (d < 0) return false;
        v = (v << 4) | static_cast<uint32_t>(d);
    }
    if (n == 6) v = (v << 8) | 0xFFu;
    *out = v;
    return true;
}

int find_ease(const std::string& name) {
    for (int i = 0; i < kEaseNameCount; ++i) {
        if (name == kEaseNames[i]) return i;
    }
    return -1;
}

} // namespace

int load_theme_json(const char* json_text) {
    if (!json_text || !json_text[0]) return 0;
    const nlohmann::json j = nlohmann::json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return 0;

    Theme& t = mutable_theme();
    // 底：出厂深色锚色，保证"只写一部分"的主题文件也是良定义的
    const uint32_t* base = anchors_of(THEME_PRESET_DARK);
    for (int i = 0; i < THEME_ANCHOR_COUNT; ++i) t.colors[i] = base[i];

    auto apply_colors = [&j, &t](const char* block, const TokenName* names, int count) {
        const auto it = j.find(block);
        if (it == j.end() || !it->is_object()) return;
        for (int k = 0; k < count; ++k) {
            const auto c = it->find(names[k].name);
            if (c == it->end() || !c->is_string()) continue;
            uint32_t v = 0;
            if (parse_color(c->get<std::string>(), &v)) t.colors[names[k].token] = v;
        }
    };
    apply_colors("anchors", kAnchorNames, THEME_ANCHOR_COUNT);
    derive_extended(t);          // 先按 anchors 派生，再让 colors 覆盖派生结果
    apply_colors("colors", kDerivedNames, static_cast<int>(sizeof(kDerivedNames) / sizeof(kDerivedNames[0])));

    const auto m = j.find("metrics");
    if (m != j.end() && m->is_object()) {
        for (int k = 0; k < THEME_METRIC_COUNT; ++k) {
            const auto v = m->find(kMetricNames[k].name);
            if (v == m->end() || !v->is_number()) continue;
            t.metrics[kMetricNames[k].token] = v->get<float>();
        }
    }

    // style 块：质感参数（渐变幅度/投影）。缺省项保持当前值——与 metrics
    // 不同，这里允许"只改投影不改渐变"的部分覆盖。
    const auto s = j.find("style");
    if (s != j.end() && s->is_object()) {
        ThemeStyle st = t.style;
        if (const auto v = s->find("fill_grad"); v != s->end() && v->is_number()) {
            st.fill_grad = v->get<float>();
        }
        if (const auto v = s->find("panel_grad"); v != s->end() && v->is_number()) {
            st.panel_grad = v->get<float>();
        }
        if (const auto v = s->find("shadow_alpha"); v != s->end() && v->is_number()) {
            st.shadow_alpha = v->get<float>();
        }
        if (const auto v = s->find("shadow_spread"); v != s->end() && v->is_number()) {
            st.shadow_spread = v->get<float>();
        }
        // 渐变幅度出现负值等于把按钮画成凹坑，钳到 [0, 0.5]
        if (st.fill_grad < 0.0f) st.fill_grad = 0.0f;
        if (st.fill_grad > 0.5f) st.fill_grad = 0.5f;
        if (st.panel_grad < 0.0f) st.panel_grad = 0.0f;
        if (st.panel_grad > 0.5f) st.panel_grad = 0.5f;
        if (st.shadow_alpha < 0.0f) st.shadow_alpha = 0.0f;
        if (st.shadow_alpha > 1.0f) st.shadow_alpha = 1.0f;
        if (st.shadow_spread < 0.0f) st.shadow_spread = 0.0f;
        t.style = st;
    }

    // animation 块缺省时保持现状（换主题文件不该悄悄重置宿主调过的参数）
    const auto a = j.find("animation");
    if (a != j.end() && a->is_object()) {
        ThemeAnim anim = theme_anim();
        if (const auto v = a->find("enabled"); v != a->end() && v->is_boolean()) {
            anim.enabled = v->get<bool>();
        }
        if (const auto v = a->find("hover_ms"); v != a->end() && v->is_number()) {
            anim.hover_s = v->get<float>() * 0.001f;
        }
        if (const auto v = a->find("press_ms"); v != a->end() && v->is_number()) {
            anim.press_s = v->get<float>() * 0.001f;
        }
        if (const auto v = a->find("ease"); v != a->end() && v->is_string()) {
            const int e = find_ease(v->get<std::string>());
            if (e >= 0) anim.ease = e;
        }
        if (anim.hover_s < 0.0f) anim.hover_s = 0.0f;
        if (anim.press_s < 0.0f) anim.press_s = 0.0f;
        set_theme_anim(anim);
    }
    return 1;
}

int load_theme_file(const char* path) {
    if (!path || !path[0]) return 0;
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return load_theme_json(text.c_str());
}

namespace {

void derive_extended(Theme& t) {
    const uint32_t* a = t.colors;   // 前 17 项即锚色（顺序与枚举一致）

    t.colors[THEME_COLOR_POPUP_BG]              = a[THEME_ANCHOR_WINDOW_BG];
    t.colors[THEME_COLOR_TITLE_BG]              = a[THEME_ANCHOR_HEADER];
    t.colors[THEME_COLOR_TITLE_BG_ACTIVE]       = mix(a[THEME_ANCHOR_HEADER], a[THEME_ANCHOR_ACCENT], 0.15f);
    t.colors[THEME_COLOR_TITLE_BG_COLLAPSED]    = shade(a[THEME_ANCHOR_HEADER], 0.85f);
    t.colors[THEME_COLOR_MENUBAR_BG]            = a[THEME_ANCHOR_HEADER];
    t.colors[THEME_COLOR_SCROLLBAR_BG]          = mix(a[THEME_ANCHOR_WINDOW_BG], a[THEME_ANCHOR_CHILD_BG], 0.35f);
    t.colors[THEME_COLOR_SCROLLBAR_GRAB]        = mix(a[THEME_ANCHOR_BUTTON], a[THEME_ANCHOR_BORDER], 0.5f);
    t.colors[THEME_COLOR_SCROLLBAR_GRAB_HOVERED]= a[THEME_ANCHOR_BUTTON_HOVER];
    t.colors[THEME_COLOR_SCROLLBAR_GRAB_ACTIVE] = a[THEME_ANCHOR_BUTTON_ACTIVE];
    t.colors[THEME_COLOR_SEPARATOR]             = a[THEME_ANCHOR_BORDER];
    t.colors[THEME_COLOR_SEPARATOR_HOVERED]     = mix(a[THEME_ANCHOR_BORDER], a[THEME_ANCHOR_ACCENT], 0.5f);
    t.colors[THEME_COLOR_SEPARATOR_ACTIVE]      = a[THEME_ANCHOR_ACCENT];
    t.colors[THEME_COLOR_RESIZE_GRIP]           = mix(a[THEME_ANCHOR_BUTTON], a[THEME_ANCHOR_TEXT_DIM], 0.30f);
    t.colors[THEME_COLOR_RESIZE_GRIP_HOVERED]   = mix(a[THEME_ANCHOR_BUTTON], a[THEME_ANCHOR_ACCENT], 0.50f);
    t.colors[THEME_COLOR_RESIZE_GRIP_ACTIVE]    = a[THEME_ANCHOR_ACCENT];
    t.colors[THEME_COLOR_CHECK_MARK]            = a[THEME_ANCHOR_ACCENT];
    t.colors[THEME_COLOR_SLIDER_GRAB]           = a[THEME_ANCHOR_ACCENT];
    t.colors[THEME_COLOR_SLIDER_GRAB_ACTIVE]    = a[THEME_ANCHOR_ACCENT_ACTIVE];
    t.colors[THEME_COLOR_HEADER_ACTIVE]         = mix(a[THEME_ANCHOR_HEADER_HOVER], a[THEME_ANCHOR_ACCENT], 0.35f);
    t.colors[THEME_COLOR_PLOT_LINES]            = a[THEME_ANCHOR_ACCENT];
    t.colors[THEME_COLOR_PLOT_LINES_HOVERED]    = a[THEME_ANCHOR_ACCENT_HOVER];
    t.colors[THEME_COLOR_PLOT_HISTOGRAM]        = mix(a[THEME_ANCHOR_ACCENT], a[THEME_ANCHOR_TEXT], 0.25f);
    t.colors[THEME_COLOR_PLOT_HISTOGRAM_HOVERED]= a[THEME_ANCHOR_ACCENT_HOVER];
    t.colors[THEME_COLOR_TABLE_HEADER_BG]       = a[THEME_ANCHOR_HEADER];
    t.colors[THEME_COLOR_TABLE_BORDER_STRONG]   = a[THEME_ANCHOR_BORDER];
    t.colors[THEME_COLOR_TABLE_BORDER_LIGHT]    = mix(a[THEME_ANCHOR_BORDER], a[THEME_ANCHOR_WINDOW_BG], 0.5f);
    t.colors[THEME_COLOR_TABLE_ROW_BG]          = with_alpha(a[THEME_ANCHOR_WINDOW_BG], 0x00);
    t.colors[THEME_COLOR_TABLE_ROW_BG_ALT]      = with_alpha(mix(a[THEME_ANCHOR_WINDOW_BG], a[THEME_ANCHOR_CHILD_BG], 0.5f), 0x60);
    t.colors[THEME_COLOR_DRAG_DROP_TARGET]      = a[THEME_ANCHOR_ACCENT_HOVER];
    t.colors[THEME_COLOR_NAV_HIGHLIGHT]         = a[THEME_ANCHOR_ACCENT];
    t.colors[THEME_COLOR_NAV_WINDOWING_HIGHLIGHT] = with_alpha(mix(a[THEME_ANCHOR_TEXT], a[THEME_ANCHOR_WINDOW_BG], 0.7f), 0xB0);
    t.colors[THEME_COLOR_NAV_WINDOWING_DIM_BG]  = with_alpha(a[THEME_ANCHOR_WINDOW_BG], 0xCC);
    t.colors[THEME_COLOR_MODAL_WINDOW_DIM_BG]   = with_alpha(a[THEME_ANCHOR_WINDOW_BG], 0xCC);
    t.colors[THEME_COLOR_BORDER_SHADOW]         = 0x00000000u;
    t.colors[THEME_COLOR_TAB]                   = mix(a[THEME_ANCHOR_BUTTON], a[THEME_ANCHOR_WINDOW_BG], 0.4f);
    t.colors[THEME_COLOR_TAB_HOVERED]           = a[THEME_ANCHOR_BUTTON_HOVER];
    t.colors[THEME_COLOR_TAB_SELECTED]          = mix(a[THEME_ANCHOR_ACCENT], a[THEME_ANCHOR_WINDOW_BG], 0.6f);
    t.colors[THEME_COLOR_TAB_SELECTED_OVERLINE] = a[THEME_ANCHOR_ACCENT];
    t.colors[THEME_COLOR_TAB_DIMMED]            = mix(a[THEME_ANCHOR_BUTTON], a[THEME_ANCHOR_WINDOW_BG], 0.7f);
    t.colors[THEME_COLOR_TAB_DIMMED_SELECTED]   = mix(a[THEME_ANCHOR_ACCENT], a[THEME_ANCHOR_WINDOW_BG], 0.85f);
    // 控件描边：取 BUTTON 与 BORDER 的中点——正好"看得出边界、又不显成线框"。
    // 直接拿 BORDER 描边时，每个控件都顶着一圈比自身底色亮两档的细线，成片的
    // 控件就变成一张表格；这个 token 是让控件靠"底色 + 微弱边缘"立住。
    t.colors[THEME_COLOR_CONTROL_BORDER]        = mix(a[THEME_ANCHOR_BUTTON], a[THEME_ANCHOR_BORDER], 0.5f);
}

Theme& mutable_theme() {
    static Theme t = [] {
        Theme d{};
        const uint32_t* anchors = anchors_of(THEME_PRESET_DARK);
        for (int i = 0; i < THEME_ANCHOR_COUNT; ++i) d.colors[i] = anchors[i];
        for (int i = 0; i < THEME_METRIC_COUNT; ++i) d.metrics[i] = kMetrics[i];
        derive_extended(d);
        return d;
    }();
    return t;
}

} // namespace

const char* theme_preset_name(int preset) {
    if (preset < 0 || preset >= THEME_PRESET_COUNT) return "";
    return kPresetNames[preset];
}

const Theme& theme() { return mutable_theme(); }

void set_preset(int preset) {
    Theme& t = mutable_theme();
    const uint32_t* anchors = anchors_of(preset);
    for (int i = 0; i < THEME_ANCHOR_COUNT; ++i) t.colors[i] = anchors[i];
    // 整体重填：既刷新派生 token，也清掉上一套预设期间的个别覆盖
    // （覆盖不跨预设存活——否则换预设换不干净）。质感参数同理：
    // 出厂预设不带 style 信息，恢复编译器默认值。
    t.style = ThemeStyle{};
    derive_extended(t);
}

void set_color(int token, uint32_t rgba) {
    if (token < 0 || token >= THEME_COLOR_COUNT) return;
    mutable_theme().colors[token] = rgba;
}

uint32_t color(int token) {
    if (token < 0 || token >= THEME_COLOR_COUNT) return 0;
    return theme().colors[token];
}

void set_metric(int token, float value) {
    if (token < 0 || token >= THEME_METRIC_COUNT) return;
    mutable_theme().metrics[token] = value;
}

float metric(int token) {
    if (token < 0 || token >= THEME_METRIC_COUNT) return 0.0f;
    return theme().metrics[token];
}

const ThemeAnim& theme_anim() {
    static ThemeAnim a{};
    return a;
}

void set_theme_anim(const ThemeAnim& a) {
    const_cast<ThemeAnim&>(theme_anim()) = a;
}

const ThemeStyle& theme_style() {
    return theme().style;
}

void set_theme_style(const ThemeStyle& s) {
    mutable_theme().style = s;
}

void apply() {
#ifdef GRYCE_HAS_IMGUI
    // 无上下文时安全返回：apply 是宿主面的 API（GRender_Init 前后都能调），
    // 而 ImGui 的 GetStyle() 在没上下文时是空指针解引用。渲染器没接管
    // ImGui（--no-debug-ui 且无 UI 回调）时就会走到这里。
    if (ImGui::GetCurrentContext() == nullptr) return;
    const Theme& t = theme();
    ImGuiStyle& s = ImGui::GetStyle();
    auto col = [&](int token) {
        const uint32_t c = t.colors[token];
        return ImVec4(static_cast<float>((c >> 24) & 0xFF) / 255.0f,
                      static_cast<float>((c >> 16) & 0xFF) / 255.0f,
                      static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                      static_cast<float>((c & 0xFF) / 255.0f));
    };

    // 度量：内边距/间距/缩进 + 圆角与滚动条（美化项）
    s.WindowPadding    = ImVec2(t.metrics[THEME_METRIC_PAD_X], t.metrics[THEME_METRIC_PAD_Y]);
    s.FramePadding     = ImVec2(t.metrics[THEME_METRIC_PAD_X], t.metrics[THEME_METRIC_PAD_Y] - 1.0f);
    s.ItemSpacing      = ImVec2(t.metrics[THEME_METRIC_PAD_X], t.metrics[THEME_METRIC_ITEM_SPACING]);
    s.IndentSpacing    = t.metrics[THEME_METRIC_INDENT];
    s.ChildBorderSize  = t.metrics[THEME_METRIC_BORDER_W];
    s.FrameBorderSize  = t.metrics[THEME_METRIC_BORDER_W];
    s.WindowBorderSize = t.metrics[THEME_METRIC_BORDER_W];
    s.WindowRounding   = t.metrics[THEME_METRIC_WINDOW_ROUNDING];
    s.ChildRounding    = t.metrics[THEME_METRIC_CHILD_ROUNDING];
    s.FrameRounding    = t.metrics[THEME_METRIC_FRAME_ROUNDING];
    s.GrabRounding     = t.metrics[THEME_METRIC_GRAB_ROUNDING];
    s.PopupRounding    = t.metrics[THEME_METRIC_POPUP_ROUNDING];
    s.TabRounding      = t.metrics[THEME_METRIC_TAB_ROUNDING];
    s.ScrollbarSize    = t.metrics[THEME_METRIC_SCROLLBAR_SIZE];
    s.GrabMinSize      = t.metrics[THEME_METRIC_GRAB_MIN_SIZE];

    s.Colors[ImGuiCol_WindowBg]             = col(THEME_COLOR_WINDOW_BG);
    s.Colors[ImGuiCol_ChildBg]              = col(THEME_COLOR_CHILD_BG);
    s.Colors[ImGuiCol_PopupBg]              = col(THEME_COLOR_POPUP_BG);
    s.Colors[ImGuiCol_Border]               = col(THEME_COLOR_BORDER);
    s.Colors[ImGuiCol_BorderShadow]         = col(THEME_COLOR_BORDER_SHADOW);
    s.Colors[ImGuiCol_Text]                 = col(THEME_COLOR_TEXT);
    s.Colors[ImGuiCol_TextDisabled]         = col(THEME_COLOR_TEXT_DIM);
    s.Colors[ImGuiCol_TextSelectedBg]       = col(THEME_COLOR_SELECTION);
    s.Colors[ImGuiCol_TitleBg]              = col(THEME_COLOR_TITLE_BG);
    s.Colors[ImGuiCol_TitleBgActive]        = col(THEME_COLOR_TITLE_BG_ACTIVE);
    s.Colors[ImGuiCol_TitleBgCollapsed]     = col(THEME_COLOR_TITLE_BG_COLLAPSED);
    s.Colors[ImGuiCol_MenuBarBg]            = col(THEME_COLOR_MENUBAR_BG);
    s.Colors[ImGuiCol_ScrollbarBg]          = col(THEME_COLOR_SCROLLBAR_BG);
    s.Colors[ImGuiCol_ScrollbarGrab]        = col(THEME_COLOR_SCROLLBAR_GRAB);
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = col(THEME_COLOR_SCROLLBAR_GRAB_HOVERED);
    s.Colors[ImGuiCol_ScrollbarGrabActive]  = col(THEME_COLOR_SCROLLBAR_GRAB_ACTIVE);
    s.Colors[ImGuiCol_Separator]            = col(THEME_COLOR_SEPARATOR);
    s.Colors[ImGuiCol_SeparatorHovered]     = col(THEME_COLOR_SEPARATOR_HOVERED);
    s.Colors[ImGuiCol_SeparatorActive]      = col(THEME_COLOR_SEPARATOR_ACTIVE);
    s.Colors[ImGuiCol_FrameBg]              = col(THEME_COLOR_BUTTON);
    s.Colors[ImGuiCol_FrameBgHovered]       = col(THEME_COLOR_BUTTON_HOVER);
    s.Colors[ImGuiCol_FrameBgActive]        = col(THEME_COLOR_BUTTON_ACTIVE);
    s.Colors[ImGuiCol_Button]               = col(THEME_COLOR_BUTTON);
    s.Colors[ImGuiCol_ButtonHovered]        = col(THEME_COLOR_BUTTON_HOVER);
    s.Colors[ImGuiCol_ButtonActive]         = col(THEME_COLOR_BUTTON_ACTIVE);
    s.Colors[ImGuiCol_CheckMark]            = col(THEME_COLOR_CHECK_MARK);
    s.Colors[ImGuiCol_SliderGrab]           = col(THEME_COLOR_SLIDER_GRAB);
    s.Colors[ImGuiCol_SliderGrabActive]     = col(THEME_COLOR_SLIDER_GRAB_ACTIVE);
    s.Colors[ImGuiCol_Header]               = col(THEME_COLOR_HEADER);
    s.Colors[ImGuiCol_HeaderHovered]        = col(THEME_COLOR_HEADER_HOVER);
    s.Colors[ImGuiCol_HeaderActive]         = col(THEME_COLOR_HEADER_ACTIVE);
    s.Colors[ImGuiCol_ResizeGrip]           = col(THEME_COLOR_RESIZE_GRIP);
    s.Colors[ImGuiCol_ResizeGripHovered]    = col(THEME_COLOR_RESIZE_GRIP_HOVERED);
    s.Colors[ImGuiCol_ResizeGripActive]     = col(THEME_COLOR_RESIZE_GRIP_ACTIVE);
    s.Colors[ImGuiCol_PlotLines]            = col(THEME_COLOR_PLOT_LINES);
    s.Colors[ImGuiCol_PlotLinesHovered]     = col(THEME_COLOR_PLOT_LINES_HOVERED);
    s.Colors[ImGuiCol_PlotHistogram]        = col(THEME_COLOR_PLOT_HISTOGRAM);
    s.Colors[ImGuiCol_PlotHistogramHovered] = col(THEME_COLOR_PLOT_HISTOGRAM_HOVERED);
    s.Colors[ImGuiCol_TableHeaderBg]        = col(THEME_COLOR_TABLE_HEADER_BG);
    s.Colors[ImGuiCol_TableBorderStrong]    = col(THEME_COLOR_TABLE_BORDER_STRONG);
    s.Colors[ImGuiCol_TableBorderLight]     = col(THEME_COLOR_TABLE_BORDER_LIGHT);
    s.Colors[ImGuiCol_TableRowBg]           = col(THEME_COLOR_TABLE_ROW_BG);
    s.Colors[ImGuiCol_TableRowBgAlt]        = col(THEME_COLOR_TABLE_ROW_BG_ALT);
    s.Colors[ImGuiCol_DragDropTarget]       = col(THEME_COLOR_DRAG_DROP_TARGET);
    s.Colors[ImGuiCol_NavHighlight]         = col(THEME_COLOR_NAV_HIGHLIGHT);
    s.Colors[ImGuiCol_NavWindowingHighlight]= col(THEME_COLOR_NAV_WINDOWING_HIGHLIGHT);
    s.Colors[ImGuiCol_NavWindowingDimBg]    = col(THEME_COLOR_NAV_WINDOWING_DIM_BG);
    s.Colors[ImGuiCol_ModalWindowDimBg]     = col(THEME_COLOR_MODAL_WINDOW_DIM_BG);
    s.Colors[ImGuiCol_Tab]                  = col(THEME_COLOR_TAB);
    s.Colors[ImGuiCol_TabHovered]           = col(THEME_COLOR_TAB_HOVERED);
    s.Colors[ImGuiCol_TabSelected]          = col(THEME_COLOR_TAB_SELECTED);
    s.Colors[ImGuiCol_TabSelectedOverline]  = col(THEME_COLOR_TAB_SELECTED_OVERLINE);
    s.Colors[ImGuiCol_TabDimmed]            = col(THEME_COLOR_TAB_DIMMED);
    s.Colors[ImGuiCol_TabDimmedSelected]    = col(THEME_COLOR_TAB_DIMMED_SELECTED);
#endif
}

} // namespace gryce_ui
