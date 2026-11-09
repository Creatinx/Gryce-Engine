// renderer_smoke.cpp — Gryce 渲染器双后端冒烟 / 演示程序
//
// 用法:
//   GryceRenderSmoke.exe [--api opengl|vulkan|dx12] [--project <dir>] [--scene <res:/...>]
//                        [--frames N] [--shot <png>] [--w N] [--h N]
//                        [--sync] [--no-analyze] [--list]
//                        [--play] [--speed <m/s>] [--sensitivity <deg/px>]
//
// 行为与参考工程的 GryceGame 宿主一致：
//   1. 以 <project> 为项目根初始化 Core（res:/ 由此解析），载入主场景；
//   2. 读取 <project>/project.data 的运行时设置（HDR / 色调映射 / 曝光 / 阴影 /
//      环境光 / IBL），应用到渲染器，保证与参考工程样的画面配方；
//   3. 驱动 N 帧（渲染 3D 世界 + 2D 覆盖层），随后请求截图并多驱动几帧让
//      渲染线程真正写盘；
//   4. 用 stb_image 回读截图做统计，画面若是"纯清屏色"则判定渲染链路未生效。
//
// --play：进入交互模式 —— 鼠标锁定（光标隐藏、锁定在窗口内）用鼠标转视角，
//         WASD 前后左右移动、空格/左Ctrl 升降、Shift 加速、Esc 退出。
//         按一下 Alt 切换"锁定/解锁"（解锁后指针可见、视角冻结，可去点菜单或
//         别的窗口；再按一下 Alt 恢复锁定），松开 Alt 不会自动恢复。
//         交互模式下 --frames 表示"第 N 帧截一次图"，不退出。
//
// 退出码: 0 = 正常；1 = 初始化失败；2 = 渲染结果为空（着色器/管线未生效）。 
#include "api/render_api.h"
#include "api/core_api.h"
#include "api/input_api.h"
#include "api/key_codes.h"
#include "api/scene_api.h"
#include "api/edit_api.h"   // --editor-preview 的选中/Gizmo 预览路径
#include "api/entity_api.h"
#include "api/component_api.h"
#include "api/viewport_api.h"
#include "platform/window.h"
#include "components/ui_root.h"
#include "ecs/world.h"
#include "scene/scene.h"
#include "ui/ui_widgets.h"
#include "ui/ui_editor_widgets.h"
#include "ui/ui_extended_widgets.h"
#include "ui/ui_theme.h"
#include "ui/ui_texture.h"
#include "render/font_atlas.h"
#include "render/render2d.h"
#include "assets/texture_data.h"
#include "resources/resource_path.h"

#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

using gryce_engine::platform::Window;
using gryce_engine::platform::WindowMode;
using gryce_engine::platform::WindowContextType;

namespace gryce_engine::components {
class UIRoot;
}

namespace {

bool inject_gryce_ui_demo() {
    auto* world = static_cast<gryce_engine::ecs::World*>(GCore_GetInternalWorldPtr());
    if (!world || !world->scene()) return false;
    auto* entity = world->scene()->create_entity("GryceUI Demo");
    if (!entity) return false;

    auto* root = entity->add_component<gryce_engine::components::UIRoot>();
    auto* panel = entity->add_component<gryce_engine::components::UIPanel>();
    auto* title = entity->add_component<gryce_engine::components::UILabel>();
    auto* button = entity->add_component<gryce_engine::components::UIButton>();
    auto* check = entity->add_component<gryce_engine::components::UICheckBox>();
    auto* slider = entity->add_component<gryce_engine::components::UISlider>();
    auto* input = entity->add_component<gryce_engine::components::UITextInput>();
    auto* scroll = entity->add_component<gryce_engine::components::UIScrollView>();

    panel->set_local_position(gryce_engine::math::Vector3f(24.0f, 60.0f, 0.0f));
    panel->size = gryce_engine::math::Vector2f(420.0f, 390.0f);
    panel->min_size = panel->size;
    panel->padding = 14.0f;
    panel->gap = 10.0f;
    panel->layout_mode = gryce_engine::components::UILayoutMode::Column;
    panel->color = gryce_engine::math::Vector4f(0.08f, 0.10f, 0.14f, 0.96f);

    title->text = "GryceUI Phase 2";
    title->font_size = 22.0f;
    title->size = gryce_engine::math::Vector2f(0.0f, 32.0f);
    button->text = "Apply";
    button->size = gryce_engine::math::Vector2f(160.0f, 34.0f);
    check->text = "Draw debug bounds";
    check->size = gryce_engine::math::Vector2f(220.0f, 26.0f);
    slider->value = 0.65f;
    slider->size = gryce_engine::math::Vector2f(300.0f, 26.0f);
    input->text = "res:/scenes/main.gesc";
    input->size = gryce_engine::math::Vector2f(340.0f, 30.0f);
    scroll->size = gryce_engine::math::Vector2f(360.0f, 90.0f);
    scroll->content_height = 180.0f;

    entity->set_component_parent(panel, root);
    entity->set_component_parent(title, panel);
    entity->set_component_parent(button, panel);
    entity->set_component_parent(check, panel);
    entity->set_component_parent(slider, panel);
    entity->set_component_parent(input, panel);
    entity->set_component_parent(scroll, panel);
    return true;
}

// ---------------------------------------------------------------------------
// GryceUI 组件画廊：展示所有内置控件 + 多主题切换（5 个预设 + 实时 JSON 编辑）。
// 用 --ui-gallery 启动；--theme <dark|light|highcontrast|nord|fluent> 选初始主题。
// 运行时点主题按钮切预设，点 "Apply JSON" 用编辑器里的 JSON 实时覆盖主题。
// ---------------------------------------------------------------------------
struct GalleryHandles {
    gryce_engine::components::UIRoot*   root      = nullptr;
    gryce_engine::components::UILabel*  theme_label = nullptr;
    gryce_engine::components::UIButton* b_dark  = nullptr;
    gryce_engine::components::UIButton* b_light = nullptr;
    gryce_engine::components::UIButton* b_high  = nullptr;
    gryce_engine::components::UIButton* b_nord  = nullptr;
    gryce_engine::components::UIButton* b_fluent = nullptr;
    gryce_engine::components::UICodeEditor* theme_json = nullptr;
    gryce_engine::components::UIButton* b_apply = nullptr;
    gryce_engine::components::UIMenuBar* menu_bar = nullptr;
    gryce_engine::components::UIMenuFlyout* menu_flyout = nullptr;
    std::string current_theme = "Dark";
};
static GalleryHandles g_gallery;

static gryce_engine::ui::UIThemePreset parse_theme(const std::string& s) {
    if (s == "light")          return gryce_engine::ui::UIThemePreset::Light;
    if (s == "highcontrast" || s == "high-contrast" || s == "hc")
        return gryce_engine::ui::UIThemePreset::HighContrast;
    if (s == "nord")           return gryce_engine::ui::UIThemePreset::Nord;
    if (s == "fluent")         return gryce_engine::ui::UIThemePreset::Fluent;
    return gryce_engine::ui::UIThemePreset::Dark;
}

// 平台输入 -> 引擎输入层。
//
// smoke 自己建 GLFW 窗口（不走 GWindow_*），引擎"每帧从平台窗口现查输入"的
// 那条路就是空的：界面收不到指针与按键，鼠标点按钮、滚轮翻页全都没反应。
// 这里把窗口状态逐帧灌进输入层。
//
// GInput_BeginHostFrame 必须先调：它把上帧状态快照成"上一帧"，之后注入的新
// 状态才会产生正确的 press / release 边沿。少了它，按下的那一帧每帧都被认成
// "新按下"（点变成连点），松开事件则永远不产生。
static void pump_input(gryce_engine::platform::Window& window) {
    GInput_BeginHostFrame();

    double cursor_x = 0.0;
    double cursor_y = 0.0;
    window.get_cursor_pos(cursor_x, cursor_y);
    const float px = static_cast<float>(cursor_x);
    const float py = static_cast<float>(cursor_y);
    GInput_InjectMouseMove(px, py);
    for (int button = 0; button < 3; ++button) {
        GInput_InjectMouseButton(button,
                                 window.get_mouse_button(button) ? GINPUT_ACTION_PRESS
                                                                 : GINPUT_ACTION_RELEASE,
                                 px, py);
    }

    double scroll_x = 0.0;
    double scroll_y = 0.0;
    if (window.take_scroll(scroll_x, scroll_y)) {
        GInput_InjectMouseScroll(static_cast<float>(scroll_x),
                                 static_cast<float>(scroll_y));
    }

    unsigned int chars[64] = {};
    const int char_count = window.pull_chars(chars, 64);
    for (int i = 0; i < char_count; ++i) GInput_InjectChar(chars[i]);

    // 只转发输入层会读取的那批键（键码与 GLFW 一致，见 api/key_codes.h）。
    static constexpr int kPumpedKeys[] = {
        GKEY_ENTER, GKEY_ESCAPE, GKEY_TAB, GKEY_BACKSPACE, GKEY_DELETE,
        GKEY_LEFT, GKEY_RIGHT, GKEY_UP, GKEY_DOWN, GKEY_HOME, GKEY_END,
        GKEY_A, GKEY_C, GKEY_V, GKEY_X,
        GKEY_F1, GKEY_F2, GKEY_F3, GKEY_F4, GKEY_F5, GKEY_F6,
        GKEY_F7, GKEY_F8, GKEY_F9, GKEY_F10, GKEY_F11, GKEY_F12,
        GKEY_LEFT_CTRL, GKEY_LEFT_SHIFT, GKEY_LEFT_ALT,
    };
    for (int key : kPumpedKeys) {
        GInput_InjectKey(key, window.get_key(key) ? GINPUT_ACTION_PRESS
                                                  : GINPUT_ACTION_RELEASE);
    }
}

static void gallery_poll() {
    auto set_theme = [](gryce_engine::components::UIButton* b,
                       gryce_engine::ui::UIThemePreset p, const char* name) {
        if (b && b->clicked) {
            b->clicked = false;
            gryce_engine::ui::set_ui_theme_preset(p);
            g_gallery.current_theme = name;
        }
    };
    set_theme(g_gallery.b_dark,  gryce_engine::ui::UIThemePreset::Dark,          "Dark");
    set_theme(g_gallery.b_light, gryce_engine::ui::UIThemePreset::Light,         "Light");
    set_theme(g_gallery.b_high,  gryce_engine::ui::UIThemePreset::HighContrast,  "High Contrast");
    set_theme(g_gallery.b_nord,  gryce_engine::ui::UIThemePreset::Nord,          "Nord");
    set_theme(g_gallery.b_fluent, gryce_engine::ui::UIThemePreset::Fluent,       "Fluent");
    if (g_gallery.b_apply && g_gallery.b_apply->clicked) {
        g_gallery.b_apply->clicked = false;
        if (g_gallery.theme_json)
            gryce_engine::ui::load_ui_theme_json(g_gallery.theme_json->text.c_str());
    }
    if (g_gallery.theme_label) {
        g_gallery.theme_label->text =
            "Theme: " + g_gallery.current_theme +
            "    (buttons switch preset  |  Apply JSON live-edits theme)";
    }
}

// UI 贴图服务：把文件解码成 TextureData，再交给渲染器建 GPU 纹理。注册一次
// 之后控件只写 texture_path 就能显示贴图；按路径的缓存由 UI 层负责，这里只在
// 首次遇到某个路径时被调用一次。
static bool ui_texture_from_file(const char* path,
                                 gryce_engine::ui::TextureRef* out) {
    if (!path || !*path || !out) return false;
    auto* renderer = static_cast<gryce_engine::render::IRenderer2D*>(
        GRender_GetRenderer2D());
    if (!renderer) return false;
    const std::string resolved = gryce_engine::resources::ResourcePath::resolve(path);
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(resolved.c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        std::fprintf(stderr, "[smoke] ui texture not found: %s (%s)\n",
                     path, resolved.c_str());
        return false;
    }
    gryce_engine::assets::TextureData data;
    data.width = width;
    data.height = height;
    data.channels = 4;
    data.pixels.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    const gryce_engine::render::RHITextureHandle handle =
        renderer->create_texture_from_data(&data);
    if (!handle.is_valid()) return false;
    out->index = handle.index;
    out->generation = handle.generation;
    out->width = width;
    out->height = height;
    return true;
}

static bool inject_gryce_ui_gallery(const std::string& theme_arg, bool fit_all, int scroll_y,
                                    bool menu_open_demo) {
    using namespace gryce_engine::components;
    using namespace gryce_engine::ui;
    auto* world = static_cast<gryce_engine::ecs::World*>(GCore_GetInternalWorldPtr());
    if (!world || !world->scene()) return false;
    auto* entity = world->scene()->create_entity("GryceUI Gallery");
    if (!entity) return false;

    g_gallery = GalleryHandles{};
    g_gallery.current_theme = (theme_arg == "light") ? "Light" :
        (theme_arg == "highcontrast") ? "High Contrast" :
        (theme_arg == "nord") ? "Nord" :
        (theme_arg == "fluent") ? "Fluent" : "Dark";
    set_ui_theme_preset(parse_theme(theme_arg));

    // 贴图预热：注入期（未持渲染器锁）把用到的路径全部加载成纹理句柄，绘制期
    // 只查缓存。若留到绘制期懒加载，加载回调会去取渲染器 → 同一把锁 → 死锁。
    for (const char* tex_path : {"res:/ui/demo_photo.png",
                                 "res:/ui/icon_folder.png",
                                 "res:/ui/icon_save.png",
                                 "res:/ui/icon_undo.png",
                                 "res:/ui/icon_redo.png",
                                 "res:/ui/icon_play.png",
                                 "res:/ui/icon_search.png",
                                 "res:/ui/icon_settings.png",
                                 "res:/ui/icon_close.png",
                                 "res:/ui/icon_check.png"}) {
        gryce_engine::ui::ui_texture(tex_path);
    }

    auto* root = entity->add_component<UIRoot>();
    g_gallery.root = root;
    auto* main = entity->add_component<UIPanel>();
    auto place = [&](NodeUI* w, NodeUI* parent, float ww, float hh) {
        w->size = gryce_engine::math::Vector2f(ww, hh);
        w->min_size = gryce_engine::math::Vector2f(ww, hh);
        entity->set_component_parent(w, parent);
    };

    // 分区高度集中声明：section() 用它们排布，滚动区内容高度与整页高度都从同一
    // 组常量推导。以前散在三处的魔数各改一次，加一个分区就会漏改、出现重叠或
    // 空档，症状像"布局没对齐"。
    const float h_surfaces = 300.0f;
    const float h_images   = 330.0f;
    const float h_buttons  = 262.0f;
    const float h_inputs   = 350.0f;
    const float h_nav      = 168.0f;
    const float h_shell    = 520.0f;
    const float h_lists    = 1150.0f;
    const float h_editors  = 1210.0f;
    const float h_popups   = 1200.0f;
    const float sections_total = h_surfaces + h_images + h_buttons + h_inputs +
                                 h_nav + h_shell + h_lists + h_editors + h_popups;
    // 滚动区内容比可见区域高 600，--gallery-scroll 才有翻页余量。
    const float scroll_content_h = sections_total + 150.0f;

    // 面板按 1440 设计宽度排版，但横向锚在父容器中点：窗口被拉宽时整块居中，
    // 不会只剩左边一列、右边一大片空背景（拖窗口时最扎眼的就是这个）。
    main->anchor_min = gryce_engine::math::Vector2f(0.5f, 0.0f);
    main->anchor_max = gryce_engine::math::Vector2f(0.5f, 0.0f);
    main->set_local_position(gryce_engine::math::Vector3f(-720.0f, 24.0f, 0.0f));
    // fit_all：把滚动区拉到约 1320，填满无头帧缓冲（引擎把帧缓冲高度钳到 ~1570），
    // 配合 --gallery-scroll 翻页截全部分区；交互模式保持 720 高、可滚动。
    // 466 = 标题栏/菜单栏/命令栏/标题/说明/主题行/JSON 行(368) + 7 个 10px 行距
    // + 上下 padding 28。
    const float main_h = fit_all ? (466.0f + scroll_content_h - 600.0f) : 1080.0f;
    main->size = gryce_engine::math::Vector2f(1440.0f, main_h);
    main->min_size = main->size;
    main->padding = 14.0f; main->gap = 10.0f;
    main->layout_mode = UILayoutMode::Column;
    entity->set_component_parent(main, root);

    // 窗口框架：标题栏 + 命令栏（图标全部走自定义贴图，不依赖字体里有没有
    // 对应字形）。菜单栏在下面的"Navigation"分区里，展开后靠 draws_on_top
    // 压在其它内容之上。
    auto* titlebar = entity->add_component<UITitleBar>();
    titlebar->title = "Gryce Engine";
    titlebar->subtitle = "scenes/main.gesc";
    place(titlebar, main, 1412, 40);

    // 菜单栏放在标题栏与命令栏之间，和 WinUI 3 的窗口结构一致：标题栏 → 菜单 →
    // 命令栏 → 内容。浮层是菜单栏的子节点，命中测试按节点矩形走，画在父 rect
    // 之外也能收到点击；UIPopup 的 draws_on_top 让它压在后面的卡片之上。
    auto* mb = entity->add_component<UIMenuBar>();
    mb->menus = {
        {"File", {{"New Scene", "Ctrl+N", 1},
                  {"Open...", "Ctrl+O", 2},
                  {"Save", "Ctrl+S", 3},
                  {"", "", -1, true},
                  {"Exit", "Alt+F4", 9}}},
        {"Edit", {{"Undo", "Ctrl+Z", 11},
                  {"Redo", "Ctrl+Y", 12},
                  {"", "", -1, true},
                  {"Cut", "Ctrl+X", 13},
                  {"Copy", "Ctrl+C", 14},
                  {"Paste", "Ctrl+V", 15}}},
        {"View", {{"Show Grid", "", 21, false, true},
                  {"Show Gizmos", "", 22, false, true},
                  {"", "", -1, true},
                  {"Fullscreen", "F11", 23}}},
        {"Help", {{"Documentation", "", 31},
                  {"About Gryce", "", 32, false, false, true, true}}},
    };
    place(mb, main, 1412, 32);
    auto* mb_fly = entity->add_component<UIMenuFlyout>();
    entity->set_component_parent(mb_fly, mb);
    mb->flyout = mb_fly;
    mb_fly->set_entries(mb->menus[0].items);
    mb_fly->place_at(0.0f, 34.0f);
    g_gallery.menu_bar = mb;
    g_gallery.menu_flyout = mb_fly;
    if (menu_open_demo) {
        mb_fly->set_entries(mb->menus[0].items);
        mb->selected = 0;
        mb->open_index = 0;
        mb->open = true;
        mb_fly->place_at(0.0f, 34.0f);
        mb_fly->set_open(true);
    }

    auto* cmdbar = entity->add_component<UICommandBar>();
    place(cmdbar, main, 1412, 48);
    auto mkcmd = [&](const char* label, const char* icon) -> UIIconButton* {
        auto* b = entity->add_component<UIIconButton>();
        b->text = label;
        b->icon_path = icon;
        b->icon_size = 16.0f;
        // 宽度 = 图标 + 间距 + 文字 + 左右内边距
        const float w = 16.0f + 8.0f + ui_estimate_text_width(label, 14.0f) + 26.0f;
        place(b, cmdbar, std::max(40.0f, w), 32);
        return b;
    };
    mkcmd("Open", "res:/ui/icon_folder.png");
    mkcmd("Save", "res:/ui/icon_save.png");
    mkcmd("Undo", "res:/ui/icon_undo.png");
    mkcmd("Redo", "res:/ui/icon_redo.png");
    auto* cmd_sep = entity->add_component<UISeparator>();
    place(cmd_sep, cmdbar, 9, 32);
    mkcmd("Play", "res:/ui/icon_play.png");
    {
        auto* find = entity->add_component<UIIconButton>();
        find->icon_path = "res:/ui/icon_search.png";
        find->icon_size = 18.0f;
        place(find, cmdbar, 40, 32);
        auto* cfg_btn = entity->add_component<UIIconButton>();
        cfg_btn->icon_path = "res:/ui/icon_settings.png";
        cfg_btn->icon_size = 18.0f;
        place(cfg_btn, cmdbar, 40, 32);
    }

    auto* title = entity->add_component<UILabel>();
    title->text = "GryceUI Component Gallery"; title->font_size = 24.0f;
    title->size = gryce_engine::math::Vector2f(0.0f, 34.0f);
    entity->set_component_parent(title, main);

    g_gallery.theme_label = entity->add_component<UILabel>();
    // 说明行是次要信息：12px + 次要色，标题才用大字。
    g_gallery.theme_label->font_size = 12.0f;
    g_gallery.theme_label->foreground_role = UIThemeColor::TextMuted;
    g_gallery.theme_label->size = gryce_engine::math::Vector2f(0.0f, 24.0f);
    entity->set_component_parent(g_gallery.theme_label, main);

    auto* theme_row = entity->add_component<UIPanel>();
    theme_row->draws_background = false;
    theme_row->layout_mode = UILayoutMode::Row; theme_row->gap = 8.0f; theme_row->padding = 4.0f;
    theme_row->size = gryce_engine::math::Vector2f(1420.0f, 40.0f); theme_row->min_size = theme_row->size;
    entity->set_component_parent(theme_row, main);


    auto mkbtn = [&](UIPanel* parent, const char* label) -> UIButton* {
        auto* b = entity->add_component<UIButton>();
        b->text = label;
        b->size = gryce_engine::math::Vector2f(132.0f, 32.0f); b->min_size = b->size;
        entity->set_component_parent(b, parent);
        return b;
    };
    g_gallery.b_dark  = mkbtn(theme_row, "Dark");
    g_gallery.b_light = mkbtn(theme_row, "Light");
    g_gallery.b_high  = mkbtn(theme_row, "High Contrast");
    g_gallery.b_nord  = mkbtn(theme_row, "Nord");
    g_gallery.b_fluent = mkbtn(theme_row, "Fluent");

    auto* json_row = entity->add_component<UIPanel>();
    json_row->draws_background = false;
    json_row->layout_mode = UILayoutMode::Row; json_row->gap = 8.0f; json_row->padding = 4.0f;
    json_row->size = gryce_engine::math::Vector2f(1420.0f, 150.0f); json_row->min_size = json_row->size;
    entity->set_component_parent(json_row, main);

    g_gallery.theme_json = entity->add_component<UICodeEditor>();
    g_gallery.theme_json->language = "json";
    g_gallery.theme_json->show_line_numbers = true;
    g_gallery.theme_json->set_code(
        "{\n"
        "  \"name\": \"Custom\",\n"
        "  \"colors\": { \"accent\": \"#33aaff\", \"panel\": \"#1b2030\" },\n"
        "  \"metrics\": { \"padding\": 10 }\n"
        "}\n", false);
    g_gallery.theme_json->size = gryce_engine::math::Vector2f(1080.0f, 140.0f);
    g_gallery.theme_json->min_size = g_gallery.theme_json->size;
    entity->set_component_parent(g_gallery.theme_json, json_row);
    g_gallery.b_apply = mkbtn(json_row, "Apply JSON");

    auto* scroll = entity->add_component<UIScrollView>();
    // fit_all 时把滚动区撑到内容高度：Vulkan 后端没有裁剪实现，滚动后的内容
    // 会溢出到标题区，分页截图会重叠。
    const float scroll_h = fit_all ? (scroll_content_h - 600.0f) : 760.0f;
    scroll->size = gryce_engine::math::Vector2f(1420.0f, scroll_h); scroll->min_size = scroll->size;
    entity->set_component_parent(scroll, main);

    auto section = [&](const char* heading, float h) -> UIPanel* {
        // 每个分区是一张 Fluent 卡片：比页面底亮一档的表面 + 发丝边 + 8px 圆角，
        // 内部 20px 内边距与 12px 行距，标题用 Subtitle 字号拉开层级。
        auto* p = entity->add_component<UIPanel>();
        p->layout_mode = UILayoutMode::Column; p->padding = 20.0f; p->gap = 12.0f;
        p->background_role = UIThemeColor::PanelRaised;
        p->size = gryce_engine::math::Vector2f(1400.0f, h); p->min_size = p->size;
        auto* t = entity->add_component<UILabel>();
        t->text = heading;
        t->font_size = ui_theme_metric(UIThemeMetric::SubtitleFontSize);
        t->size = gryce_engine::math::Vector2f(0.0f, 32.0f);
        entity->set_component_parent(t, p);
        entity->set_component_parent(p, scroll);
        return p;
    };
    // 一行控件：Row 布局 + 12px 间隔。Fluent 的设置页是"标题 -> 若干行"，
    // 每个控件独占整行会让页面拉得很长、也很松散。
    auto subgroup = [&](UIPanel* parent, float h) -> UIPanel* {
        auto* row = entity->add_component<UIPanel>();
        row->layout_mode = UILayoutMode::Row;
        row->gap = 12.0f;
        row->padding = 0.0f;
        row->draws_background = false;
        place(row, parent, 1360.0f, h);
        return row;
    };

    // 0) Fluent 的圆角阶梯与层次
    auto* s0 = section("Surfaces: Corner Radius & Elevation", h_surfaces);
    auto* radius_row = entity->add_component<UIPanel>();
    radius_row->draws_background = false;
    radius_row->layout_mode = UILayoutMode::Row; radius_row->gap = 18.0f; radius_row->padding = 4.0f;
    place(radius_row, s0, 1360, 130);
    auto mkcard = [&](UIPanel* parent, const char* caption, UIRadiusTier tier,
                      float elevation) {
        auto* slot = entity->add_component<UIPanel>();
        slot->draws_background = false;
        place(slot, parent, 210, 120);
        auto* card = entity->add_component<UIPanel>();
        card->radius_tier = tier;
        card->rest_elevation = elevation;
        // 浮起的表面要比底色亮一档，否则深色主题上"抬起"看不出来。
        card->background_role = UIThemeColor::PanelRaised;
        card->set_local_position(gryce_engine::math::Vector3f(20.0f, 18.0f, 0.0f));
        place(card, slot, 170, 70);
        auto* label = entity->add_component<UILabel>();
        label->text = caption;
        label->font_size = 12.0f;
        label->set_local_position(gryce_engine::math::Vector3f(8.0f, 24.0f, 0.0f));
        place(label, card, 154, 22);
    };
    mkcard(radius_row, "radius small", UIRadiusTier::Small, 0.0f);
    mkcard(radius_row, "radius medium", UIRadiusTier::Medium, 0.0f);
    mkcard(radius_row, "radius large", UIRadiusTier::Large, 0.0f);
    mkcard(radius_row, "elevation 3", UIRadiusTier::Medium, 3.0f);
    mkcard(radius_row, "elevation 8", UIRadiusTier::Medium, 8.0f);
    mkcard(radius_row, "elevation 14", UIRadiusTier::Medium, 14.0f);

    auto* motion_row = entity->add_component<UIPanel>();
    motion_row->draws_background = false;
    motion_row->layout_mode = UILayoutMode::Row; motion_row->gap = 12.0f; motion_row->padding = 4.0f;
    place(motion_row, s0, 1360, 44);
    auto mkmotion = [&](const char* caption, float hover_duration, float press_scale) {
        auto* b = entity->add_component<UIButton>();
        b->text = caption;
        b->hover_duration = hover_duration;
        b->press_scale = press_scale;
        place(b, motion_row, 260, 34);
    };
    mkmotion("hover 83ms (fast)", 0.083f, 0.02f);
    mkmotion("hover 167ms (normal)", 0.167f, 0.02f);
    mkmotion("hover 250ms (slow)", 0.25f, 0.04f);
    auto* note = entity->add_component<UILabel>();
    note->text = "radius tiers and elevation come from theme tokens; hover, press and focus run on eased tweens";
    note->font_size = 12.0f;
    place(note, s0, 1200, 20);

    // 0.5) 贴图：UIFitMode 决定缩放/裁切，图标按钮用同一套纹理服务
    auto* s_img = section("Images & Custom Textures", h_images);
    auto* row_img = subgroup(s_img, 176);
    auto mkimg = [&](const char* path, gryce_engine::ui::UIFitMode mode, float w, float h,
                     const char* caption) {
        auto* holder = entity->add_component<UIPanel>();
        holder->draws_background = false;
        holder->layout_mode = UILayoutMode::Column;
        holder->gap = 8.0f;
        place(holder, row_img, w, 176);
        auto* im = entity->add_component<UIImage>();
        im->texture_path = path;
        im->fit = mode;
        place(im, holder, w, h);
        auto* lab = entity->add_component<UILabel>();
        lab->text = caption;
        lab->font_size = 12.0f;
        lab->foreground_role = UIThemeColor::TextMuted;
        place(lab, holder, w, 20);
    };
    mkimg("res:/ui/demo_photo.png", gryce_engine::ui::UIFitMode::Cover, 300, 148, "Cover - fills, crops the sides");
    mkimg("res:/ui/demo_photo.png", gryce_engine::ui::UIFitMode::Contain, 160, 148, "Contain - whole image");
    mkimg("res:/ui/demo_photo.png", gryce_engine::ui::UIFitMode::Stretch, 120, 148, "Stretch");

    auto* row_icon = subgroup(s_img, 40);
    for (const char* name : {"folder", "save", "undo", "redo", "play", "search",
                             "settings", "close", "check"}) {
        auto* b = entity->add_component<UIIconButton>();
        b->icon_path = std::string("res:/ui/icon_") + name + ".png";
        b->icon_size = 18.0f;
        place(b, row_icon, 40, 32);
    }
    auto* icon_note = entity->add_component<UILabel>();
    icon_note->text = "24px PNG icons loaded through the UI texture service (path -> cached texture handle)";
    icon_note->font_size = 12.0f;
    icon_note->foreground_role = UIThemeColor::TextMuted;
    place(icon_note, s_img, 1200, 20);

    // 1) 按钮：Standard / Accent / Subtle 三种变体 + 切换与图标按钮
    auto* s1 = section("Buttons", h_buttons);
    auto* row1 = subgroup(s1, 40);
    auto* b1 = entity->add_component<UIButton>(); b1->text = "Standard"; place(b1, row1, 140, 32);
    auto* b2 = entity->add_component<UIButton>(); b2->text = "Accent";
    b2->variant = UIButtonVariant::Accent; place(b2, row1, 140, 32);
    auto* b3 = entity->add_component<UIButton>(); b3->text = "Subtle";
    b3->variant = UIButtonVariant::Subtle; place(b3, row1, 140, 32);
    auto* tg = entity->add_component<UIToggleButton>(); tg->text = "Toggle";
    tg->checked = true; place(tg, row1, 140, 32);
    auto* ic = entity->add_component<UIIconButton>(); ic->text = "Icon"; ic->icon = "*"; place(ic, row1, 120, 32);

    auto* row2 = subgroup(s1, 40);
    auto* sw1 = entity->add_component<UIToggleSwitch>(); sw1->text = "Wi-Fi";
    sw1->on = true; sw1->switch_tween.reset(1.0f); place(sw1, row2, 180, 32);
    auto* sw2 = entity->add_component<UIToggleSwitch>(); sw2->text = "Bluetooth"; place(sw2, row2, 200, 32);
    auto* cb = entity->add_component<UICheckBox>(); cb->text = "Checkbox"; cb->checked = true; place(cb, row2, 180, 32);
    auto* cb2 = entity->add_component<UICheckBox>(); cb2->text = "Unchecked"; place(cb2, row2, 180, 32);
    auto* r1 = entity->add_component<UIRadioButton>(); r1->text = "Radio A"; r1->selected = true; place(r1, row2, 150, 32);
    auto* r2 = entity->add_component<UIRadioButton>(); r2->text = "Radio B"; place(r2, row2, 150, 32);

    auto* row3 = subgroup(s1, 40);
    auto* pb = entity->add_component<UIProgressBar>(); pb->value = 0.62f; place(pb, row3, 260, 32);
    auto* sl = entity->add_component<UISlider>(); sl->value = 0.42f; place(sl, row3, 320, 32);
    auto* rs = entity->add_component<UIRangeSlider>(); rs->low = 0.25f; rs->high = 0.75f; place(rs, row3, 320, 32);

    // 2) 文本 / 数值输入
    auto* s2 = section("Text & Numeric Inputs", h_inputs);
    auto* row4 = subgroup(s2, 40);
    auto* ti = entity->add_component<UITextInput>(); ti->text = "res:/scenes/main.gesc"; place(ti, row4, 340, 32);
    auto* sb = entity->add_component<UISearchBox>(); place(sb, row4, 260, 32);
    auto* cbx = entity->add_component<UIComboBox>(); cbx->items = "Option A\nOption B\nOption C";
    cbx->selected = 0; place(cbx, row4, 200, 32);
    auto* kb = entity->add_component<UIKeyBindingEditor>(); kb->binding = "Ctrl+S"; place(kb, row4, 180, 32);

    auto* row5 = subgroup(s2, 40);
    auto* ni = entity->add_component<UINumberInput>(); ni->set_value(3.14f, false); place(ni, row5, 160, 32);
    auto* df = entity->add_component<UIDragFloat>(); df->set_value(2.5f, false); place(df, row5, 220, 32);
    auto* ve = entity->add_component<UIVectorEditor>(); ve->set_value(0, 1.0f, false);
    ve->set_value(1, 2.0f, false); place(ve, row5, 280, 32);

    auto* row6 = subgroup(s2, 160);
    auto* cp = entity->add_component<UIColorPicker>();
    cp->color = gryce_engine::math::Vector4f(1.0f, 0.5f, 0.1f, 1.0f); place(cp, row6, 220, 150);

    // 3) 导航 / 选择器
    auto* s3 = section("Navigation & Pickers", h_nav);
    auto* row7 = subgroup(s3, 40);
    auto* tb = entity->add_component<UITabBar>(); tb->items = "Scene\nGame\nAssets";
    tb->selected = 0; place(tb, row7, 380, 32);

    auto* row8 = subgroup(s3, 40);
    auto* tlb = entity->add_component<UIToolbar>();
    { auto* x1 = entity->add_component<UIButton>(); x1->text = "Open"; place(x1, tlb, 92, 28);
      auto* x2 = entity->add_component<UIButton>(); x2->text = "Save"; place(x2, tlb, 92, 28);
      auto* x3 = entity->add_component<UIButton>(); x3->text = "Build";
      place(x3, tlb, 92, 28); }
    place(tlb, row8, 440, 40);
    auto* bc = entity->add_component<UIBreadcrumb>();
    bc->size = gryce_engine::math::Vector2f(400.0f, 32.0f); bc->min_size = bc->size;
    place(bc, row8, 400, 32);

    auto* row9 = subgroup(s3, 40);
    auto* sp = entity->add_component<UISplitter>(); sp->vertical = true; place(sp, row9, 8, 40);

    // 3b) 窗口外壳：NavigationView + InfoBar
    //
    // 版式照 WinUI 3 的设置页：左侧固定宽度的导航，右侧一列全宽通知条。
    // 导航项的图标走自定义贴图，不复用字体字形。
    auto* s3b = section("Shell: NavigationView & InfoBar", h_shell);
    auto* shell_row = entity->add_component<UIPanel>();
    shell_row->draws_background = false;
    shell_row->layout_mode = UILayoutMode::Row;
    shell_row->gap = 24.0f;
    shell_row->padding = 0.0f;
    place(shell_row, s3b, 1360.0f, 424.0f);

    auto* nav = entity->add_component<UINavigationView>();
    nav->header = "Workspace";
    nav->row_height = 42.0f;
    nav->items = {
        {"Home", "res:/ui/icon_folder.png", false},
        {"Scenes", "res:/ui/icon_save.png", false},
        {"Playback", "res:/ui/icon_play.png", false},
        {"History", "res:/ui/icon_undo.png", false},
        {"Search", "res:/ui/icon_search.png", false},
        {"Settings", "res:/ui/icon_settings.png", true},
    };
    nav->selected = 1;
    place(nav, shell_row, 300, 424);

    auto* bar_col = entity->add_component<UIPanel>();
    bar_col->draws_background = false;
    bar_col->layout_mode = UILayoutMode::Column;
    bar_col->gap = 12.0f;
    bar_col->padding = 0.0f;
    place(bar_col, shell_row, 1036, 424);

    auto mkinfobar = [&](UIInfoBarSeverity severity, const char* heading,
                         const char* message) {
        auto* bar = entity->add_component<UIInfoBar>();
        bar->severity = severity;
        bar->title = heading;
        bar->message = message;
        place(bar, bar_col, 1036, 56);
    };
    mkinfobar(UIInfoBarSeverity::Informational, "Update available",
              "Gryce 1.4 is ready to install — restart to finish.");
    mkinfobar(UIInfoBarSeverity::Success, "Build succeeded",
              "Scene compiled in 2.3 s with 0 warnings.");
    mkinfobar(UIInfoBarSeverity::Warning, "Unreferenced asset",
              "textures/rock.png is not used by any material.");
    mkinfobar(UIInfoBarSeverity::Error, "Shader compile failed",
              "pbr.frag line 42: unknown uniform u_light_dir.");

    // 4) 列表 / 树 / 表格
    auto* s4 = section("Lists, Trees & Tables", h_lists);
    auto* lv = entity->add_component<UIListView>(); lv->items = "Item One\nItem Two\nItem Three\nItem Four"; lv->selected = 1; place(lv, s4, 300, 140);
    auto* tv = entity->add_component<UITreeView>();
    tv->items.push_back({"Root Node", 0, true, true, true});
    tv->items.push_back({"  Child A", 1, true, false, true});
    tv->items.push_back({"  Child B", 1, true, false, true});
    tv->items.push_back({"    Sub A1", 2, true, false, true});
    tv->items.push_back({"  Leaf", 1, true, false, true});
    place(tv, s4, 300, 200);
    auto* tbl = entity->add_component<UITableView>();
    tbl->columns = "Name\nType\nSize";
    tbl->rows = "Main.gesc|Scene|4 KB\nplayer.js|Script|8 KB\nrock.png|Texture|16 KB";
    place(tbl, s4, 500, 200);
    auto* ag = entity->add_component<UIAssetGrid>(); ag->items = "cube.obj\nmain.gesc\nplayer.js\nrock.png\nlogo.png"; place(ag, s4, 400, 200);
    auto* pg = entity->add_component<UIPropertyGrid>(); place(pg, s4, 400, 160);
    auto* cv = entity->add_component<UIConsoleView>(); cv->items = "> loaded scene\n> 5 meshes\n> render ok"; place(cv, s4, 500, 140);

    // 5) 高级 / 编辑器
    auto* s5 = section("Advanced & Editors", h_editors);
    auto* ce = entity->add_component<UICodeEditor>();
    ce->language = "javascript";
    ce->set_code("function greet(name) {\n  return `Hello, ${name}`;\n}\nconsole.log(greet('Gryce'));\n", false);
    place(ce, s5, 700, 260);
    auto* st = entity->add_component<UIStatusBar>(); st->text = "Ready"; st->right_text = "GryceEngine 1.0"; place(st, s5, 800, 24);
    auto* nc = entity->add_component<UINotificationCenter>(); place(nc, s5, 400, 160);
    auto* ds = entity->add_component<UIDragSource>(); ds->label = "Drag me"; ds->payload = "asset/cube"; place(ds, s5, 200, 32);
    auto* dz = entity->add_component<UIDropZone>(); dz->last_payload = "drop here"; place(dz, s5, 200, 60);
    auto* dp = entity->add_component<UIDockPanel>(); dp->title = "Inspector"; place(dp, s5, 400, 160);
    auto* img = entity->add_component<UIImage>(); place(img, s5, 200, 140);
    auto* vp = entity->add_component<UIViewportPanel>(); place(vp, s5, 400, 220);
    auto* lbl = entity->add_component<UILabel>(); lbl->text = "A plain label with themed text color."; place(lbl, s5, 400, 24);

    // 6) 弹层 / 对话框 / 通知
    auto* s6 = section("Popups, Dialogs & Notifications", h_popups);
    auto* md = entity->add_component<UIMessageDialog>(); md->title = "Saved"; md->message = "Your work was saved."; md->visible = true; md->receives_input = true; place(md, s6, 440, 160);
    auto* cd = entity->add_component<UIConfirmDialog>(); cd->title = "Delete"; cd->message = "Delete this asset?"; cd->visible = true; cd->receives_input = true; place(cd, s6, 460, 160);
    auto* fd = entity->add_component<UIFileDialog>(); fd->title = "Open File"; fd->entries = "scenes\nscripts\nmodels\ntextures"; fd->visible = true; fd->receives_input = true; place(fd, s6, 620, 300);
    auto* mn = entity->add_component<UIMenu>(); mn->items = "Open\nSave\nExit"; mn->visible = true; mn->receives_input = true; place(mn, s6, 220, 90);
    auto* cm = entity->add_component<UIContextMenu>(); cm->visible = true; cm->receives_input = true; place(cm, s6, 220, 90);
    auto* toast = entity->add_component<UIToast>(); toast->text = "Build succeeded"; toast->visible = true; toast->receives_input = true; place(toast, s6, 320, 50);
    auto* tip = entity->add_component<UITooltip>(); tip->text = "Tooltip text"; tip->visible = true; tip->receives_input = false; place(tip, s6, 180, 28);
    auto* fp = entity->add_component<UIFloatingPanel>(); fp->title = "Floating"; fp->visible = true; fp->receives_input = true; place(fp, s6, 360, 170);

    scroll->content_height = scroll_content_h;
    if (scroll_y > 0) scroll->scroll_y = static_cast<float>(scroll_y);
    return true;
}

// ---------------------------------------------------------------------------
// 无场景代码编辑器演示（--ui-codeeditor）
//
// 只建 UIRoot + 一个 UICodeEditor（外加一行状态标签），不加载任何 3D 场景：
// 配合 GCore_SetAutoLoadMainScene(false)，引擎保留一个空的 "Untitled" 场景，
// 世界与渲染照常初始化，但没有任何网格/相机/灯光。用于验证：
//   * 语法高亮（tokenizer → 彩色 run → draw_text_colored）
//   * 查找高亮（find_query → 行内匹配背景块）
//   * 撤销/重做（undo_depth 状态 + Ctrl+Z / Ctrl+Y）
//   * 字体抗锯齿（小字号独立烘档后的清晰度）
// ---------------------------------------------------------------------------
static bool inject_code_editor_demo(const std::string& language, const std::string& find_query,
                                    float code_font_size) {
    using namespace gryce_engine::components;
    auto* world = static_cast<gryce_engine::ecs::World*>(GCore_GetInternalWorldPtr());
    if (!world || !world->scene()) return false;
    auto* entity = world->scene()->create_entity("Code Editor Demo");
    if (!entity) return false;

    const bool js = (language.rfind("javascript", 0) == 0) || language == "js" ||
                    language == "typescript" || language == "ts";
    const std::string sample = js ? std::string(R"(// Gryce engine — JavaScript syntax highlight demo
import { render } from './engine.js';

const MAX_STEPS = 128;
let accumulator = 0.0;

/* Render the scene into an offscreen target.
   The word render appears here and below for the find demo. */
function renderScene(scene, camera) {
  const light = { dir: [0, -1, 0], color: '#ffddaa' };
  for (let i = 0; i < MAX_STEPS; ++i) {
    accumulator += render(scene, camera, i) * 0.5;
  }
  return accumulator > 1.0 ? accumulator : 1.0;
}

export default renderScene;
)") : std::string(R"(#version 450 core
// Gryce engine — GLSL syntax highlight demo
layout(location = 0) out vec4 frag_color;

uniform sampler2D u_albedo;
uniform vec3 u_light_dir;   // normalized, world space

float specular(vec3 n, vec3 l, vec3 v, float shininess) {
    vec3 h = normalize(l + v);
    return pow(max(dot(n, h), 0.0), shininess);
}

void main() {
    vec4 base = texture(u_albedo, uv);          // sample the render target
    float ndl = max(dot(normal, u_light_dir), 0.0);
    float spec = specular(normal, u_light_dir, view_dir, 64.0);
    vec3 color = base.rgb * (ndl + spec);       // render with light + specular
    frag_color = vec4(color, base.a);
}
)");

    auto* root = entity->add_component<UIRoot>();
    auto* panel = entity->add_component<UIPanel>();
    panel->layout_mode = UILayoutMode::Column;
    panel->padding = 10.0f; panel->gap = 8.0f;
    panel->set_local_position(gryce_engine::math::Vector3f(24.0f, 24.0f, 0.0f));
    panel->size = gryce_engine::math::Vector2f(1470.0f, 950.0f);
    panel->min_size = panel->size;
    entity->set_component_parent(panel, root);

    auto* editor = entity->add_component<UICodeEditor>();
    editor->language = language;
    editor->show_line_numbers = true;
    editor->highlight = true;
    editor->highlight_current_line = true;
    editor->font_size = code_font_size;
    editor->line_height = std::round(code_font_size * 1.38f);
    editor->set_code(sample, false);
    editor->set_find_query(find_query);
    editor->set_editor_cursor(0);
    editor->set_editor_focus(true);
    editor->size = gryce_engine::math::Vector2f(1450.0f, 880.0f);
    editor->min_size = editor->size;
    entity->set_component_parent(editor, panel);

    auto* status = entity->add_component<UILabel>();
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "lang=%s   find=\"%s\"   matches=%d   undo_depth=%d   (Ctrl+Z / Ctrl+Y)",
                  language.c_str(), find_query.c_str(),
                  editor->match_count(), editor->undo_depth());
    status->text = buf;
    status->font_size = 14.0f;
    status->size = gryce_engine::math::Vector2f(0.0f, 24.0f);
    entity->set_component_parent(status, panel);
    return true;
}

// ---------------------------------------------------------------------------
// 代码编辑器逻辑自测（--code-selftest）：不建窗口/场景，直接验证 tokenizer、
// 查找计数、撤销/重做。用真实的 Character 事件驱动编辑路径。
// ---------------------------------------------------------------------------
static int run_code_selftest() {
    using gryce_engine::components::UICodeEditor;
    using gryce_engine::ui::CodeTokenRole;
    using gryce_engine::ui::UIEvent;
    using gryce_engine::ui::UIEventType;
    bool ok = true;

    UICodeEditor ed;
    ed.language = "javascript";
    ed.set_code("let a = 1;\nconst b = 2;\n", false);

    {
        const auto tokens = gryce_engine::ui::tokenize_code_line("let a = 1;", "javascript");
        bool kw = false, num = false;
        for (const auto& t : tokens) {
            if (t.role == CodeTokenRole::Keyword) kw = true;
            if (t.role == CodeTokenRole::Number) num = true;
        }
        ok = ok && kw && num;
        std::printf("[selftest] tokenizer keyword=%d number=%d\n", kw ? 1 : 0, num ? 1 : 0);
    }
    {
        ed.set_find_query("const");
        const int m = ed.match_count();
        ok = ok && (m == 1);
        std::printf("[selftest] match_count(const)=%d expect=1\n", m);
        ed.set_find_query("");
    }
    {
        ed.set_editor_focus(true);
        ed.set_editor_cursor(ed.text.size());
        const std::string before = ed.text;
        UIEvent ev;
        ev.type = UIEventType::Character;
        ev.codepoint = static_cast<uint32_t>('X');
        ed.on_ui_event(ev);
        const bool inserted = (ed.text.size() == before.size() + 1) && ed.text.back() == 'X';
        const int depth = ed.undo_depth();
        ed.undo();
        const bool undone = (ed.text == before);
        ed.redo();
        const bool redone = (ed.text.size() == before.size() + 1) && ed.text.back() == 'X';
        ok = ok && inserted && depth >= 1 && undone && redone;
        std::printf("[selftest] edit=%d undo_depth=%d undo=%d redo=%d\n",
                    inserted ? 1 : 0, depth, undone ? 1 : 0, redone ? 1 : 0);
        ed.set_editor_focus(false);
    }
    std::printf("[selftest] %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}

// 圆角 / 层次 / 动效自测（--fluent-selftest）：不建窗口，直接驱动 UIRuntime，
// 检查半径是否来自主题、hover 提亮是否单调收敛、按下是否产生内缩、焦点框是否
// 随焦点淡入，以及悬停时是否真的多出一条投影命令。
static int run_fluent_selftest() {
    using namespace gryce_engine::components;
    using namespace gryce_engine::ui;
    bool ok = true;

    set_ui_theme_preset(UIThemePreset::Fluent);
    const float small_radius = ui_theme_radius(UIRadiusTier::Small);
    const float medium_radius = ui_theme_radius(UIRadiusTier::Medium);
    std::printf("[selftest] radius small=%.1f medium=%.1f large=%.1f\n",
                small_radius, medium_radius, ui_theme_radius(UIRadiusTier::Large));

    gryce_engine::scene::Entity entity("FluentMotion");
    auto* root = entity.add_component<UIPanel>();
    root->size = gryce_engine::math::Vector2f(400.0f, 200.0f);
    auto* button = entity.add_component<UIButton>();
    button->text = "Button";
    button->size = gryce_engine::math::Vector2f(120.0f, 32.0f);
    button->min_size = button->size;
    button->set_local_position(gryce_engine::math::Vector3f(10.0f, 10.0f, 0.0f));
    button->hover_duration = 0.10f;
    button->press_duration = 0.08f;
    entity.set_component_parent(button, root);

    UIRuntime runtime;
    runtime.set_root(root);
    runtime.set_viewport({0.0f, 0.0f, 400.0f, 200.0f});

    gryce_engine::ui::UIInputFrame idle;
    idle.pointer = gryce_engine::math::Vector2f(-5.0f, -5.0f);
    idle.delta_time = 0.02f;
    runtime.update(idle);

    auto step = [&](float px, float py, bool down, bool edge, uint64_t serial) {
        gryce_engine::ui::UIInputFrame frame;
        frame.pointer = gryce_engine::math::Vector2f(px, py);
        frame.pointer_down[0] = down;
        frame.pointer_pressed[0] = edge && down;
        frame.delta_time = 0.02f;
        frame.frame_serial = serial;
        runtime.update(frame);
    };

    // hover：单调上升且按缓动收敛到 1（不是线性跳变）。
    float previous = 0.0f;
    bool monotonic = true;
    float mid = 0.0f;
    for (uint64_t i = 0; i < 10; ++i) {
        step(20.0f, 20.0f, false, false, 100 + i);
        const float h = button->hover_amount();
        if (h + 1e-4f < previous) monotonic = false;
        if (i == 1) mid = h;
        previous = h;
    }
    const bool hover_ok = monotonic && previous > 0.99f && mid < previous;
    ok = ok && hover_ok;
    std::printf("[selftest] hover first=%.2f settled=%.2f monotonic=%d\n",
                mid, previous, monotonic ? 1 : 0);

    // 悬停时应当多出一条投影命令（hover_elevation = 3）。
    UIDrawList hovered_draw;
    runtime.build_draw_list(hovered_draw);
    int shadows = 0;
    float button_radius = -1.0f;
    for (const UIDrawCommand& command : hovered_draw.commands()) {
        if (command.type == UIDrawCommandType::Shadow) ++shadows;
        if (command.type == UIDrawCommandType::Rect &&
            std::fabs(command.rect.w - 120.0f) < 0.5f && button_radius < 0.0f) {
            button_radius = command.radii[0];
        }
    }
    const bool radius_ok = std::fabs(button_radius - small_radius) < 0.01f;
    const bool shadow_ok = shadows >= 1;
    ok = ok && radius_ok && shadow_ok;
    std::printf("[selftest] button_radius=%.1f expect=%.1f shadows=%d\n",
                button_radius, small_radius, shadows);

    // press：按下后绘制矩形应当比布局矩形小（按压内缩）。
    for (uint64_t i = 0; i < 8; ++i) step(20.0f, 20.0f, true, i == 0, 200 + i);
    UIDrawList pressed_draw;
    runtime.build_draw_list(pressed_draw);
    float pressed_w = -1.0f;
    for (const UIDrawCommand& command : pressed_draw.commands()) {
        if (command.type == UIDrawCommandType::Rect &&
            command.rect.y > 5.0f && command.rect.w > 100.0f) {
            pressed_w = command.rect.w;
            break;
        }
    }
    const bool press_ok = button->press_amount() > 0.9f &&
                          pressed_w > 0.0f && pressed_w < 119.5f;
    ok = ok && press_ok;
    std::printf("[selftest] press=%.2f draw_w=%.2f layout_w=120.0\n",
                button->press_amount(), pressed_w);

    // 焦点框：淡入而不是瞬变。按下时已经拿过焦点，先让它降下去再重新聚焦。
    step(-5.0f, -5.0f, false, false, 300);
    button->set_focused_state(false);
    for (uint64_t i = 0; i < 10; ++i) step(-5.0f, -5.0f, false, false, 301 + i);
    const float focus_rest = button->focus_amount();
    button->set_focused_state(true);
    step(-5.0f, -5.0f, false, false, 320);
    const float focus_mid = button->focus_amount();
    for (uint64_t i = 0; i < 10; ++i) step(-5.0f, -5.0f, false, false, 321 + i);
    const bool focus_ok = focus_rest < 0.01f && focus_mid > 0.0f && focus_mid < 1.0f &&
                          button->focus_amount() > 0.99f;
    ok = ok && focus_ok;
    std::printf("[selftest] focus rest=%.2f mid=%.2f settled=%.2f\n",
                focus_rest, focus_mid, button->focus_amount());

    std::printf("[selftest] %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// 交互式相机（--play）：鼠标锁定 + 视角 + WASD
// ---------------------------------------------------------------------------
// GLFW 键码是固定值（GLFW_KEY_*），这里直接用常量，避免为了几个按键把 GLFW
// 头文件拉进宿主。
constexpr int kKeySpace = 32;
constexpr int kKeyA = 65, kKeyD = 68, kKeyE = 69, kKeyQ = 81, kKeyS = 83, kKeyW = 87;
constexpr int kKeyEscape = 256;
constexpr int kKeyLeftShift = 340, kKeyLeftControl = 341;
// 左右 Alt（GLFW_KEY_LEFT_ALT / RIGHT_ALT）：按一下切换"锁定/解锁"指针。
constexpr int kKeyLeftAlt = 342, kKeyRightAlt = 346;

// 相机本地坐标约定：-Z 前、+X 右、+Y 上（与 math::Camera::update_vectors 一致）。
struct FpsCamera {
    GEntityHandle entity = 0;
    GVec3 position{};
    float yaw_deg = 0.0f;    // 绕世界 +Y
    float pitch_deg = 0.0f;  // 绕相机本地 +X
    float speed = 3.0f;      // m/s
    float sensitivity = 0.12f;  // 度/像素
    bool mouse_ready = false;
    double last_mx = 0.0;
    double last_my = 0.0;
    // 指针是否处于"锁定（GLFW_CURSOR_DISABLED）"态。每帧按下面条件重算：
    //   窗口有焦点 && 用户没主动解锁 → 锁定；否则放开（指针可见可移动）。
    bool cursor_locked = false;
    // 用户通过 Alt 主动切换出的"解锁"态：true=指针放开、视角冻结。
    // 是"按一下切换"，不是"按住"：松开 Alt 不会自动恢复锁定。
    bool user_unlocked = false;
    // 上一帧的 Alt 状态：只在"按下沿"触发一次切换，长按产生的键盘自动重复
    // 不会连续误触发。
    bool alt_was_held = false;
    // 上一帧的焦点状态：重新获得焦点时把 user_unlocked 复位成锁定。
    bool was_focused = false;

    static void normalize(GQuat& q) {
        const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (len > 1e-6f) {
            q.x /= len; q.y /= len; q.z /= len; q.w /= len;
        }
    }
    static GQuat axis_angle(float ax, float ay, float az, float radians) {
        const float h = radians * 0.5f;
        const float s = std::sin(h);
        GQuat q{ax * s, ay * s, az * s, std::cos(h)};
        normalize(q);
        return q;
    }
    static GQuat mul(const GQuat& a, const GQuat& b) {
        GQuat r{
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
        normalize(r);
        return r;
    }
    static void rotate(const GQuat& q, float vx, float vy, float vz, GVec3& out) {
        // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
        const float cx = q.y * vz - q.z * vy + q.w * vx;
        const float cy = q.z * vx - q.x * vz + q.w * vy;
        const float cz = q.x * vy - q.y * vx + q.w * vz;
        out.x = vx + 2.0f * (q.y * cz - q.z * cy);
        out.y = vy + 2.0f * (q.z * cx - q.x * cz);
        out.z = vz + 2.0f * (q.x * cy - q.y * cx);
    }

    // yaw/pitch → 四元数：先本地俯仰、再世界偏航（标准 FPS 组合）
    GQuat to_quat() const {
        const GQuat qy = axis_angle(0.0f, 1.0f, 0.0f,
                                    static_cast<float>(yaw_deg * 3.14159265358979 / 180.0));
        const GQuat qp = axis_angle(1.0f, 0.0f, 0.0f,
                                    static_cast<float>(pitch_deg * 3.14159265358979 / 180.0));
        return mul(qy, qp);
    }
    // 从实体当前旋转反解 yaw/pitch：forward 满足 (-cosφ sinψ, sinφ, -cosφ cosψ)
    bool init_from_entity(float speed_value, float sensitivity_value) {
        entity = find_camera_entity();
        if (entity == 0) return false;
        speed = speed_value;
        sensitivity = sensitivity_value;

        GQuat q{};
        GVec3 p{};
        if (GEntity_GetLocalRotation(entity, &q) != 0) return false;
        if (GEntity_GetLocalPosition(entity, &p) != 0) return false;
        position = p;

        GVec3 f{};
        rotate(q, 0.0f, 0.0f, -1.0f, f);
        const float fy = std::max(-1.0f, std::min(1.0f, f.y));
        pitch_deg = static_cast<float>(std::asin(fy) * 180.0 / 3.14159265358979);
        yaw_deg = static_cast<float>(std::atan2(-f.x, -f.z) * 180.0 / 3.14159265358979);
        return true;
    }

    static GEntityHandle find_camera_entity() {
        const int count = GEntity_GetCount();
        GEntityHandle fallback = 0;
        for (int i = 0; i < count; ++i) {
            const GEntityHandle e = GEntity_GetAt(i);
            if (e <= 0) continue;
            char name[128] = {};
            GEntity_GetName(e, name, static_cast<int>(sizeof(name)));
            const int comp_count = GComponent_GetCount(e);
            for (int c = 0; c < comp_count; ++c) {
                char type_name[128] = {};
                if (GComponent_GetTypeNameAt(e, c, type_name,
                                             static_cast<int>(sizeof(type_name))) < 0) {
                    continue;
                }
                std::string tn = type_name;
                const auto pos = tn.rfind("::");
                if (pos != std::string::npos) tn = tn.substr(pos + 2);
                if (tn != "Camera") continue;
                if (fallback == 0) fallback = e;
                if (std::strcmp(name, "MainCamera") == 0) return e;
            }
        }
        return fallback;
    }

    // 每帧：鼠标转视角 + WASD 位移。返回 false 表示用户按 Esc 要求退出。
    bool update(Window& window, float dt) {
        if (entity == 0) return true;

        if (window.get_key(kKeyEscape)) return false;

        // ---- 指针锁定策略 ----
        //  1) **只在窗口有焦点时锁定**：Alt-Tab 切走还锁着鼠标的话，指针被系统
        //     扣在这个窗口里，别的窗口点不到、也看不到指针。失焦即放开。
        //  2) **按一下 Alt 切换**（是"切换"，不是"按住"）：解锁后指针可见、视角
        //     冻结，可以自由去点菜单或别的窗口；再按一下 Alt 恢复锁定。松开 Alt
        //     不会自动恢复，所以"按住"语义下那套长按/自动重复的坑不存在。
        //  3) 重新获得焦点时回到锁定态，避免"点回游戏却还停在解锁态、鼠标转不了视角"。
        const bool alt_down = window.get_key(kKeyLeftAlt) || window.get_key(kKeyRightAlt);
        if (alt_down != alt_was_held) {
            // 原始按键边沿：只看"键有没有送到窗口、送了几次"，与锁定策略无关。
            // 排查"按一下 Alt 却切了两次/没切"时先看这一行（一次按下只应有一行 DOWN）。
            std::printf("[smoke] alt raw: %s (focused=%d)\n", alt_down ? "DOWN" : "UP",
                        window.has_focus() ? 1 : 0);
            std::fflush(stdout);
        }
        if (alt_down && !alt_was_held) {
            // 只在"按下沿"切换一次：长按产生的键盘自动重复不会连续误触发。
            user_unlocked = !user_unlocked;
            // ASCII 前缀 "alt toggle:" 便于脚本计数（test/tools/cursor_lock_stress.ps1）。
            std::printf("[smoke] alt toggle: %s (focused=%d)\n",
                        user_unlocked ? "UNLOCKED" : "LOCKED",
                        window.has_focus() ? 1 : 0);
            std::fflush(stdout);
        }
        alt_was_held = alt_down;

        const bool focused = window.has_focus();
        if (focused && !was_focused) user_unlocked = false;   // 重新进入窗口 → 默认锁定
        was_focused = focused;

        const bool want_lock = focused && !user_unlocked;
        if (want_lock != cursor_locked) {
            cursor_locked = want_lock;
            window.set_cursor_disabled(want_lock);
            // 状态变化时打一行日志：交互模式出问题时（"鼠标被别的窗口抢走后没回来"
            // 之类）能从日志一眼看出是哪一步没生效。
            // 用 ASCII 标记（LOCKED/UNLOCKED + reason=…）：中文说明在后，
            // 这样脚本也能按 ASCII 前缀校验状态机（test/tools/cursor_lock_probe.ps1）。
            std::printf("[smoke] cursor state: %s (reason=%s) 指针%s（%s）\n",
                        want_lock ? "LOCKED" : "UNLOCKED",
                        !focused ? "focus-lost" : (user_unlocked ? "alt" : "focus"),
                        want_lock ? "已锁定" : "已放开",
                        !focused ? "窗口失焦" : (user_unlocked ? "Alt 解锁" : "窗口获得焦点"));
            std::fflush(stdout);   // 交互模式被强杀时也要留下这行（重定向时是整块缓冲）
            // 刚锁上时把指针挪回中心并对齐基线，避免"放开期间指针在别处"
            // 造成解锁瞬间视角跳一下。
            mouse_ready = false;
        }
        if (!cursor_locked) {
            // 未锁定（失焦或用户解锁）：不读鼠标增量、不回中、不转视角。
            // 只把当前指针位置记成基线，重新锁定时不会跳。
            window.get_cursor_pos(last_mx, last_my);
            mouse_ready = true;
            // 位移仍然照常（解锁也能走位，和大多数游戏一致）。
            apply_movement(window, dt);
            return true;
        }

        bool moved = false;
#ifdef __EMSCRIPTEN__
        // 浏览器里不能靠 GLFW 坐标差：pointer lock 期间客户端把光标位置钉死
        // （clientX/Y 恒定），GLFW 拷进来的就是一个常数 → 视角纹丝不动；未锁定时
        // 坐标又是"画布内绝对位置"，指针一碰画布边缘就停住。真实位移只有
        // movementX/Y 有，壳侧已按画布事件攒好，这里整帧取走。
        double mdx = 0.0;
        double mdy = 0.0;
        if (window.take_pointer_delta(mdx, mdy)) {
            yaw_deg -= static_cast<float>(mdx) * sensitivity;    // 右移 → 右转
            pitch_deg -= static_cast<float>(mdy) * sensitivity;  // 上移 → 抬头
            pitch_deg = std::max(-89.0f, std::min(89.0f, pitch_deg));
            moved = true;
        }
        // 基线照旧跟着实际位置走：解锁期间指针会飘到别处，重新锁定时不能跳。
        window.get_cursor_pos(last_mx, last_my);
        mouse_ready = true;
#else
        int w = 0, h = 0;
        window.get_size(w, h);
        const double cx = w > 0 ? w * 0.5 : 0.0;
        const double cy = h > 0 ? h * 0.5 : 0.0;

        double mx = 0.0, my = 0.0;
        window.get_cursor_pos(mx, my);
        if (!mouse_ready) {
            mouse_ready = true;
            // 刚锁定的这一帧先把指针拉回中心，再以中心为基线（丢弃这一帧的增量）。
            last_mx = cx;
            last_my = cy;
            window.set_cursor_pos(cx, cy);
            apply_movement(window, dt);
            return true;
        }
        const double dx = mx - last_mx;
        const double dy = my - last_my;
        // 立即回中：锁定模式下光标会被系统不断推走，回中可以避免越界与漂移
        window.set_cursor_pos(cx, cy);
        last_mx = cx;
        last_my = cy;

        if (dx != 0.0 || dy != 0.0) {
            yaw_deg -= static_cast<float>(dx) * sensitivity;    // 右移 → 右转
            pitch_deg -= static_cast<float>(dy) * sensitivity;  // 上移 → 抬头
            pitch_deg = std::max(-89.0f, std::min(89.0f, pitch_deg));
            moved = true;
        }
#endif

        if (apply_movement(window, dt)) moved = true;

        if (moved) {
            const GQuat q = to_quat();
            GEntity_SetLocalRotation(entity, &q);
        }
        return true;
    }

    // WASD/空格/Ctrl 位移（视角锁定与否都要能用：按住 Alt 只是放开指针，
    // 不该把走位也一起禁掉）。返回是否真的移动了。
    bool apply_movement(Window& window, float dt) {
        const float in_fwd = (window.get_key(kKeyW) ? 1.0f : 0.0f) -
                             (window.get_key(kKeyS) ? 1.0f : 0.0f);
        const float in_side = (window.get_key(kKeyD) ? 1.0f : 0.0f) -
                              (window.get_key(kKeyA) ? 1.0f : 0.0f);
        const float in_up = ((window.get_key(kKeySpace) || window.get_key(kKeyE)) ? 1.0f : 0.0f) -
                            ((window.get_key(kKeyLeftControl) || window.get_key(kKeyQ)) ? 1.0f : 0.0f);
        if (in_fwd == 0.0f && in_side == 0.0f && in_up == 0.0f) return false;

        const GQuat q = to_quat();
        GVec3 f{};
        rotate(q, 0.0f, 0.0f, -1.0f, f);
        // 水平前向（WASD 只在地面平面内移动，抬头不影响前进方向）
        const float flat = std::sqrt(f.x * f.x + f.z * f.z);
        const float fx = flat > 1e-4f ? f.x / flat : 0.0f;
        const float fz = flat > 1e-4f ? f.z / flat : 0.0f;
        // right = normalize(forward × worldUp) = (-f.z, 0, f.x)
        GVec3 move{};
        move.x = fx * in_fwd - fz * in_side;
        move.y = in_up;
        move.z = fz * in_fwd + fx * in_side;
        const float len = std::sqrt(move.x * move.x + move.y * move.y + move.z * move.z);
        if (len <= 1e-4f) return false;

        const float sprint = window.get_key(kKeyLeftShift) ? 2.5f : 1.0f;
        const float scale = speed * sprint * dt / len;
        position.x += move.x * scale;
        position.y += move.y * scale;
        position.z += move.z * scale;
        GEntity_SetLocalPosition(entity, &position);
        return true;
    }
};

// ---------------------------------------------------------------------------
// project.data — 与参考工程一致的运行时设置（缺失时用引擎默认值）
// ---------------------------------------------------------------------------
struct ProjectSettings {
    std::string title = "Gryce Renderer";
    int width = 1280;
    int height = 720;
    std::string render_api = "opengl";
    std::string main_scene = "res:/scenes/main.gesc";
    bool hdr = true;
    int tone_map_mode = 1;
    float exposure = 1.0f;
    bool shadow_enabled = true;
    int shadow_map_size = 2048;
    float ambient[3] = {0.2f, 0.22f, 0.26f};
    float ibl_intensity = 1.0f;
    bool default_environment = false;
    bool ssao_enabled = false;
    bool ssr_enabled = false;
    bool rt_reflection_enabled = false;
    bool rt_transmission_enabled = false;
    bool rt_gi_enabled = false;
    bool ssil_enabled = false;
    float ssr_max_steps = 64.0f;
    float ssr_max_roughness = 0.6f;
    float ssr_thickness = 0.1f;
    float ssr_bilateral = 0.5f;
    float ssr_env_fallback = 1.0f;      // 未命中回退到 IBL/探针的强度（1=替换）
    float ssr_resolution_scale = 1.0f;  // SSR 内部渲染分辨率缩放
    bool pcss_enabled = false;          // 接触硬化软阴影
    float pcss_light_size = 0.06f;
    float pcss_max_radius = 12.0f;
    bool loaded = false;
};

ProjectSettings load_project_settings(const std::string& project_root) {
    ProjectSettings s;
    const std::filesystem::path path =
        std::filesystem::path(project_root) / "project.data";
    std::ifstream in(path);
    if (!in) return s;
    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("name") && j["name"].is_string()) s.title = j["name"].get<std::string>();
        if (j.contains("window") && j["window"].is_object()) {
            s.width = j["window"].value("width", s.width);
            s.height = j["window"].value("height", s.height);
            if (j["window"].contains("title") && j["window"]["title"].is_string()) {
                s.title = j["window"]["title"].get<std::string>();
            }
        }
        s.render_api = j.value("render_api", s.render_api);
        // 配置里的值常被手改成带首尾空白（如 "vulkan "），会与后端名比对失败，这里去掉
        {
            const std::string& v = s.render_api;
            const size_t b = v.find_first_not_of(" \t\r\n");
            const size_t e = v.find_last_not_of(" \t\r\n");
            s.render_api = (b == std::string::npos) ? std::string{} : v.substr(b, e - b + 1);
        }
        s.main_scene = j.value("main_scene", j.value("entry_scene", s.main_scene));
        s.hdr = j.value("hdr", s.hdr);
        s.tone_map_mode = j.value("tone_map_mode", s.tone_map_mode);
        s.exposure = j.value("exposure", s.exposure);
        s.shadow_enabled = j.value("shadow_enabled", s.shadow_enabled);
        s.shadow_map_size = j.value("shadow_map_size", s.shadow_map_size);
        s.ambient[0] = j.value("ambient_r", s.ambient[0]);
        s.ambient[1] = j.value("ambient_g", s.ambient[1]);
        s.ambient[2] = j.value("ambient_b", s.ambient[2]);
        s.ibl_intensity = j.value("ibl_intensity", s.ibl_intensity);
        s.default_environment = j.value("default_environment", s.default_environment);
        s.ssao_enabled = j.value("ssao_enabled", s.ssao_enabled);
        s.ssr_enabled = j.value("ssr_enabled", s.ssr_enabled);
        s.rt_reflection_enabled = j.value("rt_reflection_enabled", s.rt_reflection_enabled);
        s.rt_transmission_enabled = j.value("rt_transmission_enabled", s.rt_transmission_enabled);
        s.rt_gi_enabled = j.value("rt_gi_enabled", s.rt_gi_enabled);
        s.ssil_enabled = j.value("ssil_enabled", s.ssil_enabled);
        s.ssr_max_steps = j.value("ssr_max_steps", s.ssr_max_steps);
        s.ssr_max_roughness = j.value("ssr_max_roughness", s.ssr_max_roughness);
        s.ssr_thickness = j.value("ssr_thickness", s.ssr_thickness);
        s.ssr_bilateral = j.value("ssr_bilateral_filter", s.ssr_bilateral);
        s.ssr_env_fallback = j.value("ssr_env_fallback", s.ssr_env_fallback);
        s.ssr_resolution_scale = j.value("ssr_resolution_scale", s.ssr_resolution_scale);
        s.pcss_enabled = j.value("pcss_enabled", s.pcss_enabled);
        s.pcss_light_size = j.value("pcss_light_size", s.pcss_light_size);
        s.pcss_max_radius = j.value("pcss_max_radius", s.pcss_max_radius);
        s.loaded = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[smoke] project.data parse failed: %s\n", e.what());
    }
    return s;
}

const char* short_type_name(const char* full) {
    static thread_local std::string buf;
    std::string n = full ? full : "";
    const auto p = n.rfind("::");
    buf = (p == std::string::npos) ? n : n.substr(p + 2);
    return buf.c_str();
}

// 场景清单：确认主场景真的把相机/灯光/网格载进来了
struct SceneStats {
    int entities = 0;
    int cameras = 0;
    int lights = 0;
    int meshes = 0;
};

SceneStats scan_scene() {
    SceneStats st;
    const int count = GEntity_GetCount();
    if (count <= 0) return st;
    st.entities = count;
    for (int i = 0; i < count; ++i) {
        const GEntityHandle e = GEntity_GetAt(i);
        if (e <= 0) continue;
        const int cc = GComponent_GetCount(e);
        for (int c = 0; c < cc; ++c) {
            char name[128] = {};
            if (GComponent_GetTypeNameAt(e, c, name, (int)sizeof(name)) < 0) continue;
            const std::string t = short_type_name(name);
            if (t == "Camera") ++st.cameras;
            else if (t == "Light") ++st.lights;
            else if (t == "MeshRenderer") ++st.meshes;
        }
    }
    return st;
}

// 截图统计：判定"真的画出东西了"而不是一片清屏色。
struct ImageStats {
    bool ok = false;
    int width = 0;
    int height = 0;
    double mean[3] = {0.0, 0.0, 0.0};
    double non_dominant_ratio = 0.0;   // 与主色调相差较远的像素占比
    int unique_bins = 0;
};

// 裁剪一块区域并最近邻放大，用来肉眼核对文字边缘的灰度台阶（抗锯齿质量）。
// 双线性放大自己会制造平滑假象，所以必须用最近邻，让原始像素块保持方正。
bool write_zoomed_region(const std::string& src, const std::string& dst,
                         int rx, int ry, int rw, int rh, int scale) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(src.c_str(), &w, &h, &channels, 4);
    if (!pixels) return false;
    if (scale < 1) scale = 1;
    rx = std::max(0, std::min(rx, w - 1));
    ry = std::max(0, std::min(ry, h - 1));
    rw = std::max(1, std::min(rw, w - rx));
    rh = std::max(1, std::min(rh, h - ry));

    const int ow = rw * scale;
    const int oh = rh * scale;
    std::vector<unsigned char> out(static_cast<std::size_t>(ow) * oh * 4);
    for (int y = 0; y < oh; ++y) {
        const int sy = ry + y / scale;
        for (int x = 0; x < ow; ++x) {
            const int sx = rx + x / scale;
            std::memcpy(&out[(static_cast<std::size_t>(y) * ow + x) * 4],
                        &pixels[(static_cast<std::size_t>(sy) * w + sx) * 4], 4);
        }
    }
    stbi_image_free(pixels);
    return stbi_write_png(dst.c_str(), ow, oh, 4, out.data(), ow * 4) != 0;
}

ImageStats analyze_image(const std::string& path) {
    ImageStats st;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 3);
    if (!pixels) return st;
    st.ok = true;
    st.width = w;
    st.height = h;

    const long long total = static_cast<long long>(w) * h;
    long long sum[3] = {0, 0, 0};
    std::unordered_map<unsigned int, long long> histogram;
    histogram.reserve(4096);
    for (long long i = 0; i < total; ++i) {
        const unsigned char* px = pixels + i * 3;
        sum[0] += px[0];
        sum[1] += px[1];
        sum[2] += px[2];
        const unsigned int bin =
            (static_cast<unsigned int>(px[0] >> 3) << 10) |
            (static_cast<unsigned int>(px[1] >> 3) << 5) |
            static_cast<unsigned int>(px[2] >> 3);
        ++histogram[bin];
    }
    stbi_image_free(pixels);

    for (int c = 0; c < 3; ++c) st.mean[c] = static_cast<double>(sum[c]) / total;
    long long dominant = 0;
    for (const auto& [bin, n] : histogram) {
        (void)bin;
        if (n > dominant) dominant = n;
    }
    st.unique_bins = static_cast<int>(histogram.size());
    st.non_dominant_ratio = 1.0 - static_cast<double>(dominant) / total;
    return st;
}

void print_usage() {
    std::printf(
        "GryceRenderSmoke — Gryce 渲染器双后端冒烟/演示程序\n"
        "  --api <opengl|vulkan|dx12>  渲染后端（默认取 project.data 的 render_api）\n"
        "  --project <dir>        项目根（默认编译期内置的 demo_project）\n"
        "  --scene <res:/...>     覆盖起始场景\n"
        "  --frames <N>           渲染帧数后截图退出（默认 60）\n"
        "  --shot <png>           截图输出路径（默认 shot_<api>.png）\n"
        "  --w / --h <px>         窗口与视口尺寸（默认取 project.data）\n"
        "  --sync                 同步渲染模式（不启动渲染线程）\n"
        "  --no-vsync             关闭垂直同步（性能测量用，默认打开）\n"
        "  --resolution WxH       设置渲染分辨率（等价 --w W --h H）\n"
        "  --ssr-scale <0.25..1>  SSR 内部渲染分辨率缩放（0.5 约省 3/4 SSR 开销）\n"
        "  --ssr-env-fallback <0..1>  SSR 未命中回退 IBL/探针的强度（默认 1=替换）\n"
        "  --pcss / --no-pcss     接触硬化软阴影开关（默认取 project.data）\n"
        "  --debug-view <0..8>    调试视图：5=阴影因子 6=直接光 7=环境光 8=级联着色\n"
        "  --fps / --no-fps       左上角白色 FPS 叠加（默认开）\n"
        "  --fps-limit <N>        软件锁帧：帧率上限 N（0 = 不限制；与 --vsync 互补）\n"
        "  --no-debug-ui          不画调试 UI（左上角读数 + ImGui 调试窗口）\n"
        "  --ui-interval <N>      UI 帧节流：调试面板每 N 个渲染帧才构建一次（默认 1）\n"
        "  --editor-preview       走编辑器预览路径：离屏视口输出 + 双视口 + Gizmo 命中测试\n"
        "  --ui-demo              注入 GryceUI 基础控件演示\n"
        "  --ui-gallery [--theme X] [--gallery-all] [--gallery-scroll N]  注入全组件画廊 + 多主题切换\n"
        "  --resize-probe                                                 运行中改窗口尺寸（核对不黑屏）\n"
        "  --click-probe                                                  合成一次点击（核对输入链路）\n"
        "  --bench [N]            开启 GPU 分段计时，跑 N 帧（默认 240）后打印每 pass 平均 GPU 时间\n"
        "  --ssr / --no-ssr       覆盖屏幕空间反射开关（A/B 对照用）\n"
        "  --ssao / --no-ssao     覆盖屏幕空间环境光遮蔽开关\n"
        "  --ssil / --no-ssil     覆盖屏幕空间间接光开关\n"
        "  --no-analyze           跳过截图统计\n"
        "  --no-gpu-cull          关闭 GPU 视锥剔除（A/B 对照用）\n"
        "  --no-occlusion         关闭 HiZ 遮挡剔除（A/B 对照用）\n"
        "  --msaa <N>             MSAA 采样数（前向 HDR pass；默认 4，--no-msaa 关闭）\n"
        "  --power-saver          强制开启省电模式（低分辨率 HDR + 跳过后处理）\n"
        "  --no-power-saver       强制关闭省电模式（A/B 对照用）\n"
        "  --list                 打印场景清单后退出\n"
        "  --simulate             进入 Play 模式驱动组件 on_update（粒子/动画），但不接管相机、不锁鼠标\n"
        "  --play                 交互模式：鼠标锁定转视角，WASD 移动，Esc 退出\n"
        "  --speed <m/s>          交互模式移动速度（默认 3.0）\n"
        "  --sensitivity <度/像素> 交互模式鼠标灵敏度（默认 0.12）\n"
        "  --play-frames <N>      交互模式运行 N 帧后自动退出（自检用）\n"
        "  --stress <N>           生成 N 个动态立方体的重负载场景（性能基准用）。\n"
        "  --instanced <N>        注入 1 个 InstancedMeshRenderer（N 个实例），用于\n"
        "                         验证 GPU 剔除（compute + 间接绘制）的 A/B 对照。\n"
        "                         与 --scene 互斥；生成后替换当前场景内容。\n"
        "  --stress-shadow        压力场景加载 N 个投影体（默认全开阴影投射）\n");
}

// ---------------------------------------------------------------------------
// 压力场景生成器（--stress）
//
// 目的：demo 场景只有 9 实体 / 5 mesh，阴影、合批、剔除、CPU 遍历这些开销
// 全被稀释到噪声里，量不出真实优化空间。这里用 GEntity_ImportJson 批量灌入
// 实体 —— 它走的是与场景文件完全相同的 SceneSerializer::deserialize 路径，
// 因此生成的实体与手写 .gesc 在引擎里没有任何区别。
//
// 布局：一条沿 -Z 延伸的立方体"走廊"，每个立方体都投阴影（这是关键：
// 阴影 pass 的绘制次数 = 投影体数量，是 CPU 侧 O(N) 遍历 + GPU 侧几何提交的
// 双重压力源）。地面用一个大 quad 承接所有阴影，保证接收端像素覆盖满屏。
//
// 注意：必须在 GRender_Init 之后调用（材质/网格资源此时才可解析）。
// ---------------------------------------------------------------------------
struct StressReport {
    int entities = 0;   // 成功灌入的实体数
    int failed = 0;     // 导入失败的批次数
};

StressReport generate_stress_scene(int count) {
    StressReport rep;
    if (count <= 0) return rep;

    // 立方体边长与间距：间距略大于边长，避免互相穿模影响视觉判读，
    // 同时保证有大量重叠投影（阴影 pass 的 overdraw 压力）。
    const float kCubeSize = 1.0f;
    const float kSpacing = 1.6f;
    // 每行多少个：排成方阵，中心对齐原点
    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const float half = (side - 1) * 0.5f * kSpacing;

    // ---- 地面（承接阴影；单个大 quad，保证接收端满屏覆盖）----
    {
        const float ground_extent = std::max(40.0f, side * kSpacing + 20.0f);
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            R"({"version":2,"name":"StressGround","entities":[{
              "name":"StressGround","parent":null,"enabled":true,
              "transform":{"position":[0,0,0],"rotation":[0,0,0,1],
                           "scale":[%.3f,1.0,%.3f]},
              "components":[{"type":"MeshRenderer","enabled":true,
                "mesh_path":"res:/models/quad.obj","billboard":false,
                "depth_test":true,"material":{"name":"StressGroundMat",
                "preset_name":"Concrete"}}]}]})",
            static_cast<double>(ground_extent), static_cast<double>(ground_extent));
        if (GEntity_ImportJson(buf, 0) == 0) ++rep.failed;
    }

    // ---- N 个立方体，一次一个 JSON（批量拼字符串会在大 N 下爆内存）----
    // 交替使用几种材质预设，制造不同 roughness/metalic，避免被合批成一次
    // instanced draw —— 我们要的是"每个都是独立 draw call"的 CPU 压力。
    static const char* kPresets[] = {"Concrete", "Gold", "Chrome", "Wood"};
    int idx = 0;
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            if (idx >= count) break;
            const float px = static_cast<float>(x) * kSpacing - half;
            const float pz = static_cast<float>(z) * kSpacing - half;
            // 高度错落：让阴影层次可见，也避免完全规整的图案
            const float py = kCubeSize * 0.5f + 0.12f * static_cast<float>(idx % 5);
            // 各自的 Y 轴旋转：避免被实例化合批，同时让投影形状多样
            const float ang = static_cast<float>(idx) * 0.37f;
            const float half_ang = ang * 0.5f;
            const float qy = std::sin(half_ang);
            const float qw = std::cos(half_ang);
            const char* preset = kPresets[idx % 4];

            char buf[768];
            std::snprintf(buf, sizeof(buf),
                R"({"version":2,"name":"StressCube%d","entities":[{
                  "name":"StressCube%d","parent":null,"enabled":true,
                  "transform":{"position":[%.4f,%.4f,%.4f],
                               "rotation":[0,%.6f,0,%.6f],
                               "scale":[%.3f,%.3f,%.3f]},
                  "components":[{"type":"MeshRenderer","enabled":true,
                    "mesh_path":"res:/models/cube.obj","billboard":false,
                    "depth_test":true,"material":{"name":"StressMat%d",
                    "preset_name":"%s"}}]}]})",
                idx, idx,
                static_cast<double>(px), static_cast<double>(py), static_cast<double>(pz),
                static_cast<double>(qy), static_cast<double>(qw),
                static_cast<double>(kCubeSize), static_cast<double>(kCubeSize),
                static_cast<double>(kCubeSize),
                idx, preset);

            if (GEntity_ImportJson(buf, 0) != 0) {
                ++rep.entities;
            } else {
                ++rep.failed;
            }
            ++idx;
        }
        if (idx >= count) break;
    }
    return rep;
}

// ---------------------------------------------------------------------------
// 实例化场景注入器（--instanced N）
//
// 为什么需要它：GPU 剔除（compute 视锥剔除 + HiZ + 间接绘制）只在
// **InstancedMeshRenderer 且实例数 ≥ 32** 时才启用（见
// RenderPipeline::collect_instanced 的 k_min_gpu_cull_instances）。而 demo 场景
// 用 MeshRenderer、--stress 也刻意用 MeshRenderer 打散合批 —— 于是这条路径在
// 现有场景里**一次都跑不到**，"开关能编译、功能是死代码"。
//
// 这里灌一个 InstancedMeshRenderer 实体（走与 .gesc 完全相同的
// SceneSerializer 路径），实例按 seed 确定性散布成一片，用来做
// 「--no-gpu-cull A/B」的正确性对照：两条路径的画面应当**完全一致**。
// ---------------------------------------------------------------------------
bool spawn_instanced_scene(int count) {
    if (count <= 0) return false;
    char buf[768];
    std::snprintf(buf, sizeof(buf),
        R"({"version":2,"name":"InstancedField","entities":[{
          "name":"InstancedField","parent":null,"enabled":true,
          "transform":{"position":[0,1.0,0],"rotation":[0,0,0,1],
                       "scale":[1.0,1.0,1.0]},
          "components":[{"type":"InstancedMeshRenderer","enabled":true,
            "mesh_path":"res:/models/cube.obj","material_path":"",
            "instance_count":%d,"spacing":1.4,"seed":7,
            "material":{"name":"InstancedMat","preset_name":"Gold"}}]}]})",
        count);
    return GEntity_ImportJson(buf, 0) != 0;
}

} // namespace

#ifdef __EMSCRIPTEN__
// 浏览器常驻循环的状态。emscripten_set_main_loop_arg 靠异常展开 main 的栈，
// 所以循环能碰到的东西必须是堆对象（见 main 里 Window/FpsCamera 的分配）。
struct WebLoopState {
    Window* window = nullptr;
    FpsCamera* camera = nullptr;
    bool camera_ready = false;
};

// 每帧一步：requestAnimationFrame 驱动，dt 取墙钟（钳到 [0.5ms, 100ms]，
// 标签页切回前台时不会因为累积的大间隔跳一大步）。
void web_render_step(void* arg) {
    WebLoopState* state = static_cast<WebLoopState*>(arg);
    Window& window = *state->window;
    window.poll_events();

    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    dt = std::max(0.0005f, std::min(dt, 0.1f));

    // Esc 在浏览器里只代表释放指针锁定，不结束循环（没有窗口可关）。
    if (state->camera_ready) state->camera->update(window, dt);

    GCore_BeginFrame(dt);
    GRender_BeginFrame();
    GRender_RenderWorld();
    GRender_RenderGizmo();
    GRender_EndFrame();
    GCore_EndFrame();
}
#endif

int main(int argc, char** argv) {
    // 排障用：stdout 默认是块缓冲，进程崩溃（尤其 D3D12 侧访问违规）时
    // printf 的路标会整块丢掉，只剩 GLOG 的行 —— 无法定位崩溃发生在哪个阶段。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    // 纯逻辑自测：不需要窗口/GPU，先于一切初始化返回。
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--code-selftest") == 0) return run_code_selftest();
        if (std::strcmp(argv[i], "--fluent-selftest") == 0) return run_fluent_selftest();
    }


    std::string project_root = GRYCE_DEMO_PROJECT_ROOT;
    std::string scene_override;
    int frames = 60;
    int shot_delay = 4;             // 请求截图后再驱动的帧数（等渲染线程写盘）
    std::string shot_path;
    // --shot-zoom x,y,w,h,scale：截图后裁一块区域最近邻放大另存 <shot>.zoom.png
    std::string shot_zoom_arg;
    std::string api_arg;
    int width_override = 0;
    int height_override = 0;
    bool sync_mode = false;
    bool analyze = true;
    bool list_only = false;
    int ssr_override = -1;    // -1 = 跟随 project.data
    int ssao_override = -1;
    int ssil_override = -1;
    int env_override = -1;
    int msaa_override = -1;   // -1 = 默认 4x；0 = 关闭；N = 指定采样数
    bool play_mode = false;
    // --simulate：只把引擎切进 Play 模式（World::update 生效，组件 on_update 跑起来），
    // 但不把相机交给 FpsCamera，也不锁鼠标。用于脚本/CI 验证粒子、动画这类依赖
    // on_update 的效果：不切 Play 模式它们全程静止，截出来的图里根本没有粒子。
    bool simulate_mode = false;
    // --move：每帧平移主相机（不接管输入），用于复现"相机移动"路径下的开销。
    // 反射探针在相机位移时会高频重烘全场景，静止相机的基准测不到这部分。
    bool move_cam = false;
    // --cam-pos / --cam-look：覆盖主相机位置与朝向（不接管输入），用于复现
    // "面向粒子/背向粒子"这类视角相关的开销差异。
    bool cam_override = false;
    GVec3 cam_pos_override{};
    GVec3 cam_look_override{};
    int bench_frames = 0;   // >0：开启 GPU 分段计时，跑满 N 帧后打印每 pass 平均 GPU 时间
    float play_speed = 3.0f;
    float play_sensitivity = 0.12f;
    int play_frames = 0;   // >0：交互模式跑满 N 帧自动退出（脚本/自检用）
    // --stress：生成 N 个动态立方体的重负载场景，用于把 demo 轻场景里
    // 被稀释的阴影/合批/剔除/CPU 遍历开销暴露出来。0 = 不生成。
    int stress_count = 0;
    bool stress_shadow = false;   // 保留：投影体默认全开，此项仅用于日志标注
    int instanced_count = 0;      // >0：注入一个 InstancedMeshRenderer（GPU 剔除验证用）
    bool frames_explicit = false;
    bool shot_explicit = false;   // 用户显式给了 --shot（决定 UI 预览是否常驻）
    bool vsync = true;   // 默认开垂直同步；--no-vsync 用于性能测量
    float ssr_env_fallback_override = -1.0f;
    float ssr_scale_override = -1.0f;
    int debug_view_override = -1;
    int pcss_override = -1;
    bool fps_overlay = true;   // 左上角白色 FPS，默认开
    int  ui_interval_override = 0;   // --ui-interval N，<=0 不干预（默认每帧构建 UI）
    int  fps_limit = -1;       // --fps-limit N（<0 表示不设置，保持默认 0 = 不限速）
    bool debug_ui_enabled = true;   // --no-debug-ui 关闭调试 UI
    bool ui_demo = false;
    bool ui_gallery = false;
    bool menu_open_demo = false;   // --menu-open：注入时展开 File 菜单（截图核对浮层）
    bool gallery_all = false;   // --gallery-all：拉高滚动区填满帧缓冲，配合 --gallery-scroll 翻页
    int gallery_scroll = 0;    // --gallery-scroll N：初始滚动像素（无头翻页截图用）
    bool resize_probe = false; // --resize-probe：运行中改窗口尺寸，核对不黑屏
    bool click_probe = false;  // --click-probe：合成一次点击，核对输入链路
    float probe_x = 0.0f;
    float probe_y = 0.0f;
    bool ui_codeeditor = false;       // --ui-codeeditor：无场景代码编辑器演示
    std::string code_find = "render"; // --code-find <q>：查找高亮查询词
    std::string code_lang = "glsl";   // --code-lang <lang>：高亮语言
    float code_font_size = 16.0f;     // --code-font <px>：编辑器字号
    float font_shape = 1.0f;          // --font-shape <k>：字形整形强度（0 关闭，1 标准，>1 更实）
    std::string theme_arg = "dark";
    // --editor-preview：编辑器前置能力的运行时冒烟。开启后 tonemap 写离屏视口目标
    // 而不是默认帧缓冲（窗口本身会是黑的，这是编辑器预览的正常形态），并额外
    // 渲染一次 GameView、跑一次 Gizmo 命中测试。
    bool editor_preview = false;
    bool preview_ok = true;
    // -1 = 不干预（用引擎默认）；0/1 = 强制关/开。用于 A/B 对照优化开销。
    int gpu_cull_override = -1;
    int occlusion_override = -1;
    int power_saver_override = -1;   // -1=跟随 auto 判定；1=强制开；0=强制关
    int ssr_debug_override = 0;  // --ssr-debug：SSR 调试视图

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[smoke] %s 缺少参数\n", flag);
                std::exit(1);
            }
            return argv[++i];
        };
        auto need_vec3 = [&](const char* flag, GVec3& out) {
            if (i + 3 >= argc) {
                std::fprintf(stderr, "[smoke] %s 需要 3 个数值\n", flag);
                std::exit(1);
            }
            out.x = static_cast<float>(std::atof(argv[++i]));
            out.y = static_cast<float>(std::atof(argv[++i]));
            out.z = static_cast<float>(std::atof(argv[++i]));
        };
        if (std::strcmp(a, "--api") == 0) api_arg = need_value(a);
        else if (std::strcmp(a, "--project") == 0) project_root = need_value(a);
        else if (std::strcmp(a, "--scene") == 0) scene_override = need_value(a);
        else if (std::strcmp(a, "--frames") == 0) {
            frames = std::atoi(need_value(a));
            frames_explicit = true;
        }
        else if (std::strcmp(a, "--shot") == 0) { shot_path = need_value(a); shot_explicit = true; }
        else if (std::strcmp(a, "--shot-zoom") == 0) shot_zoom_arg = need_value(a);        else if (std::strcmp(a, "--w") == 0) width_override = std::atoi(need_value(a));
        else if (std::strcmp(a, "--h") == 0) height_override = std::atoi(need_value(a));
        else if (std::strcmp(a, "--sync") == 0) sync_mode = true;
        else if (std::strcmp(a, "--vsync") == 0) vsync = true;
        else if (std::strcmp(a, "--no-vsync") == 0) vsync = false;
        // SSR：未命中回退强度 / 内部分辨率缩放
        else if (std::strcmp(a, "--ssr-env-fallback") == 0 ||
                 std::strcmp(a, "--ssr-fallback") == 0) {
            ssr_env_fallback_override = static_cast<float>(std::atof(need_value(a)));
        }
        else if (std::strcmp(a, "--ssr-scale") == 0) {
            ssr_scale_override = static_cast<float>(std::atof(need_value(a)));
        }
        else if (std::strcmp(a, "--debug-view") == 0 || std::strcmp(a, "--debug") == 0) {
            debug_view_override = std::atoi(need_value(a));
        }
        else if (std::strcmp(a, "--pcss") == 0) pcss_override = 1;
        else if (std::strcmp(a, "--no-pcss") == 0) pcss_override = 0;
        else if (std::strcmp(a, "--no-gpu-cull") == 0) gpu_cull_override = 0;
        else if (std::strcmp(a, "--no-occlusion") == 0) occlusion_override = 0;
        // 省电模式：forced 语义是"手动覆盖 auto 判定"，因此三态（默认 -1 不干预 /
        // 1 强制开 / 0 强制关）都映射到同一个 API。
        else if (std::strcmp(a, "--power-saver") == 0) power_saver_override = 1;
        else if (std::strcmp(a, "--no-power-saver") == 0) power_saver_override = 0;
        else if (std::strcmp(a, "--fps") == 0) fps_overlay = true;
        else if (std::strcmp(a, "--no-fps") == 0) fps_overlay = false;
        // --fps-limit N：软件锁帧（帧率上限），0 = 不限制。与 --vsync 互补。
        else if (std::strcmp(a, "--fps-limit") == 0) fps_limit = std::atoi(need_value(a));
        // --no-debug-ui：完全不画调试 UI（关掉左上角读数与 ImGui 调试窗口），
        // 用于"纯画面"截图比对（UI 会让两张图必然不同）。
        else if (std::strcmp(a, "--no-debug-ui") == 0) debug_ui_enabled = false;
        // --ui-interval N：UI 帧节流，调试面板每 N 个渲染帧才构建一次
        // （中间的帧重绘上一份 draw data）。N<=1 = 每帧构建（默认）。
        else if (std::strcmp(a, "--ui-interval") == 0)
            ui_interval_override = std::atoi(need_value(a));
        else if (std::strcmp(a, "--editor-preview") == 0) editor_preview = true;
        else if (std::strcmp(a, "--ui-demo") == 0) ui_demo = true;
        else if (std::strcmp(a, "--ui-gallery") == 0) ui_gallery = true;
        else if (std::strcmp(a, "--ui-codeeditor") == 0) ui_codeeditor = true;
        else if (std::strcmp(a, "--code-find") == 0) code_find = need_value(a);
        else if (std::strcmp(a, "--code-lang") == 0) code_lang = need_value(a);
        else if (std::strcmp(a, "--code-font") == 0) code_font_size = static_cast<float>(std::atof(need_value(a)));
        else if (std::strcmp(a, "--font-shape") == 0) font_shape = static_cast<float>(std::atof(need_value(a)));
        else if (std::strcmp(a, "--gallery-all") == 0) gallery_all = true;
        else if (std::strcmp(a, "--menu-open") == 0) menu_open_demo = true;
        else if (std::strcmp(a, "--gallery-scroll") == 0) gallery_scroll = std::atoi(need_value(a));
        else if (std::strcmp(a, "--resize-probe") == 0) resize_probe = true;
        else if (std::strcmp(a, "--click-probe") == 0) click_probe = true;
        else if (std::strcmp(a, "--theme") == 0) theme_arg = need_value(a);
        else if (std::strcmp(a, "--ssr-debug") == 0) ssr_debug_override = std::atoi(need_value(a));
        else if (std::strcmp(a, "--resolution") == 0) {
            // --resolution 1920x1080 等价于 --w 1920 --h 1080
            const char* v = need_value(a);
            int rw = 0, rh = 0;
            if (std::sscanf(v, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
                width_override = rw;
                height_override = rh;
            } else {
                std::fprintf(stderr, "[smoke] --resolution 需要 WxH 格式（如 1920x1080）\n");
            }
        }
        // 屏幕空间效果覆盖（A/B 对照测试用）
        else if (std::strcmp(a, "--ssr") == 0) ssr_override = 1;
        else if (std::strcmp(a, "--no-ssr") == 0) ssr_override = 0;
        else if (std::strcmp(a, "--ssao") == 0) ssao_override = 1;
        else if (std::strcmp(a, "--no-ssao") == 0) ssao_override = 0;
        else if (std::strcmp(a, "--ssil") == 0) ssil_override = 1;
        else if (std::strcmp(a, "--no-ssil") == 0) ssil_override = 0;
        else if (std::strcmp(a, "--env") == 0) env_override = 1;
        else if (std::strcmp(a, "--no-env") == 0) env_override = 0;
        // MSAA：--msaa N / --no-msaa（A/B 对照用）。0 = 关闭。
        else if (std::strcmp(a, "--msaa") == 0) msaa_override = std::atoi(need_value(a));
        else if (std::strcmp(a, "--no-msaa") == 0) msaa_override = 0;
        else if (std::strcmp(a, "--simulate") == 0) simulate_mode = true;
        else if (std::strcmp(a, "--move") == 0) move_cam = true;
        else if (std::strcmp(a, "--cam-pos") == 0) {
            need_vec3(a, cam_pos_override);
            cam_override = true;
        }
        else if (std::strcmp(a, "--cam-look") == 0) {
            need_vec3(a, cam_look_override);
            cam_override = true;
        }
        else if (std::strcmp(a, "--play") == 0 || std::strcmp(a, "--interactive") == 0 ||
                 std::strcmp(a, "-i") == 0) play_mode = true;
        else if (std::strcmp(a, "--speed") == 0) play_speed = static_cast<float>(std::atof(need_value(a)));
        else if (std::strcmp(a, "--sensitivity") == 0) {
            play_sensitivity = static_cast<float>(std::atof(need_value(a)));
        }
        else if (std::strcmp(a, "--play-frames") == 0) play_frames = std::atoi(need_value(a));
        else if (std::strcmp(a, "--stress") == 0) stress_count = std::atoi(need_value(a));
        else if (std::strcmp(a, "--instanced") == 0) instanced_count = std::atoi(need_value(a));
        else if (std::strcmp(a, "--stress-shadow") == 0) stress_shadow = true;
        else if (std::strcmp(a, "--bench") == 0) {
            // 可选帧数：--bench 200；不跟数字时用默认 240 帧
            if (i + 1 < argc && argv[i + 1][0] != '-') bench_frames = std::atoi(argv[++i]);
            else bench_frames = 240;
        }
        else if (std::strcmp(a, "--no-analyze") == 0) analyze = false;
        else if (std::strcmp(a, "--list") == 0) list_only = true;
        else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "[smoke] 未知参数: %s\n", a);
            print_usage();
            return 1;
        }
    }

    // 1) 项目设置
    ProjectSettings cfg = load_project_settings(project_root);
    if (cfg.loaded) {
        std::printf("[smoke] project '%s' (hdr=%d tone_map=%d exposure=%.2f shadow=%d/%d "
                    "ambient=%.2f/%.2f/%.2f ibl=%.2f)\n",
                    project_root.c_str(), cfg.hdr ? 1 : 0, cfg.tone_map_mode,
                    static_cast<double>(cfg.exposure), cfg.shadow_enabled ? 1 : 0,
                    cfg.shadow_map_size, static_cast<double>(cfg.ambient[0]),
                    static_cast<double>(cfg.ambient[1]), static_cast<double>(cfg.ambient[2]),
                    static_cast<double>(cfg.ibl_intensity));
    } else {
        std::printf("[smoke] no project.data at '%s' — using engine defaults\n",
                    project_root.c_str());
    }

    if (api_arg.empty()) api_arg = cfg.render_api;
    GRenderAPI api = GRYCE_RENDER_API_OPENGL;
    if (api_arg == "vulkan") api = GRYCE_RENDER_API_VULKAN;
    else if (api_arg == "dx12") api = GRYCE_RENDER_API_DX12;
    else if (api_arg == "webgl") api = GRYCE_RENDER_API_WEBGL;
    else if (api_arg != "opengl" && !api_arg.empty()) {
        std::fprintf(stderr, "[smoke] 未知后端 '%s'（用 opengl、vulkan、dx12 或 webgl）\n", api_arg.c_str());
        return 1;
    }
    const bool use_vulkan = (api == GRYCE_RENDER_API_VULKAN);
    const bool use_dx12   = (api == GRYCE_RENDER_API_DX12);
    const bool use_webgl  = (api == GRYCE_RENDER_API_WEBGL);
    const bool use_opengl = (api == GRYCE_RENDER_API_OPENGL);
    const char* backend_name =
        use_vulkan ? "vulkan" : use_dx12 ? "dx12" : use_webgl ? "webgl" : "opengl";
    int width = width_override > 0 ? width_override : cfg.width;
    int height = height_override > 0 ? height_override : cfg.height;
    if (shot_path.empty()) {
        shot_path = use_vulkan ? "shot_vulkan.png"
                  : use_dx12   ? "shot_dx12.png"
                  : use_webgl  ? "shot_webgl.png"
                               : "shot_opengl.png";
    }

    // 2) 平台 + Core（载入 <project>/project.data 与主场景）
    if (!Window::init_sdk()) {
        std::fprintf(stderr, "[smoke] failed to init GLFW SDK\n");
        return 1;
    }

    // WebGL 归入 OpenGL 分支：Emscripten 的 GLFW 端口建的就是 WebGL context
    const WindowContextType ctx =
        (use_opengl || use_webgl) ? WindowContextType::OpenGL : WindowContextType::NoApi;
#ifdef __EMSCRIPTEN__
    // 浏览器用 set_main_loop 常驻循环，它会展开 main 的栈；栈上的 Window 析构
    // 会销毁 canvas，所以这里放堆上（页面生命周期内不回收）。
    Window& window = *new Window(cfg.title.c_str(), width, height, WindowMode::Windowed, ctx);
#else
    Window window(cfg.title.c_str(), width, height, WindowMode::Windowed, ctx);
#endif
    if (!window.is_valid()) {
        std::fprintf(stderr, "[smoke] failed to create window\n");
        Window::shutdown_sdk();
        return 1;
    }

#ifdef __EMSCRIPTEN__
    // 平台层已把画布缓冲同步成页面尺寸（CSS 决定的显示尺寸），渲染分辨率
    // 必须以实际缓冲为准，否则窗口尺寸与显示尺寸不一致 —— 画面被浏览器拉伸、
    // 指针换算也会偏。
    {
        int fb_w = 0;
        int fb_h = 0;
        window.get_framebuffer_size(fb_w, fb_h);
        if (fb_w > 0 && fb_h > 0) {
            width = fb_w;
            height = fb_h;
        }
    }
#endif

    std::printf("[smoke] backend=%s frames=%d viewport=%dx%d sync=%d\n",
                backend_name, frames, width, height, sync_mode ? 1 : 0);

    GCoreInitDesc core_desc{};
    core_desc.version = sizeof(GCoreInitDesc);
    core_desc.project_root = project_root.c_str();
    core_desc.enable_reflection = true;
    // UI 预览模式（--ui-gallery / --ui-demo / --ui-codeeditor）：只关心界面，
    // 不加载项目主场景。否则每帧都要跑一遍 PBR + 阴影 + 后处理，1080p 上就是
    // 100ms/帧，界面跟着鼠标都拖不动，而这些开销对你正在看的控件毫无意义。
    // 显式给了 --scene 就说明要看 3D，退回完整管线。
    const bool ui_only = (ui_gallery || ui_demo || ui_codeeditor) && scene_override.empty();
    GCore_SetAutoLoadMainScene(scene_override.empty() && !ui_only);
    if (GCore_Init(&core_desc) != 0) {
        std::fprintf(stderr, "[smoke] GCore_Init failed\n");
        Window::shutdown_sdk();
        return 1;
    }
    if (!scene_override.empty() && GScene_Load(scene_override.c_str()) != 0) {
        std::fprintf(stderr, "[smoke] GScene_Load('%s') failed\n", scene_override.c_str());
    }

    // 3) 渲染器（Project Settings 里"重启生效"的项必须在 init 之前应用）
    GRender_SetShadowMapSize(cfg.shadow_map_size);
    GRender_SetVSync(vsync);
    // 调试 UI 也属于"init 前生效"：引擎在 GRender_Init 里决定要不要接管 ImGui，
    // --no-debug-ui 必须在那之前告知，否则 ImGui 照旧会初始化（只是不画面板）。
    if (!debug_ui_enabled) GRender_SetDebugUI(false);
    // 离屏视口输出同理：管线在 init 时按目标 FBO 建立 tonemap 的 render pass。
    // UI 预览没有 3D 可预览，开了只会多一倍离屏目标与一次合成。
    if (editor_preview && !ui_only) GRender_SetViewportOutput(true);

    GRenderInitDesc desc{};
    desc.version = sizeof(GRenderInitDesc);
    desc.native_window = window.native_handle();
    desc.api = api;
    desc.viewport_w = width;
    desc.viewport_h = height;
    desc.sync_mode = sync_mode;
    // MSAA 必须在 GRender_Init 之前下发：采样数决定 init 时创建的 HDR 目标
    // 形态（多样本颜色附件 + resolve 目标）。--no-msaa → 1（= 关闭）。
    if (msaa_override >= 0) {
        const int msaa = msaa_override > 0 ? msaa_override : 1;
        GRender_SetMSAASamples(msaa);
        std::printf("[smoke] msaa = %d%s\n", msaa, msaa == 1 ? " (off)" : "x");
    }
    // 字形整形强度必须在 GRender_Init 之前下发：首次烘图集发生在 init 内。
    if (font_shape != 1.0f) {
        gryce_engine::render::FontAtlas::set_shape_strength(font_shape);
        std::printf("[smoke] font shape strength = %.2f\n", font_shape);
    }
    if (GRender_Init(&desc) != 0) {
        std::fprintf(stderr, "[smoke] GRender_Init failed\n");
        GCore_Shutdown();
        Window::shutdown_sdk();
        return 1;
    }

#ifdef __EMSCRIPTEN__
    // 浏览器窗口缩放：平台层已同步好画布缓冲，这里跟进去重建渲染目标。
    // 视口尺寸一变，HDR/G-Buffer/SSAO/SSR 等目标都要按新尺寸重建。
    window.set_resize_callback([](int w, int h) {
        std::printf("[web] viewport resize -> %dx%d\n", w, h);
        GViewport_SetSize(w, h);
    });
#endif

    // UI 贴图服务必须在渲染器起来之后注册：UIImage / 图标按钮写 texture_path
    // 就能直接显示贴图。
    gryce_engine::ui::ui_set_texture_loader(&ui_texture_from_file);

    GRender_SetHDR(cfg.hdr);
    GRender_SetToneMapMode(cfg.tone_map_mode);
    GRender_SetExposure(cfg.exposure);
    GRender_SetShadowEnabled(cfg.shadow_enabled);
    GRender_SetAmbient(cfg.ambient[0], cfg.ambient[1], cfg.ambient[2]);
    GRender_SetIBLIntensity(cfg.ibl_intensity);
    if (env_override >= 0) cfg.default_environment = env_override != 0;
    GRender_SetDefaultEnvironment(cfg.default_environment);
    std::printf("[smoke] default_environment=%d\n", cfg.default_environment ? 1 : 0);

    // 屏幕空间效果：SSR/SSIL 依赖管线自动跑的 depth+normal 预通道；
    // 命令行覆盖优先于 project.data，方便做 A/B 对照。
    if (ssr_override >= 0) cfg.ssr_enabled = ssr_override != 0;
    if (ssao_override >= 0) cfg.ssao_enabled = ssao_override != 0;
    if (ssil_override >= 0) cfg.ssil_enabled = ssil_override != 0;
    GRender_SetSSRParams(cfg.ssr_max_steps, cfg.ssr_max_roughness, cfg.ssr_thickness,
                         cfg.ssr_bilateral);
    GRender_SetSSREnvFallback(cfg.ssr_env_fallback);
    GRender_SetSSRResolutionScale(cfg.ssr_resolution_scale);
    if (ssr_env_fallback_override >= 0.0f) {
        cfg.ssr_env_fallback = ssr_env_fallback_override;
        GRender_SetSSREnvFallback(ssr_env_fallback_override);
    }
    if (ssr_scale_override > 0.0f) {
        cfg.ssr_resolution_scale = ssr_scale_override;
        GRender_SetSSRResolutionScale(ssr_scale_override);
    }
    if (debug_view_override >= 0) GRender_SetDebugView(debug_view_override);
    if (pcss_override >= 0) cfg.pcss_enabled = pcss_override != 0;
    GRender_SetPCSSEnabled(cfg.pcss_enabled);
    GRender_SetPCSSParams(cfg.pcss_light_size, cfg.pcss_max_radius, 1.0f);
    // 左上角白色 FPS（默认开；--no-fps 关闭）
    GRender_SetFPSOverlay(fps_overlay);
    // UI 帧节流（--ui-interval N）：调试面板每 N 个渲染帧才重新构建一次，
    // 中间的帧重绘上一份 draw data。默认 1 = 每帧构建。
    if (ui_interval_override > 0) {
        GRender_SetUiFrameInterval(ui_interval_override);
        std::printf("[smoke] ui frame interval = %d\n", ui_interval_override);
    }
    if (!debug_ui_enabled) GRender_SetDebugUI(false);
    if (fps_limit >= 0) {
        GRender_SetFrameRateLimit(fps_limit);
        std::printf("[smoke] fps limit = %d%s\n", fps_limit,
                    fps_limit == 0 ? " (unlimited)" : "");
    }
    if (gpu_cull_override >= 0) GRender_SetGPUCullingEnabled(gpu_cull_override != 0);
    if (occlusion_override >= 0) GRender_SetOcclusionCullingEnabled(occlusion_override != 0);
    if (power_saver_override >= 0) GRender_SetPowerSaverForced(power_saver_override != 0);
    if (ssr_debug_override > 0) GRender_SetSSRDebugView(ssr_debug_override);
    GRender_SetSSAO(cfg.ssao_enabled);
    GRender_SetSSR(cfg.ssr_enabled);
    GRender_SetRTReflection(cfg.rt_reflection_enabled);
    GRender_SetRTTransmission(cfg.rt_transmission_enabled);
    GRender_SetRTGI(cfg.rt_gi_enabled);
    GRender_SetSSIL(cfg.ssil_enabled);
    if (editor_preview && !ui_only) {
        // Gizmo 需要选中实体才有东西可画：用屏幕中心拾取一个，
        // 拾取不到也不影响其余检查（命中测试会如实返回 -1）。
        const GEntityHandle picked = GScene_PickScreen(
            static_cast<float>(cfg.width) * 0.5f, static_cast<float>(cfg.height) * 0.5f,
            cfg.width, cfg.height, 0);
        if (picked != 0) GEdit_SelectEntity(picked);
        GRender_SetGizmoEnabled(true);
        std::printf("[smoke] editor preview: picked entity %d, gizmo enabled\n",
                    static_cast<int>(picked));
    }
    std::printf("[smoke] screen-space: ssao=%d ssr=%d ssil=%d "
                "(ssr steps=%.0f max_rough=%.2f thickness=%.3f bilateral=%.2f)\n",
                cfg.ssao_enabled ? 1 : 0, cfg.ssr_enabled ? 1 : 0, cfg.ssil_enabled ? 1 : 0,
                static_cast<double>(cfg.ssr_max_steps),
                static_cast<double>(cfg.ssr_max_roughness),
                static_cast<double>(cfg.ssr_thickness),
                static_cast<double>(cfg.ssr_bilateral));

    // ---- 压力场景注入（在统计与渲染之前）----
    // 必须在 GRender_Init 之后：ImportJson 内部按材质名解析资源，
    // 渲染器未初始化时材质/网格拿不到 GPU 句柄。
    if (stress_count > 0) {
        const StressReport rep = generate_stress_scene(stress_count);
        std::printf("[smoke] --stress %d: 注入 %d 个实体（失败 %d）%s\n",
                    stress_count, rep.entities, rep.failed,
                    stress_shadow ? " [shadow-casters]" : "");
    }
    if (instanced_count > 0) {
        const bool ok = spawn_instanced_scene(instanced_count);
        std::printf("[smoke] --instanced %d: %s\n", instanced_count,
                    ok ? "实例化实体已注入（GPU 剔除路径应被启用）" : "注入失败");
    }
    if (ui_demo && !inject_gryce_ui_demo()) {
        std::fprintf(stderr, "[smoke] failed to inject GryceUI demo\n");
        return 1;
    }
    if (ui_gallery && !inject_gryce_ui_gallery(theme_arg, gallery_all, gallery_scroll,
                                               menu_open_demo)) {
        std::fprintf(stderr, "[smoke] failed to inject GryceUI gallery\n");
        return 1;
    }
    if (ui_codeeditor) {
        set_ui_theme_preset(parse_theme(theme_arg));
        if (!inject_code_editor_demo(code_lang, code_find, code_font_size)) {
            std::fprintf(stderr, "[smoke] failed to inject code editor demo\n");
            return 1;
        }
    }

    // UI 预览：切到 scene2d 路径（清屏 + 2D 覆盖层），彻底不跑 3D 管线。
    // 清屏色取主题的 Window 角色，页面边缘与窗口底色才连成一片。
    uint32_t last_clear_packed = 0;
    auto sync_clear_color = [&last_clear_packed]() {
        // 主题色是 0xRRGGBBAA（alpha 在最低字节），换 float 时按位段取。
        const uint32_t packed = gryce_engine::ui::ui_theme_color(
            gryce_engine::ui::UIThemeColor::Window);
        if (packed == last_clear_packed) return;
        last_clear_packed = packed;
        GRender_SetScene2DClearColor(static_cast<float>((packed >> 24) & 0xFFu) / 255.0f,
                                     static_cast<float>((packed >> 16) & 0xFFu) / 255.0f,
                                     static_cast<float>((packed >> 8) & 0xFFu) / 255.0f);
    };
    if (ui_only) {
        GRender_SetScene2D(true);
        sync_clear_color();
        std::printf("[smoke] ui-only: scene2d path (3D pipeline skipped)\n");
    }

    const SceneStats scene = scan_scene();
    std::printf("[smoke] scene: entities=%d cameras=%d lights=%d meshes=%d\n",
                scene.entities, scene.cameras, scene.lights, scene.meshes);
    std::printf("[smoke] renderer initialized (%s)\n", backend_name);

    if (list_only) {
        GRender_Shutdown();
        GCore_Shutdown();
        Window::shutdown_sdk();
        return 0;
    }

    // 4) 交互模式：锁定鼠标、找到主相机
#ifdef __EMSCRIPTEN__
    FpsCamera& play_cam = *new FpsCamera();   // 跨 set_main_loop 的栈展开存活
#else
    FpsCamera play_cam;
#endif
    if (play_mode) {
        if (play_cam.init_from_entity(play_speed, play_sensitivity)) {
            // 光标锁定不在这里做：FpsCamera::update 每帧按"窗口有焦点 && 用户没主动
            // 解锁"重算（失焦自动放开，重新获得焦点自动恢复），单一来源，避免两处状态打架。
            window.focus_window();
            std::printf("[smoke] --play: 鼠标=转视角（仅窗口有焦点时锁定；"
                        "按一下 Alt 切换锁定/解锁）  WASD=移动  "
                        "空格/左Ctrl=升降  Shift=加速  Esc=退出  (speed=%.1f m/s sens=%.2f°/px)\n",
                        static_cast<double>(play_speed),
                        static_cast<double>(play_sensitivity));
        } else {
            std::fprintf(stderr, "[smoke] --play: 场景中没有 Camera 实体，退回普通模式\n");
            play_mode = false;
        }
    }
    if ((move_cam || cam_override) && !play_mode) {
        if (!play_cam.init_from_entity(play_speed, play_sensitivity)) {
            std::fprintf(stderr, "[smoke] --move/--cam-pos: 场景中没有 Camera 实体，忽略\n");
            move_cam = false;
            cam_override = false;
        }
    }
    if (cam_override && play_cam.entity != 0) {
        GVec3 d{cam_look_override.x - cam_pos_override.x,
                cam_look_override.y - cam_pos_override.y,
                cam_look_override.z - cam_pos_override.z};
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len > 1e-6f) { d.x /= len; d.y /= len; d.z /= len; }
        constexpr float k_rad2deg = 57.29577951308232f;
        play_cam.position = cam_pos_override;
        play_cam.pitch_deg = std::asin(std::max(-1.0f, std::min(1.0f, d.y))) * k_rad2deg;
        play_cam.yaw_deg = std::atan2(-d.x, -d.z) * k_rad2deg;
        GEntity_SetLocalPosition(play_cam.entity, &cam_pos_override);
        GQuat q = play_cam.to_quat();
        GEntity_SetLocalRotation(play_cam.entity, &q);
        std::printf("[smoke] 相机已覆盖: pos=(%.2f,%.2f,%.2f) look=(%.2f,%.2f,%.2f)\n",
                    static_cast<double>(cam_pos_override.x), static_cast<double>(cam_pos_override.y),
                    static_cast<double>(cam_pos_override.z), static_cast<double>(cam_look_override.x),
                    static_cast<double>(cam_look_override.y), static_cast<double>(cam_look_override.z));
    }
    // 进入引擎 Play 模式：--play 只负责相机控制，不会自动切换播放态；
    // 不切换的话 World::update 不跑，组件的 on_update（动画/物理/粒子）全程静止。
    if (play_mode || simulate_mode) {
        GCommand play_cmd{};
        play_cmd.type = ECMD_PLAY_MODE;
        if (GCore_PushCommand(&play_cmd) != 0) {
            std::fprintf(stderr, "[smoke] 进入 Play 模式失败\n");
        } else {
            std::printf("[smoke] 已进入 Play 模式（组件 on_update 生效）\n");
        }
    }

#ifdef __EMSCRIPTEN__
    // 浏览器默认常驻渲染：没有显式要求一次性跑帧（--frames / --bench /
    // --play-frames）时进 requestAnimationFrame 循环，页面不会停在最后一帧。
    // 无头验证仍然走下面的一次性路径，保证进程可终止。
    if (!frames_explicit && bench_frames == 0 && play_frames == 0) {
        bool camera_ready = play_cam.entity != 0;
        if (!camera_ready) {
            // 网页里不给相机控制就只是张静态图：默认接管主相机，
            // 点击画面锁定鼠标后即可用鼠标转视角 + WASD 移动。
            camera_ready = play_cam.init_from_entity(play_speed, play_sensitivity);
            if (camera_ready) {
                std::printf("[web] 交互模式：点击画面锁定鼠标，WASD 移动，Shift 加速，Esc 释放\n");
            }
        }
        WebLoopState* state = new WebLoopState{&window, &play_cam, camera_ready};
        emscripten_set_main_loop_arg(web_render_step, state, 0, 1);
        return 0;
    }
#endif

    // UI 预览默认常驻：只要没显式 --frames 也没要截图，就一直跑到关窗。
    // 原来默认第 60 帧截图退出，慢的时候 60 帧只有 6 秒，看起来像"自己闪退"。
    const bool ui_resident = ui_only && !frames_explicit && !shot_explicit &&
                             bench_frames == 0 && play_frames == 0;
    // 交互模式默认一直跑到用户退出；只有显式给了 --frames N 才在第 N 帧截图。
    const bool shot_wanted = ui_resident ? false
                           : (play_mode ? (frames_explicit && frames > 0) : true);
    if (ui_resident) {
        std::printf("[smoke] ui-only: 常驻运行（关窗退出；要出图加 --frames N --shot <png>）\n");
    }

    // --bench：开启 GPU 分段计时，跑满指定帧数后打印每 pass 的 GPU 时间。
    if (bench_frames > 0) {
        frames = bench_frames;
        GRender_SetGPUProfiling(1);
        std::printf("[smoke] --bench: GPU 分段计时已开启（%d 帧后打印；"
                    "建议同时加 --no-vsync 让帧循环不被垂直同步拖慢）\n", bench_frames);
    }

    // 5) 帧循环：渲染 -> 请求截图 -> 再驱动若干帧让渲染线程写盘
    int rendered = 0;
    int frames_after_shot = -1;
    bool shot_requested = false;
    auto play_clock = std::chrono::steady_clock::now();
    float move_phase = 0.0f;
    // 逐帧墙钟耗时（含 GPU 等待）：用来暴露周期性卡顿——平均值正常但存在
    // 规律性尖峰时，只看 pass 平均耗时是发现不了的。
    std::vector<float> bench_frame_ms;
    if (bench_frames > 0) bench_frame_ms.reserve(static_cast<size_t>(bench_frames) + 8);
    // 0=world update 1=render begin 2=RenderWorld 3=RenderGizmo 4=EndFrame 5=Core EndFrame
    double bench_phase_ms[6] = {};
    // 视口尺寸跟随窗口：窗口有可拖动的 resize 边框，被拉伸/最大化后如果视口
    // 仍停在旧尺寸，引擎就继续按旧尺寸渲染，back buffer 里超出的部分停在清屏色
    // —— 画面只占一块、其余全黑。每帧现查帧缓冲尺寸，变化了才下发（幂等）。
    int tracked_vp_w = width;
    int tracked_vp_h = height;
    while (!window.should_close()) {
        window.poll_events();
        // --click-probe：在第 30/31 帧合成一次"按下 → 松开"，验证"窗口事件 →
        // 输入层 → UI 运行时 → clicked"整条链路。目标取运行时布局结果（不写死
        // 像素），两帧都绕开窗口状态注入，否则窗口里的指针位置会把点击位置冲掉。
        bool synthetic_click_frame = false;
        if (click_probe && g_gallery.root && g_gallery.b_light) {
            if (rendered == 30) {
                const gryce_engine::ui::UIRect* rect =
                    g_gallery.root->runtime().rect_for(g_gallery.b_light);
                if (rect) {
                    probe_x = rect->x + rect->w * 0.5f;
                    probe_y = rect->y + rect->h * 0.5f;
                }
                std::printf("[click] target=Light at (%.0f, %.0f)\n", probe_x, probe_y);
            }
            if (rendered == 30 || rendered == 31) {
                synthetic_click_frame = true;
                GInput_BeginHostFrame();
                GInput_InjectMouseMove(probe_x, probe_y);
                GInput_InjectMouseButton(0,
                                         rendered == 30 ? GINPUT_ACTION_PRESS
                                                        : GINPUT_ACTION_RELEASE,
                                         probe_x, probe_y);
            }
            if (rendered == 40) {
                std::printf("[click] theme=%s ok=%d\n", g_gallery.current_theme.c_str(),
                            g_gallery.current_theme == "Light" ? 1 : 0);
            }
        }
        if (!synthetic_click_frame) pump_input(window);
        if (ui_only) sync_clear_color();   // 换主题时清屏色跟着走
        // --resize-probe：每 25 帧换一次窗口尺寸，用来核对"拖动窗口不黑屏"。
        // 尺寸同步走的是宿主通知路径（GViewport_NotifyWindowSize），与真实拖拽
        // 相同；这里只是把"拖"换成程序改尺寸，好让无头环境也能复现。
        if (resize_probe && rendered > 0 && rendered % 25 == 0) {
            static const int kProbeSizes[3][2] = {{1280, 720}, {960, 560}, {1600, 900}};
            const int* probe = kProbeSizes[(rendered / 25) % 3];
            window.set_size(probe[0], probe[1]);
        }
        if (ui_gallery) gallery_poll();
        {
            int fb_w = 0;
            int fb_h = 0;
            window.get_framebuffer_size(fb_w, fb_h);
            if (fb_w > 0 && fb_h > 0 && (fb_w != tracked_vp_w || fb_h != tracked_vp_h)) {
                tracked_vp_w = fb_w;
                tracked_vp_h = fb_h;
                // 必须走 NotifyWindowSize 而不是 GViewport_SetSize：后者只改视口，
                // 屏幕尺寸（= 交换链 extent 与 UI 坐标系）留在旧值，拖动窗口时
                // present 一直 OUT_OF_DATE、又反复按旧尺寸重建，整窗就是黑的。
                GViewport_NotifyWindowSize(fb_w, fb_h);
            }
        }

        // 交互模式的 dt 用真实时间（固定 1/60 会让移动速度随帧率漂移）
        float dt = 1.0f / 60.0f;
        if (play_mode) {
            const auto now = std::chrono::steady_clock::now();
            dt = std::chrono::duration<float>(now - play_clock).count();
            play_clock = now;
            dt = std::max(0.0005f, std::min(dt, 0.1f));
            if (!play_cam.update(window, dt)) break;   // Esc
        }
        if (move_cam && play_cam.entity != 0) {
            // 每帧位移约 0.05m：超过探针 0.02m 的单帧漂移阈值，会稳定触发
            // "相机移动"分支（每 2 帧一次全场景 6 面重烘）。
            move_phase += 0.35f;
            GVec3 p = play_cam.position;
            p.x += 0.15f * std::sin(move_phase);
            GEntity_SetLocalPosition(play_cam.entity, &p);
        }
        const auto t0 = std::chrono::steady_clock::now();
        GCore_BeginFrame(dt);
        const auto t1 = std::chrono::steady_clock::now();
        GRender_BeginFrame();
        const auto t2 = std::chrono::steady_clock::now();
        GRender_RenderWorld();
        // 编辑器预览：SceneView 之后再把 GameView 渲进它自己的颜色目标。两张图
        // 共用同一条 HDR 链，所以这里只是多一次 tonemap，不是重画一遍场景。
        if (editor_preview && !ui_only) GRender_RenderGameView();
        const auto t3 = std::chrono::steady_clock::now();
        if (!ui_only) GRender_RenderGizmo();
        const auto t4 = std::chrono::steady_clock::now();
        GRender_EndFrame();
        const auto t5 = std::chrono::steady_clock::now();
        GCore_EndFrame();
        const auto bench_frame_end = std::chrono::steady_clock::now();
        if (bench_frames > 0) {
            auto ms = [](auto a, auto b) {
                return std::chrono::duration<float, std::milli>(b - a).count();
            };
            bench_phase_ms[0] += ms(t0, t1);   // world update（ECS/粒子/动画 on_update）
            bench_phase_ms[1] += ms(t1, t2);   // render begin
            bench_phase_ms[2] += ms(t2, t3);   // RenderWorld（构建命令）
            bench_phase_ms[3] += ms(t3, t4);   // RenderGizmo
            bench_phase_ms[4] += ms(t4, t5);   // EndFrame（提交 + 等 GPU）
            bench_phase_ms[5] += ms(t5, bench_frame_end);  // Core EndFrame
            bench_frame_ms.push_back(ms(t0, bench_frame_end));
        }
        ++rendered;

        if (!shot_requested && shot_wanted && rendered >= frames) {
            // 渲染线程下一帧才会真正执行截图命令，故请求后再驱动几帧。
            GRender_SaveScreenshot(shot_path.c_str());
            shot_requested = true;
            frames_after_shot = shot_delay;
        }
        // 交互模式截图后继续跑（用户自己决定什么时候退出）
        if (!play_mode && frames_after_shot >= 0 && frames_after_shot-- == 0) break;
        if (play_mode && play_frames > 0 && rendered >= play_frames) break;
    }

    if (play_mode) window.set_cursor_disabled(false);
    if (bench_frames > 0) {
        // 统计在帧槽复用时结算（有 2 帧延迟），这里已跑满 bench_frames，直接打印。
        GRender_DumpGPUProfiling();
        GRender_SetGPUProfiling(0);
    }
    if (bench_frames > 0 && bench_frame_ms.size() > 40) {
        // 丢弃前 30 帧预热（着色器编译、探针首抓、缓冲首次分配）
        constexpr size_t k_warmup = 30;
        std::vector<float> s(bench_frame_ms.begin() + static_cast<ptrdiff_t>(k_warmup),
                             bench_frame_ms.end());
        std::sort(s.begin(), s.end());
        double sum = 0.0;
        for (float v : s) sum += v;
        const float avg = static_cast<float>(sum / static_cast<double>(s.size()));
        const float p50 = s[s.size() / 2];
        const float p99 = s[static_cast<size_t>(static_cast<double>(s.size() - 1) * 0.99)];
        const float mx = s.back();
        size_t spikes = 0;
        for (float v : s) if (v > p50 * 2.0f) ++spikes;
        std::printf("[cpu] 帧耗时 样本=%zu  平均=%.2fms(%.1f FPS)  中位=%.2fms  "
                    "p99=%.2fms  最大=%.2fms  抖动倍数=%.1fx\n",
                    s.size(), avg, avg > 0.0f ? 1000.0f / avg : 0.0f, p50, p99, mx,
                    p50 > 0.0f ? mx / p50 : 0.0f);
        std::printf("[cpu] 尖峰(>2x中位)=%zu 帧 (%.1f%%)\n", spikes,
                    s.empty() ? 0.0 : 100.0 * static_cast<double>(spikes) / static_cast<double>(s.size()));
        {
            const double n = static_cast<double>(bench_frame_ms.size());
            static const char* k_names[6] = {"world update", "render begin", "RenderWorld",
                                             "RenderGizmo", "EndFrame(提交+等GPU)", "Core EndFrame"};
            std::printf("[cpu] 分相平均:");
            for (int i = 0; i < 6; ++i) {
                std::printf(" %s=%.2fms", k_names[i], bench_phase_ms[i] / n);
            }
            std::printf("\n");
        }
        // 最慢的 5 帧及其序号：用于区分"周期性尖峰"（序号间隔固定）与
        // "收尾一次性开销"（序号集中在末尾，如截图编码/写盘）。
        std::vector<size_t> order(s.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::partial_sort(order.begin(), order.begin() + std::min<size_t>(5, order.size()),
                          order.end(),
                          [&](size_t a, size_t b) { return bench_frame_ms[k_warmup + a] > bench_frame_ms[k_warmup + b]; });
        std::printf("[cpu] 最慢 5 帧(帧序号:耗时ms):");
        for (size_t i = 0; i < std::min<size_t>(5, order.size()); ++i) {
            std::printf(" #%zu:%.1f", k_warmup + order[i], bench_frame_ms[k_warmup + order[i]]);
        }
        std::printf("\n");
        // 每 16 帧为一组打印组内最大耗时，周期性重抓（探针每 16 帧全场景 6 面）
        // 会表现为固定的第 16 帧凸起。
        std::printf("[cpu] 前 64 帧耗时(ms):");
        const size_t show = std::min<size_t>(s.size(), 64);
        for (size_t i = 0; i < show; ++i) {
            std::printf(" %s%.1f", (i % 16 == 15) ? "|" : "", bench_frame_ms[k_warmup + i]);
        }
        std::printf("\n");
    }
    // ★ 退出顺序：**先拆场景（World），再拆渲染器**。
    //
    // MeshRenderer::~MeshRenderer 会调 material->destroy_gpu(ctx_) /
    // ctx_->destroy_mesh()，而 ctx_ 是缓存的裸指针（mesh_renderer.h:68，
    // 只在首次上传时赋值，渲染器关闭时不会被清空）。若先 GRender_Shutdown
    // 再 GCore_Shutdown，这些析构就是对已释放 RenderContext 的 use-after-free。
    //
    // GL/Vulkan 之所以一直没暴露：那块堆内存被释放后内容恰好还像样，
    // running_ 读出来仍是 false，走到"同步销毁"分支糊弄过去了。
    // D3D12 的关闭路径分配更多，堆块被 0xFEEEFEEE 填充，
    // running_ 读成 true → 走 enqueue_destroy → 对垃圾 mutex 加锁 → SIGSEGV。
    // 帧率/帧时间读数：锁帧、VSync 这类"锁帧技术"的效果就靠这两个数验证
    // （--fps-limit 30 时这里应该稳定在 30 左右）。
    // 必须在 GRender_Shutdown() **之前**取 —— 渲染器销毁后统计就清零了。
    // 编辑器预览的可见证据：纹理句柄非空说明离屏目标建成了；GameView 句柄
    // 与 SceneView 不同说明双视口是两份目标，而不是回退共用一份。
    // UI 预览模式刻意不开离屏视口（没有 3D 可预览），所以这条检查只在真的
    // 需要编辑器预览时才成立。
    if (editor_preview && !ui_only) {
        GTextureHandle vp_tex = GRender_GetViewportTexture();
        GTextureHandle gv_tex = GRender_GetGameViewTexture();
        const int axis = GRender_GizmoHitTest(static_cast<float>(cfg.width) * 0.5f,
                                              static_cast<float>(cfg.height) * 0.5f);
        std::printf("[smoke] editor preview: viewport_tex=%p gameview_tex=%p "
                    "distinct=%d viewport_output=%d gizmo_hit=%d\n",
                    static_cast<void*>(vp_tex), static_cast<void*>(gv_tex),
                    (vp_tex && gv_tex && vp_tex != gv_tex) ? 1 : 0,
                    GRender_IsViewportOutputEnabled() ? 1 : 0, axis);
        if (!vp_tex) {
            std::fprintf(stderr, "[smoke] 离屏视口纹理未创建（--editor-preview 失败）\n");
            preview_ok = false;
        }
    }

    const float final_fps = GRender_GetFPS();
    const float final_frame_ms = GRender_GetFrameTimeMs();
    const int   final_limit = GRender_GetFrameRateLimit();

    GCore_Shutdown();
    GRender_Shutdown();
    Window::shutdown_sdk();

    std::printf("[smoke] done, rendered=%d frames, shot=%s\n", rendered, shot_path.c_str());
    std::printf("[smoke] fps=%.1f frame=%.2f ms (limit=%d vsync=%d)\n",
                static_cast<double>(final_fps), static_cast<double>(final_frame_ms),
                final_limit, vsync ? 1 : 0);

    if (!shot_requested) return 0;
    if (!std::filesystem::exists(shot_path)) {
        std::fprintf(stderr, "[smoke] 截图未生成（渲染线程未写盘）\n");
        return 2;
    }
    if (!shot_zoom_arg.empty()) {
        int zx = 0, zy = 0, zw = 0, zh = 0, zs = 1;
        if (std::sscanf(shot_zoom_arg.c_str(), "%d,%d,%d,%d,%d", &zx, &zy, &zw, &zh, &zs) == 5) {
            const std::string zoom_path = shot_path + ".zoom.png";
            if (write_zoomed_region(shot_path, zoom_path, zx, zy, zw, zh, zs)) {
                std::printf("[smoke] zoom crop %d,%d %dx%d x%d -> %s\n",
                            zx, zy, zw, zh, zs, zoom_path.c_str());
            } else {
                std::fprintf(stderr, "[smoke] zoom crop failed\n");
            }
        } else {
            std::fprintf(stderr, "[smoke] --shot-zoom 参数格式应为 x,y,w,h,scale\n");
        }
    }
    if (!analyze) {
        std::printf("[smoke] screenshot ok: %s\n", shot_path.c_str());
        return 0;
    }

    const ImageStats img = analyze_image(shot_path);
    if (!img.ok) {
        std::fprintf(stderr, "[smoke] 截图无法读取: %s\n", shot_path.c_str());
        return 2;
    }
    std::printf("[smoke] image %dx%d mean=(%.1f,%.1f,%.1f) dominant-color 之外占 %.2f%% "
                "(颜色档位 %d)\n",
                img.width, img.height, img.mean[0], img.mean[1], img.mean[2],
                img.non_dominant_ratio * 100.0, img.unique_bins);

    // 场景里有网格却只画出单一色块（清屏色 + 极少量覆盖），说明 RenderPipeline
    // 没生效。实测：正常渲染覆盖 ≈62%，管线退化到只清屏时 ≈9%。
    if (editor_preview) {
        // 离屏预览下 tonemap 写的是视口纹理，窗口本身只有清屏色 —— 像素占比
        // 判定在这里没有意义，改由上面的 viewport_tex 检查承担。
        std::printf("[smoke] OK: 编辑器预览路径（画面在离屏视口纹理里，窗口为空属正常）\n");
        return preview_ok ? 0 : 2;
    }
    if (scene.meshes > 0 && img.non_dominant_ratio < 0.20) {
        std::fprintf(stderr,
                     "[smoke] 场景有 %d 个 MeshRenderer，但画面只有 %.2f%% 非背景像素，"
                     "渲染管线未生效（着色器/网格上传/阴影？）\n",
                     scene.meshes, img.non_dominant_ratio * 100.0);
        return 2;
    }
    std::printf("[smoke] OK: 场景已渲染\n");
    return 0;
}
