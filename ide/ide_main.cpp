#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "api/core_api.h"
#include "api/component_api.h"
#include "api/edit_api.h"
#include "api/entity_api.h"
#include "api/input_api.h"
#include "api/render_api.h"
#include "api/scene_api.h"
#include "api/window_api.h"
#include "components/ui_root.h"
#include "ecs/world.h"
#include "scene/scene.h"
#include "ui/ui_editor_widgets.h"
#include "ui/ui_extended_widgets.h"
#include "ui/ui_widgets.h"
#include "ui/ui_clipboard.h"

namespace {

using gryce_engine::components::UIButton;
using gryce_engine::components::UIAssetGrid;
using gryce_engine::components::UICodeEditor;
using gryce_engine::components::UIConsoleView;
using gryce_engine::components::UIDockHost;
using gryce_engine::components::UIDockPanel;
using gryce_engine::components::UIDockSide;
using gryce_engine::components::UIFloatingPanel;
using gryce_engine::components::UILabel;
using gryce_engine::components::UIListView;
using gryce_engine::components::UIMenu;
using gryce_engine::components::UIMenuBar;
using gryce_engine::components::UIPropertyEditor;
using gryce_engine::components::UIPropertyKind;
using gryce_engine::components::UIPropertyRow;
using gryce_engine::components::UIRoot;
using gryce_engine::components::UISearchBox;
using gryce_engine::components::UIStatusBar;
using gryce_engine::components::UITabBar;
using gryce_engine::components::UITableView;
using gryce_engine::components::UITextInput;
using gryce_engine::components::UITreeItem;
using gryce_engine::components::UITreeView;
using gryce_engine::components::UIToast;
using gryce_engine::components::UIViewportPanel;
using gryce_engine::components::ui_format_number;
using gryce_engine::math::Vector2f;
using gryce_engine::math::Vector3f;
using gryce_engine::math::Vector4f;

struct IdeUI {
    gryce_engine::scene::Entity* owner = nullptr;
    UIRoot* root = nullptr;
    UIDockHost* dock = nullptr;
    UIDockPanel* top_bar = nullptr;
    UIDockPanel* hierarchy_panel = nullptr;
    UIDockPanel* center_panel = nullptr;
    UIDockPanel* inspector_panel = nullptr;
    UIDockPanel* bottom_panel = nullptr;
    UITreeView* hierarchy_tree = nullptr;
    UIViewportPanel* viewport = nullptr;
    UIViewportPanel* viewport_2d = nullptr;
    UIViewportPanel* game_view = nullptr;
    UITabBar* center_tabs = nullptr;
    UICodeEditor* code_editor = nullptr;
    UITabBar* bottom_tabs = nullptr;
    UILabel* bottom_content = nullptr;
    UIAssetGrid* asset_grid = nullptr;
    UIConsoleView* console_view = nullptr;
    UITableView* problems_view = nullptr;
    UILabel* hierarchy_title = nullptr;
    UITextInput* inspector_name = nullptr;
    UIPropertyEditor* reflection_grid = nullptr;
    UIMenuBar* menu_bar = nullptr;
    UIMenu* file_menu = nullptr;
    UIStatusBar* status = nullptr;
    UIFloatingPanel* palette = nullptr;
    UISearchBox* palette_search = nullptr;
    UIListView* palette_list = nullptr;
    UIToast* toast = nullptr;
    UITextInput* position_x = nullptr;
    UITextInput* position_y = nullptr;
    UITextInput* position_z = nullptr;
    UIButton* apply = nullptr;
    UIButton* undo = nullptr;
    UIButton* redo = nullptr;
    UIButton* play = nullptr;
    UIButton* pause = nullptr;
    UIButton* step = nullptr;
    UIButton* reload = nullptr;
    UIButton* view_mode = nullptr;
    UIButton* new_entity = nullptr;
    UIButton* duplicate_entity = nullptr;
    UIButton* delete_entity = nullptr;
    UIButton* rename_entity = nullptr;
    UIButton* save_scene = nullptr;
    bool view_2d = false;
    bool split_view = false;
    int center_tab_index = 0;
    std::string palette_filter;
    std::string project_root;
    std::vector<int> palette_commands;
    std::vector<GEntityHandle> hierarchy_handles;
    int hierarchy_refresh_delay = 0;
    int inspector_refresh_delay = 0;
};

struct Options {
    std::string project = "test/demo_project";
    std::string scene = "res:/scenes/main.gesc";
    std::string api = "opengl";
    std::string shot;
    int frames = 0;
    int width = 1280;
    int height = 720;
    bool view_2d = false;
    bool palette = false;
    bool script = false;
    bool split = false;
    bool game = false;
    int bottom_tab = 1;
};

std::string read_system_clipboard() {
    const int needed = GInput_GetClipboardText(nullptr, 0);
    if (needed <= 0) return {};
    std::string text(static_cast<std::size_t>(needed + 1), '\0');
    GInput_GetClipboardText(text.data(), static_cast<int>(text.size()));
    text.resize(static_cast<std::size_t>(needed));
    return text;
}

void write_system_clipboard(const std::string& text) {
    GInput_SetClipboardText(text.c_str());
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[ide] %s requires a value\n", name);
                return "";
            }
            return argv[++i];
        };
        if (std::strcmp(argv[i], "--project") == 0) options.project = need("--project");
        else if (std::strcmp(argv[i], "--scene") == 0) options.scene = need("--scene");
        else if (std::strcmp(argv[i], "--api") == 0) options.api = need("--api");
        else if (std::strcmp(argv[i], "--shot") == 0) options.shot = need("--shot");
        else if (std::strcmp(argv[i], "--frames") == 0) options.frames = std::atoi(need("--frames"));
        else if (std::strcmp(argv[i], "--w") == 0) options.width = std::atoi(need("--w"));
        else if (std::strcmp(argv[i], "--h") == 0) options.height = std::atoi(need("--h"));
        else if (std::strcmp(argv[i], "--view2d") == 0) options.view_2d = true;
        else if (std::strcmp(argv[i], "--palette") == 0) options.palette = true;
        else if (std::strcmp(argv[i], "--script") == 0) options.script = true;
        else if (std::strcmp(argv[i], "--split") == 0) options.split = true;
        else if (std::strcmp(argv[i], "--game") == 0) options.game = true;
        else if (std::strcmp(argv[i], "--assets") == 0) options.bottom_tab = 0;
        else if (std::strcmp(argv[i], "--problems") == 0) options.bottom_tab = 2;
    }
    return options;
}

GRenderAPI parse_api(const std::string& name) {
    if (name == "vulkan") return GRYCE_RENDER_API_VULKAN;
    if (name == "dx12") return GRYCE_RENDER_API_DX12;
    return GRYCE_RENDER_API_OPENGL;
}

void place(gryce_engine::components::NodeUI* node, float x, float y) {
    if (node) node->set_local_position(Vector3f(x, y, 0.0f));
}

void resize(gryce_engine::components::NodeUI* node, float w, float h) {
    if (!node) return;
    node->size = Vector2f(w, h);
    node->min_size = Vector2f(w, h);
}

void layout_ide(IdeUI& ide, int width, int height) {
    if (!ide.dock) return;
    const float w = static_cast<float>(std::max(width, 640));
    const float h = static_cast<float>(std::max(height, 400));
    const float gap = ide.dock->gap;
    const float middle_h = std::max(
        120.0f, h - ide.dock->top_height - ide.dock->bottom_height - gap * 2.0f);
    const float middle_w = std::max(
        240.0f, w - ide.dock->left_width - ide.dock->right_width - gap * 2.0f);

    if (ide.hierarchy_tree) {
        ide.hierarchy_tree->size = Vector2f(
            std::max(80.0f, ide.dock->left_width - 24.0f),
            std::max(40.0f, middle_h - 72.0f));
        ide.hierarchy_tree->min_size = ide.hierarchy_tree->size;
    }
    if (ide.viewport) {
        const float available_width = std::max(120.0f, middle_w - 16.0f);
        const float viewport_width = ide.split_view
            ? std::max(80.0f, (available_width - 4.0f) * 0.5f)
            : available_width;
        ide.viewport->size = Vector2f(
            viewport_width, std::max(80.0f, middle_h - 72.0f));
        ide.viewport->min_size = Vector2f(120.0f, 80.0f);
        place(ide.viewport, 0.0f, 60.0f);
        if (ide.viewport_2d) {
            ide.viewport_2d->size = ide.viewport->size;
            ide.viewport_2d->min_size = Vector2f(80.0f, 60.0f);
            ide.viewport_2d->set_local_position(Vector3f(
                viewport_width + 4.0f, 60.0f, 0.0f));
        }
        if (ide.game_view) {
            ide.game_view->size = Vector2f(
                available_width, std::max(80.0f, middle_h - 72.0f));
            ide.game_view->min_size = Vector2f(120.0f, 80.0f);
            place(ide.game_view, 0.0f, 60.0f);
        }
    }
    if (ide.code_editor) {
        ide.code_editor->size = ide.viewport ? ide.viewport->size :
            Vector2f(std::max(240.0f, middle_w - 16.0f),
                     std::max(100.0f, middle_h - 72.0f));
        ide.code_editor->min_size = Vector2f(160.0f, 80.0f);
    }
    if (ide.center_tabs) {
        ide.center_tabs->size = Vector2f(
            std::max(240.0f, middle_w - 16.0f), 28.0f);
        ide.center_tabs->min_size = ide.center_tabs->size;
    }
    if (ide.reflection_grid) {
        ide.reflection_grid->size = Vector2f(
            std::max(120.0f, ide.dock->right_width - 16.0f),
            std::max(80.0f, middle_h - 250.0f));
        ide.reflection_grid->min_size = Vector2f(120.0f, 80.0f);
        place(ide.reflection_grid, 0.0f, 220.0f);
    }
    if (ide.bottom_tabs) {
        ide.bottom_tabs->size = Vector2f(std::max(200.0f, w - 16.0f), 28.0f);
        ide.bottom_tabs->min_size = ide.bottom_tabs->size;
    }
    if (ide.bottom_content) {
        ide.bottom_content->size = Vector2f(
            std::max(200.0f, w - 16.0f),
            std::max(24.0f, ide.dock->bottom_height - 166.0f));
        ide.bottom_content->visible = false;
    }
    if (ide.asset_grid) {
        ide.asset_grid->size = Vector2f(
            std::max(200.0f, w - 16.0f),
            std::max(70.0f, ide.dock->bottom_height - 96.0f));
        ide.asset_grid->min_size = Vector2f(160.0f, 60.0f);
    }
    if (ide.console_view) {
        ide.console_view->size = Vector2f(
            std::max(200.0f, w - 16.0f),
            std::max(70.0f, ide.dock->bottom_height - 96.0f));
        ide.console_view->min_size = Vector2f(160.0f, 60.0f);
    }
    if (ide.problems_view) {
        ide.problems_view->size = Vector2f(
            std::max(200.0f, w - 16.0f),
            std::max(70.0f, ide.dock->bottom_height - 96.0f));
        ide.problems_view->min_size = Vector2f(160.0f, 60.0f);
    }
    if (ide.status) {
        place(ide.status, 0.0f, std::max(80.0f, ide.dock->bottom_height - 34.0f));
        ide.status->size = Vector2f(std::max(200.0f, w - 16.0f), 24.0f);
    }
    if (ide.menu_bar) {
        ide.menu_bar->size = Vector2f(std::max(240.0f, w - 160.0f), 24.0f);
        ide.menu_bar->min_size = ide.menu_bar->size;
    }
    if (ide.file_menu) {
        const auto menu_count = std::max<std::size_t>(
            1, gryce_engine::components::ui_split_lines(ide.menu_bar->items).size());
        const float menu_width = ide.menu_bar->size.x /
                                 static_cast<float>(menu_count);
        place(ide.file_menu,
              8.0f + ide.menu_bar->local_position().x +
                  menu_width * static_cast<float>(ide.menu_bar->selected),
              60.0f);
    }
    if (ide.palette) {
        ide.palette->size = Vector2f(std::max(360.0f, std::min(620.0f, w - 80.0f)),
                                     std::max(220.0f, std::min(360.0f, h - 120.0f)));
        ide.palette->min_size = Vector2f(320.0f, 180.0f);
        place(ide.palette, (w - ide.palette->size.x) * 0.5f,
              (h - ide.palette->size.y) * 0.35f);
        resize(ide.palette_search,
               std::max(260.0f, ide.palette->size.x - 20.0f), 30.0f);
        ide.palette_list->size = Vector2f(
            std::max(260.0f, ide.palette->size.x - 20.0f),
            std::max(80.0f, ide.palette->size.y - 78.0f));
        ide.palette_list->min_size = ide.palette_list->size;
    }
    if (ide.toast) {
        resize(ide.toast, std::min(360.0f, w - 40.0f), 54.0f);
        place(ide.toast, w - ide.toast->size.x - 20.0f, 116.0f);
    }
    if (ide.root) ide.root->runtime().invalidate();
}

void update_inspector(IdeUI& ide);
void refresh_reflection_rows(IdeUI& ide);

void append_hierarchy_entity(IdeUI& ide, GEntityHandle handle, int depth) {
    if (!handle || depth > 64) return;
    char name[256] = {};
    if (GEntity_GetName(handle, name, sizeof(name)) <= 0) return;
    if (std::strcmp(name, "IDE UI") == 0) return;
    const int child_count = GEntity_GetChildCount(handle);
    ide.hierarchy_handles.push_back(handle);
    ide.hierarchy_tree->items.push_back({
        name, depth, true, child_count > 0, true,
    });
    for (int child = 0; child < child_count; ++child) {
        append_hierarchy_entity(ide, GEntity_GetChildAt(handle, child), depth + 1);
    }
}

void refresh_hierarchy(IdeUI& ide) {
    if (!ide.hierarchy_tree) return;
    ide.hierarchy_handles.clear();
    ide.hierarchy_tree->items.clear();
    const int entity_count = GEntity_GetCount();
    for (int i = 0; i < entity_count; ++i) {
        const GEntityHandle handle = GEntity_GetAt(i);
        if (handle && !GEntity_GetParent(handle)) {
            append_hierarchy_entity(ide, handle, 0);
        }
    }
    ide.hierarchy_tree->selected = -1;
    const GEntityHandle selected = GEntity_GetSelected();
    for (int i = 0; i < static_cast<int>(ide.hierarchy_handles.size()); ++i) {
        if (ide.hierarchy_handles[static_cast<std::size_t>(i)] == selected) {
            ide.hierarchy_tree->selected = i;
            break;
        }
    }
    if (ide.hierarchy_tree->selected < 0 && !ide.hierarchy_handles.empty()) {
        ide.hierarchy_tree->selected = 0;
    }
    ide.root->runtime().invalidate();
}

void refresh_assets(IdeUI& ide) {
    if (!ide.asset_grid || ide.project_root.empty()) return;
    std::vector<std::string> names;
    auto add_directory = [&](const std::filesystem::path& directory,
                             const std::string& prefix) {
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) return;
        for (std::filesystem::directory_iterator it(directory, error), end;
             !error && it != end; it.increment(error)) {
            if (it->path().filename() == ".gryce") continue;
            names.push_back(prefix + it->path().filename().generic_string());
            if (names.size() >= 256) break;
        }
    };
    const std::filesystem::path root(ide.project_root);
    add_directory(root, "");
    for (const char* folder : {"scenes", "scripts", "models", "textures"}) {
        add_directory(root / folder, std::string(folder) + "/");
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::ostringstream out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i) out << '\n';
        out << names[i];
    }
    ide.asset_grid->items = out.str();
    ide.asset_grid->selected = ide.asset_grid->items.empty() ? -1 : 0;
    ide.root->runtime().invalidate();
}

void append_console(IdeUI& ide, const std::string& message) {
    if (!ide.console_view) return;
    ide.console_view->append(message);
    ide.root->runtime().invalidate();
}

void duplicate_selected_entity(IdeUI& ide) {
    const GEntityHandle selected = GEntity_GetSelected();
    if (!selected) return;
    std::vector<char> json(1024 * 1024);
    int written = GEntity_ExportJson(selected, json.data(),
                                     static_cast<int>(json.size()));
    if (written < 0) {
        json.resize(8 * 1024 * 1024);
        written = GEntity_ExportJson(selected, json.data(),
                                     static_cast<int>(json.size()));
    }
    if (written <= 0) {
        ide.toast->show("Duplicate failed", 2.5f,
                        gryce_engine::components::UIToastSeverity::Error);
        return;
    }
    const GEntityHandle duplicate =
        GEntity_ImportJson(json.data(), GEntity_GetParent(selected));
    if (!duplicate) {
        ide.toast->show("Duplicate failed", 2.5f,
                        gryce_engine::components::UIToastSeverity::Error);
        return;
    }
    GEdit_SelectEntity(duplicate);
    refresh_hierarchy(ide);
    ide.inspector_refresh_delay = 2;
    append_console(ide, "[info] Entity duplicated");
    ide.toast->show("Entity duplicated", 1.8f,
                    gryce_engine::components::UIToastSeverity::Success);
}

bool build_ui(IdeUI& ide, int width, int height) {
    auto* world = static_cast<gryce_engine::ecs::World*>(GCore_GetInternalWorldPtr());
    if (!world || !world->scene()) return false;

    ide = {};
    ide.owner = world->scene()->create_entity("IDE UI");
    if (!ide.owner) return false;

    ide.root = ide.owner->add_component<UIRoot>();
    ide.dock = ide.owner->add_component<UIDockHost>();
    ide.top_bar = ide.owner->add_component<UIDockPanel>();
    ide.hierarchy_panel = ide.owner->add_component<UIDockPanel>();
    ide.center_panel = ide.owner->add_component<UIDockPanel>();
    ide.inspector_panel = ide.owner->add_component<UIDockPanel>();
    ide.bottom_panel = ide.owner->add_component<UIDockPanel>();
    ide.viewport = ide.owner->add_component<UIViewportPanel>();
    ide.viewport_2d = ide.owner->add_component<UIViewportPanel>();
    ide.game_view = ide.owner->add_component<UIViewportPanel>();
    ide.center_tabs = ide.owner->add_component<UITabBar>();
    ide.code_editor = ide.owner->add_component<UICodeEditor>();
    ide.bottom_tabs = ide.owner->add_component<UITabBar>();
    ide.bottom_content = ide.owner->add_component<UILabel>();
    ide.asset_grid = ide.owner->add_component<UIAssetGrid>();
    ide.console_view = ide.owner->add_component<UIConsoleView>();
    ide.problems_view = ide.owner->add_component<UITableView>();
    ide.hierarchy_title = ide.owner->add_component<UILabel>();
    ide.inspector_name = ide.owner->add_component<UITextInput>();
    ide.reflection_grid = ide.owner->add_component<UIPropertyEditor>();
    ide.menu_bar = ide.owner->add_component<UIMenuBar>();
    ide.file_menu = ide.owner->add_component<UIMenu>();
    ide.status = ide.owner->add_component<UIStatusBar>();
    ide.palette = ide.owner->add_component<UIFloatingPanel>();
    ide.palette_search = ide.owner->add_component<UISearchBox>();
    ide.palette_list = ide.owner->add_component<UIListView>();
    ide.toast = ide.owner->add_component<UIToast>();
    ide.hierarchy_tree = ide.owner->add_component<UITreeView>();
    ide.position_x = ide.owner->add_component<UITextInput>();
    ide.position_y = ide.owner->add_component<UITextInput>();
    ide.position_z = ide.owner->add_component<UITextInput>();
    ide.apply = ide.owner->add_component<UIButton>();
    ide.undo = ide.owner->add_component<UIButton>();
    ide.redo = ide.owner->add_component<UIButton>();
    ide.play = ide.owner->add_component<UIButton>();
    ide.pause = ide.owner->add_component<UIButton>();
    ide.step = ide.owner->add_component<UIButton>();
    ide.reload = ide.owner->add_component<UIButton>();
    ide.view_mode = ide.owner->add_component<UIButton>();
    ide.new_entity = ide.owner->add_component<UIButton>();
    ide.duplicate_entity = ide.owner->add_component<UIButton>();
    ide.delete_entity = ide.owner->add_component<UIButton>();
    ide.rename_entity = ide.owner->add_component<UIButton>();
    ide.save_scene = ide.owner->add_component<UIButton>();

    ide.dock->top_height = 100.0f;
    ide.dock->bottom_height = 200.0f;
    ide.dock->left_width = 260.0f;
    ide.dock->right_width = 280.0f;
    ide.dock->gap = 4.0f;
    ide.dock->anchor_min = Vector2f(0.0f, 0.0f);
    ide.dock->anchor_max = Vector2f(1.0f, 1.0f);

    ide.top_bar->side = UIDockSide::Top;
    ide.top_bar->title = "GryceIDE  /  Debug Host";
    ide.top_bar->padding = 8.0f;
    ide.top_bar->layout_mode = gryce_engine::components::UILayoutMode::Manual;
    ide.top_bar->color = Vector4f(0.045f, 0.05f, 0.07f, 0.98f);

    ide.hierarchy_panel->side = UIDockSide::Left;
    ide.hierarchy_panel->title = "HIERARCHY";
    ide.hierarchy_panel->padding = 8.0f;
    ide.hierarchy_panel->color = Vector4f(0.07f, 0.08f, 0.11f, 0.98f);

    ide.center_panel->side = UIDockSide::Center;
    ide.center_panel->title = "SCENE VIEW";
    ide.center_panel->padding = 8.0f;
    ide.center_panel->color = Vector4f(0.035f, 0.04f, 0.055f, 1.0f);

    ide.inspector_panel->side = UIDockSide::Right;
    ide.inspector_panel->title = "INSPECTOR";
    ide.inspector_panel->padding = 8.0f;
    ide.inspector_panel->layout_mode = gryce_engine::components::UILayoutMode::Manual;
    ide.inspector_panel->color = Vector4f(0.075f, 0.085f, 0.115f, 0.98f);

    ide.bottom_panel->side = UIDockSide::Bottom;
    ide.bottom_panel->title = "ASSETS / CONSOLE";
    ide.bottom_panel->padding = 8.0f;
    ide.bottom_panel->layout_mode = gryce_engine::components::UILayoutMode::Manual;
    ide.bottom_panel->color = Vector4f(0.055f, 0.063f, 0.085f, 0.98f);

    ide.center_tabs->items = "3D Scene\n2D Scene\n3D + 2D\nGame\nScript";
    ide.center_tabs->selected = 0;
    ide.center_tabs->tab_height = 28.0f;
    place(ide.center_tabs, 0.0f, 28.0f);
    place(ide.viewport, 0.0f, 60.0f);
    place(ide.viewport_2d, 0.0f, 60.0f);
    ide.viewport_2d->visible = false;
    place(ide.game_view, 0.0f, 60.0f);
    ide.game_view->visible = false;
    place(ide.code_editor, 0.0f, 60.0f);
    ide.code_editor->visible = false;

    ide.hierarchy_title->text = "Entities";
    ide.hierarchy_title->font_size = 15.0f;
    place(ide.hierarchy_title, 0.0f, 28.0f);
    resize(ide.hierarchy_title, 230.0f, 20.0f);

    place(ide.hierarchy_tree, 0.0f, 56.0f);

    ide.inspector_name->text = "No selection";
    ide.inspector_name->font_size = 15.0f;
    ide.inspector_name->size = Vector2f(250.0f, 28.0f);
    place(ide.inspector_name, 0.0f, 28.0f);
    place(ide.reflection_grid, 0.0f, 220.0f);
    ide.menu_bar->items = "File\nEdit\nView\nBuild\nHelp";
    ide.menu_bar->selected = 0;
    ide.menu_bar->font_size = 14.0f;
    place(ide.menu_bar, 142.0f, 28.0f);
    ide.file_menu->items = "Open Project...\nSave Layout\nReset Layout\nExit";
    ide.file_menu->font_size = 14.0f;
    ide.file_menu->set_open(false);

    ide.status->text = "Ready";
    ide.status->right_text = "GryceEngine | OpenGL";
    ide.status->font_size = 13.0f;

    ide.palette->title = "Command Palette";
    ide.palette->draggable = false;
    ide.palette->visible = false;
    ide.palette->receives_input = false;
    ide.palette_search->placeholder = "Type a command...";
    ide.palette_search->set_query({});
    place(ide.palette_search, 10.0f, 32.0f);
    ide.palette_list->items =
        "Reload Scene\nToggle 2D / 3D View\nUndo\nRedo\nReset Layout\nSave Layout";
    ide.palette_list->selected = 0;
    place(ide.palette_list, 10.0f, 70.0f);
    ide.palette_filter.clear();

    ide.position_x->text = "0.000";
    ide.position_y->text = "0.000";
    ide.position_z->text = "0.000";
    ide.position_x->size = Vector2f(250.0f, 28.0f);
    ide.position_y->size = Vector2f(250.0f, 28.0f);
    ide.position_z->size = Vector2f(250.0f, 28.0f);
    place(ide.position_x, 0.0f, 64.0f);
    place(ide.position_y, 0.0f, 98.0f);
    place(ide.position_z, 0.0f, 132.0f);

    ide.apply->text = "Apply Transform";
    resize(ide.apply, 160.0f, 30.0f);
    place(ide.apply, 0.0f, 172.0f);
    ide.undo->text = "Undo";
    resize(ide.undo, 76.0f, 26.0f);
    place(ide.undo, 94.0f, 58.0f);
    ide.redo->text = "Redo";
    resize(ide.redo, 76.0f, 26.0f);
    place(ide.redo, 178.0f, 58.0f);
    ide.play->text = "Play";
    resize(ide.play, 76.0f, 26.0f);
    place(ide.play, 262.0f, 58.0f);
    ide.reload->text = "Reload";
    resize(ide.reload, 86.0f, 26.0f);
    place(ide.reload, 0.0f, 58.0f);
    ide.view_mode->text = "2D View";
    resize(ide.view_mode, 86.0f, 26.0f);
    place(ide.view_mode, 346.0f, 58.0f);
    ide.new_entity->text = "New";
    resize(ide.new_entity, 64.0f, 26.0f);
    place(ide.new_entity, 440.0f, 58.0f);
    ide.duplicate_entity->text = "Duplicate";
    resize(ide.duplicate_entity, 86.0f, 26.0f);
    place(ide.duplicate_entity, 512.0f, 58.0f);
    ide.delete_entity->text = "Delete";
    resize(ide.delete_entity, 70.0f, 26.0f);
    place(ide.delete_entity, 606.0f, 58.0f);
    ide.rename_entity->text = "Rename";
    resize(ide.rename_entity, 76.0f, 26.0f);
    place(ide.rename_entity, 684.0f, 58.0f);
    ide.save_scene->text = "Save Scene";
    resize(ide.save_scene, 92.0f, 26.0f);
    place(ide.save_scene, 768.0f, 58.0f);
    ide.pause->text = "Pause";
    resize(ide.pause, 66.0f, 26.0f);
    place(ide.pause, 866.0f, 58.0f);
    ide.step->text = "Step";
    resize(ide.step, 58.0f, 26.0f);
    place(ide.step, 938.0f, 58.0f);

    ide.bottom_tabs->items = "Assets\nConsole\nProblems";
    ide.bottom_tabs->selected = 1;
    place(ide.bottom_tabs, 0.0f, 28.0f);
    ide.bottom_content->text =
        "Console: GryceIDE initialized. Engine debug UI disabled.";
    ide.bottom_content->font_size = 14.0f;
    place(ide.bottom_content, 0.0f, 62.0f);
    ide.bottom_content->visible = false;
    place(ide.asset_grid, 0.0f, 62.0f);
    place(ide.console_view, 0.0f, 62.0f);
    place(ide.problems_view, 0.0f, 62.0f);
    ide.asset_grid->columns = 5;
    ide.asset_grid->tile_size = Vector2f(96.0f, 82.0f);
    ide.console_view->items =
        "[info] GryceIDE initialized\n[info] GryceUI overlay active";
    ide.problems_view->columns = "Severity\nFile\nLine\nMessage";
    ide.problems_view->rows = "Info|main.js|1|No diagnostics";
    ide.asset_grid->visible = false;
    ide.console_view->visible = true;
    ide.problems_view->visible = false;

    ide.owner->set_component_parent(ide.dock, ide.root);
    ide.owner->set_component_parent(ide.top_bar, ide.dock);
    ide.owner->set_component_parent(ide.hierarchy_panel, ide.dock);
    ide.owner->set_component_parent(ide.center_panel, ide.dock);
    ide.owner->set_component_parent(ide.bottom_panel, ide.dock);
    ide.owner->set_component_parent(ide.inspector_panel, ide.dock);
    ide.owner->set_component_parent(ide.center_tabs, ide.center_panel);
    ide.owner->set_component_parent(ide.viewport, ide.center_panel);
    ide.owner->set_component_parent(ide.viewport_2d, ide.center_panel);
    ide.owner->set_component_parent(ide.game_view, ide.center_panel);
    ide.owner->set_component_parent(ide.code_editor, ide.center_panel);
    ide.owner->set_component_parent(ide.reload, ide.top_bar);
    ide.owner->set_component_parent(ide.undo, ide.top_bar);
    ide.owner->set_component_parent(ide.redo, ide.top_bar);
    ide.owner->set_component_parent(ide.play, ide.top_bar);
    ide.owner->set_component_parent(ide.pause, ide.top_bar);
    ide.owner->set_component_parent(ide.step, ide.top_bar);
    ide.owner->set_component_parent(ide.view_mode, ide.top_bar);
    ide.owner->set_component_parent(ide.new_entity, ide.top_bar);
    ide.owner->set_component_parent(ide.duplicate_entity, ide.top_bar);
    ide.owner->set_component_parent(ide.delete_entity, ide.top_bar);
    ide.owner->set_component_parent(ide.rename_entity, ide.top_bar);
    ide.owner->set_component_parent(ide.save_scene, ide.top_bar);
    ide.owner->set_component_parent(ide.menu_bar, ide.top_bar);
    ide.owner->set_component_parent(ide.file_menu, ide.root);
    ide.owner->set_component_parent(ide.palette, ide.root);
    ide.owner->set_component_parent(ide.palette_search, ide.palette);
    ide.owner->set_component_parent(ide.palette_list, ide.palette);
    ide.owner->set_component_parent(ide.toast, ide.root);
    ide.owner->set_component_parent(ide.hierarchy_title, ide.hierarchy_panel);
    ide.owner->set_component_parent(ide.hierarchy_tree, ide.hierarchy_panel);
    ide.owner->set_component_parent(ide.inspector_name, ide.inspector_panel);
    ide.owner->set_component_parent(ide.reflection_grid, ide.inspector_panel);
    ide.owner->set_component_parent(ide.position_x, ide.inspector_panel);
    ide.owner->set_component_parent(ide.position_y, ide.inspector_panel);
    ide.owner->set_component_parent(ide.position_z, ide.inspector_panel);
    ide.owner->set_component_parent(ide.apply, ide.inspector_panel);
    ide.owner->set_component_parent(ide.bottom_tabs, ide.bottom_panel);
    ide.owner->set_component_parent(ide.asset_grid, ide.bottom_panel);
    ide.owner->set_component_parent(ide.console_view, ide.bottom_panel);
    ide.owner->set_component_parent(ide.problems_view, ide.bottom_panel);
    ide.owner->set_component_parent(ide.bottom_content, ide.bottom_panel);
    ide.owner->set_component_parent(ide.status, ide.bottom_panel);

    refresh_hierarchy(ide);
    layout_ide(ide, width, height);
    return true;
}

std::string format_property_value(int type, const std::vector<uint8_t>& data) {
    auto read = [&](std::size_t offset, auto& value) {
        if (offset + sizeof(value) > data.size()) return false;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return true;
    };
    switch (type) {
        case 0: {
            int value = 0;
            return read(0, value) ? std::to_string(value) : std::string{};
        }
        case 1: {
            float value = 0.0f;
            return read(0, value) ? ui_format_number(value, 4) : std::string{};
        }
        case 2: {
            double value = 0.0;
            return read(0, value)
                       ? ui_format_number(static_cast<float>(value), 4)
                       : std::string{};
        }
        case 3: {
            bool value = false;
            return read(0, value) ? (value ? "true" : "false") : std::string{};
        }
        case 4:
            return data.empty() ? std::string{} :
                   std::string(reinterpret_cast<const char*>(data.data()),
                               strnlen(reinterpret_cast<const char*>(data.data()),
                                       data.size()));
        case 5:
        case 6:
        case 7:
        case 8: {
            const int count = type == 5 ? 2 : type == 6 ? 3 : 4;
            std::string out;
            for (int i = 0; i < count; ++i) {
                float value = 0.0f;
                if (!read(sizeof(float) * static_cast<std::size_t>(i), value)) break;
                if (!out.empty()) out += ", ";
                out += ui_format_number(value, 3);
            }
            return out;
        }
        case 9: {
            std::string out;
            for (int i = 0; i < 4; ++i) {
                float value = 0.0f;
                if (!read(sizeof(float) * static_cast<std::size_t>(i), value)) break;
                if (!out.empty()) out += ", ";
                out += ui_format_number(value, 3);
            }
            return out;
        }
        case 10: {
            std::string out;
            for (int i = 0; i < 4; ++i) {
                float value = 0.0f;
                if (!read(sizeof(float) * static_cast<std::size_t>(i), value)) break;
                if (!out.empty()) out += ", ";
                out += ui_format_number(value, 3);
            }
            return out;
        }
        case 11: {
            int value = 0;
            return read(0, value) ? std::to_string(value) : std::string{};
        }
        default:
            return {};
    }
}

void refresh_reflection_rows(IdeUI& ide) {
    if (!ide.reflection_grid) return;
    ide.reflection_grid->rows.clear();
    ide.reflection_grid->selected = -1;
    ide.reflection_grid->editing = false;
    ide.reflection_grid->edit_buffer.clear();
    const GEntityHandle selected = GEntity_GetSelected();
    if (!selected) {
        ide.root->runtime().invalidate();
        return;
    }
    const int component_count = GComponent_GetCount(selected);
    for (int component = 0; component < component_count; ++component) {
        uint64_t type_hash = 0;
        if (GComponent_GetTypeHashAt(selected, component, &type_hash) != 0) continue;
        char full_type[256] = {};
        GComponent_GetTypeNameAt(selected, component, full_type, sizeof(full_type));
        std::string type_name = full_type;
        const std::size_t separator = type_name.rfind("::");
        if (separator != std::string::npos) type_name = type_name.substr(separator + 2);
        const int property_count = GComponent_GetPropertyCount(selected, type_hash);
        for (int property = 0; property < property_count; ++property) {
            char property_name[128] = {};
            int type = 0;
            int size = 0;
            if (GComponent_GetPropertyInfo(selected, type_hash, property,
                                           property_name, sizeof(property_name),
                                           &type, &size) != 0 ||
                size <= 0 || size > 1024) {
                continue;
            }
            std::vector<uint8_t> value(static_cast<std::size_t>(size));
            if (GComponent_GetProperty(selected, type_hash, property_name,
                                       value.data(), size) != 0) {
                continue;
            }
            UIPropertyKind kind = UIPropertyKind::String;
            if (type == 0) kind = UIPropertyKind::Integer;
            else if (type == 1 || type == 2) kind = UIPropertyKind::Float;
            else if (type == 3) kind = UIPropertyKind::Boolean;
            else if (type == 5) kind = UIPropertyKind::Vector2;
            else if (type == 6) kind = UIPropertyKind::Vector3;
            else if (type == 8) kind = UIPropertyKind::Vector4;
            else if (type == 11) kind = UIPropertyKind::Enum;
            ide.reflection_grid->rows.push_back({
                type_name + "." + property_name,
                format_property_value(type, value),
                property_name,
                type_hash,
                kind,
                0.0f,
                0.0f,
                false,
            });
            if (ide.reflection_grid->rows.size() >= 512) break;
        }
        if (ide.reflection_grid->rows.size() >= 512) break;
    }
    if (!ide.reflection_grid->rows.empty()) ide.reflection_grid->selected = 0;
    ide.root->runtime().invalidate();
}

void update_inspector(IdeUI& ide) {
    const GEntityHandle selected = GEntity_GetSelected();
    if (!selected) {
        ide.inspector_name->text = "No selection";
        refresh_reflection_rows(ide);
        return;
    }
    char name[256] = {};
    GEntity_GetName(selected, name, sizeof(name));
    ide.inspector_name->text = name;

    GVec3 pos{};
    if (GEntity_GetLocalPosition(selected, &pos)) {
        char text[64];
        std::snprintf(text, sizeof(text), "%.3f", pos.x);
        ide.position_x->text = text;
        std::snprintf(text, sizeof(text), "%.3f", pos.y);
        ide.position_y->text = text;
        std::snprintf(text, sizeof(text), "%.3f", pos.z);
        ide.position_z->text = text;
    }
    refresh_reflection_rows(ide);
}

void apply_reflection_commit(IdeUI& ide) {
    if (!ide.reflection_grid || ide.reflection_grid->committed < 0) return;
    const int index = ide.reflection_grid->committed;
    ide.reflection_grid->committed = -1;
    if (index < 0 ||
        index >= static_cast<int>(ide.reflection_grid->rows.size())) return;
    const UIPropertyRow& row =
        ide.reflection_grid->rows[static_cast<std::size_t>(index)];
    const GEntityHandle selected = GEntity_GetSelected();
    if (!selected || row.read_only) return;

    int property_size = 0;
    char property_name[128] = {};
    int property_type = 0;
    const int property_count = GComponent_GetPropertyCount(selected, row.owner_id);
    bool found = false;
    for (int i = 0; i < property_count; ++i) {
        int size = 0;
        if (GComponent_GetPropertyInfo(selected, row.owner_id, i,
                                       property_name, sizeof(property_name),
                                       &property_type, &size) == 0 &&
            row.property_id == property_name) {
            property_size = size;
            found = true;
            break;
        }
    }
    if (!found || property_size <= 0 || property_size > 1024) return;
    std::vector<uint8_t> value(static_cast<std::size_t>(property_size));
    if (GComponent_GetProperty(selected, row.owner_id, row.property_id.c_str(),
                               value.data(), property_size) != 0) {
        return;
    }

    auto write = [&](std::size_t offset, const auto& source) {
        if (offset + sizeof(source) <= value.size()) {
            std::memcpy(value.data() + offset, &source, sizeof(source));
        }
    };
    switch (row.kind) {
        case UIPropertyKind::Boolean: {
            const bool parsed = row.value == "true" || row.value == "1";
            write(0, parsed);
            break;
        }
        case UIPropertyKind::Integer:
        case UIPropertyKind::Enum: {
            const long long parsed = std::strtoll(row.value.c_str(), nullptr, 10);
            if (property_size >= static_cast<int>(sizeof(int64_t))) {
                write(0, parsed);
            } else {
                const int value32 = static_cast<int>(parsed);
                write(0, value32);
            }
            break;
        }
        case UIPropertyKind::Float: {
            const double parsed = std::strtod(row.value.c_str(), nullptr);
            if (property_size >= static_cast<int>(sizeof(double))) {
                write(0, parsed);
            } else {
                const float value32 = static_cast<float>(parsed);
                write(0, value32);
            }
            break;
        }
        case UIPropertyKind::Vector2:
        case UIPropertyKind::Vector3:
        case UIPropertyKind::Vector4: {
            std::string normalized = row.value;
            for (char& character : normalized) {
                if (character == ',') character = ' ';
            }
            std::istringstream stream(normalized);
            float component = 0.0f;
            std::size_t offset = 0;
            while (stream >> component && offset + sizeof(float) <= value.size()) {
                write(offset, component);
                offset += sizeof(float);
            }
            break;
        }
        case UIPropertyKind::String:
            std::fill(value.begin(), value.end(), uint8_t{0});
            std::memcpy(value.data(), row.value.data(),
                        std::min(value.size(), row.value.size()));
            break;
    }
    GEdit_SetProperty(selected, row.owner_id, row.property_id.c_str(),
                      value.data(), static_cast<int>(value.size()));
    ide.inspector_refresh_delay = 2;
    ide.status->text = "Property change queued";
}

constexpr int kCommandPalette = 1;
constexpr int kSaveLayout = 2;
constexpr int kReloadScene = 3;
constexpr int kTogglePlay = 4;
constexpr int kUndo = 5;
constexpr int kRedo = 6;
constexpr int kClosePalette = 7;
constexpr int kSaveScene = 10;
constexpr int kSaveActiveDocument = 11;

void install_shortcuts(IdeUI& ide) {
    if (!ide.root) return;
    auto& runtime = ide.root->runtime();
    runtime.add_shortcut(80, 3, kCommandPalette);
    runtime.add_shortcut(83, 1, kSaveScene);
    runtime.add_shortcut(83, 5, kSaveLayout);
    runtime.add_shortcut(82, 1, kReloadScene);
    runtime.add_shortcut(294, 0, kTogglePlay);
    runtime.add_shortcut(90, 1, kUndo);
    runtime.add_shortcut(89, 1, kRedo);
    runtime.add_shortcut(256, 0, kClosePalette);
}

std::string ide_layout_path(const std::string& project_root) {
    return (std::filesystem::path(project_root) / ".gryce" / "ide_layout.json").string();
}

void save_ide_layout(const IdeUI& ide, const std::string& path) {
    if (!ide.dock) return;
    nlohmann::json layout;
    layout["dock"] = {
        {"top_height", ide.dock->top_height},
        {"bottom_height", ide.dock->bottom_height},
        {"left_width", ide.dock->left_width},
        {"right_width", ide.dock->right_width},
        {"gap", ide.dock->gap},
    };
    layout["panels"] = nlohmann::json::array();
    const std::pair<const char*, const UIDockPanel*> panels[] = {
        {"top", ide.top_bar},
        {"left", ide.hierarchy_panel},
        {"center", ide.center_panel},
        {"right", ide.inspector_panel},
        {"bottom", ide.bottom_panel},
    };
    for (const auto& [role, panel] : panels) {
        if (!panel) continue;
        layout["panels"].push_back({
            {"role", role},
            {"title", panel->title},
            {"side", static_cast<int>(panel->side)},
        });
    }
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), error);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << layout.dump(2);
}

bool load_ide_layout(IdeUI& ide, const std::string& path) {
    if (!ide.dock) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    try {
        nlohmann::json layout;
        in >> layout;
        const auto dock = layout.value("dock", nlohmann::json::object());
        ide.dock->top_height = std::clamp(
            dock.value("top_height", ide.dock->top_height), 40.0f, 220.0f);
        ide.dock->bottom_height = std::clamp(
            dock.value("bottom_height", ide.dock->bottom_height), 80.0f, 420.0f);
        ide.dock->left_width = std::clamp(
            dock.value("left_width", ide.dock->left_width), 140.0f, 560.0f);
        ide.dock->right_width = std::clamp(
            dock.value("right_width", ide.dock->right_width), 140.0f, 560.0f);
        ide.dock->gap = std::clamp(
            dock.value("gap", ide.dock->gap), 0.0f, 16.0f);
        for (const auto& entry : layout.value("panels", nlohmann::json::array())) {
            const std::string role = entry.value("role", "");
            UIDockPanel* panel = nullptr;
            if (role == "top") panel = ide.top_bar;
            else if (role == "left") panel = ide.hierarchy_panel;
            else if (role == "center") panel = ide.center_panel;
            else if (role == "right") panel = ide.inspector_panel;
            else if (role == "bottom") panel = ide.bottom_panel;
            if (!panel) continue;
            panel->title = entry.value("title", panel->title);
            panel->side = static_cast<UIDockSide>(
                std::clamp(entry.value("side", static_cast<int>(panel->side)), 0, 4));
        }
        int w = 1280;
        int h = 720;
        GWindow_GetSize(&w, &h);
        layout_ide(ide, w, h);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void reset_ide_layout(IdeUI& ide) {
    if (!ide.dock) return;
    ide.dock->top_height = 100.0f;
    ide.dock->bottom_height = 200.0f;
    ide.dock->left_width = 260.0f;
    ide.dock->right_width = 280.0f;
    ide.dock->gap = 4.0f;
    if (ide.top_bar) ide.top_bar->side = UIDockSide::Top;
    if (ide.hierarchy_panel) ide.hierarchy_panel->side = UIDockSide::Left;
    if (ide.center_panel) ide.center_panel->side = UIDockSide::Center;
    if (ide.inspector_panel) ide.inspector_panel->side = UIDockSide::Right;
    if (ide.bottom_panel) ide.bottom_panel->side = UIDockSide::Bottom;
    int w = 1280;
    int h = 720;
    GWindow_GetSize(&w, &h);
    layout_ide(ide, w, h);
}

void set_view_mode(IdeUI& ide, bool view_2d) {
    ide.view_2d = view_2d;
    ide.split_view = false;
    ide.center_tab_index = view_2d ? 1 : 0;
    if (ide.center_tabs) ide.center_tabs->selected = ide.center_tab_index;
    if (ide.viewport) ide.viewport->visible = true;
    if (ide.viewport_2d) ide.viewport_2d->visible = false;
    if (ide.game_view) ide.game_view->visible = false;
    if (ide.code_editor) ide.code_editor->visible = false;
    ide.view_mode->text = ide.view_2d ? "3D View" : "2D View";
    GRender_SetCanvas2DViewportOutput(ide.view_2d);
    ide.root->runtime().invalidate();
}

void set_split_view(IdeUI& ide, bool split) {
    ide.split_view = split;
    if (split) {
        ide.center_tab_index = 2;
        if (ide.center_tabs) ide.center_tabs->selected = 2;
        if (ide.viewport) ide.viewport->visible = true;
        if (ide.viewport_2d) ide.viewport_2d->visible = true;
        if (ide.game_view) ide.game_view->visible = false;
        if (ide.code_editor) ide.code_editor->visible = false;
        GRender_SetCanvas2DViewportOutput(true);
        ide.status->text = "3D + 2D split view";
    } else {
        set_view_mode(ide, ide.view_2d);
    }
    ide.root->runtime().invalidate();
}

void activate_center_tab(IdeUI& ide, int index) {
    if (index >= 0 && index <= 1) {
        set_view_mode(ide, index == 1);
        return;
    }
    if (index == 2) {
        set_split_view(ide, true);
        return;
    }
    if (index == 3) {
        ide.center_tab_index = 3;
        ide.split_view = false;
        if (ide.center_tabs) ide.center_tabs->selected = 3;
        if (ide.viewport) ide.viewport->visible = false;
        if (ide.viewport_2d) ide.viewport_2d->visible = false;
        if (ide.game_view) ide.game_view->visible = true;
        if (ide.code_editor) ide.code_editor->visible = false;
        GRender_SetCanvas2DViewportOutput(false);
        ide.status->text = "Game view";
        ide.root->runtime().invalidate();
        return;
    }
    ide.center_tab_index = 4;
    if (ide.center_tabs) ide.center_tabs->selected = 4;
    if (ide.viewport) ide.viewport->visible = false;
    if (ide.viewport_2d) ide.viewport_2d->visible = false;
    if (ide.game_view) ide.game_view->visible = false;
    if (ide.code_editor) ide.code_editor->visible = true;
    ide.status->text = ide.code_editor && ide.code_editor->dirty
                           ? "Script modified"
                           : "Script editor";
    ide.root->runtime().invalidate();
}

void set_palette_open(IdeUI& ide, bool open) {
    if (!ide.palette) return;
    ide.palette->visible = open;
    ide.palette->receives_input = open;
    if (open) {
        ide.palette_search->set_query({});
        ide.palette_filter.clear();
        ide.palette_commands.clear();
        ide.palette_list->selected = 0;
        ide.palette_list->activated = -1;
        int w = 1280;
        int h = 720;
        GWindow_GetSize(&w, &h);
        layout_ide(ide, w, h);
    } else {
        ide.palette_search->text.clear();
        ide.palette_search->query.clear();
    }
    ide.root->runtime().invalidate();
}

void refresh_palette(IdeUI& ide) {
    if (!ide.palette || !ide.palette->visible || !ide.palette_search) return;
    if (ide.palette_filter == ide.palette_search->query) return;
    ide.palette_filter = ide.palette_search->query;
    std::string needle = ide.palette_filter;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::pair<const char*, int> commands[] = {
        {"Save Active Document", kSaveActiveDocument},
        {"Save Scene", kSaveScene},
        {"Reload Scene", kReloadScene},
        {"Toggle 2D / 3D View", 8},
        {"Undo", kUndo},
        {"Redo", kRedo},
        {"Reset Layout", 9},
        {"Save Layout", kSaveLayout},
    };
    std::string filtered;
    std::vector<int> filtered_commands;
    for (const auto& [name, command] : commands) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!needle.empty() && lower.find(needle) == std::string::npos) continue;
        if (!filtered.empty()) filtered.push_back('\n');
        filtered += name;
        filtered_commands.push_back(command);
    }
    if (filtered.empty()) filtered = "No matching commands";
    ide.palette_list->items = std::move(filtered);
    ide.palette_commands = std::move(filtered_commands);
    ide.palette_list->selected = 0;
    ide.root->runtime().invalidate();
}

void run_palette_command(IdeUI& ide, int command, bool& reload_requested,
                         const std::string& layout_path,
                         const std::string& scene_path) {
    switch (command) {
        case kReloadScene:
            reload_requested = true;
            break;
        case kSaveLayout:
            save_ide_layout(ide, layout_path);
            ide.toast->show("Layout saved", 2.0f,
                            gryce_engine::components::UIToastSeverity::Success);
            break;
        case kSaveScene:
            GEdit_SaveScene(scene_path.c_str());
            ide.toast->show("Scene save queued", 2.0f,
                            gryce_engine::components::UIToastSeverity::Success);
            break;
        case kSaveActiveDocument:
            if (ide.center_tab_index == 4 && ide.code_editor) {
                const bool saved = ide.code_editor->save_file();
                ide.toast->show(saved ? "Document saved" : "Save failed",
                                2.0f, saved
                                    ? gryce_engine::components::UIToastSeverity::Success
                                    : gryce_engine::components::UIToastSeverity::Error);
            } else {
                GEdit_SaveScene(scene_path.c_str());
            }
            break;
        case kTogglePlay: {
            const int mode = GCore_IsPlaying() ? 0 : 1;
            GEdit_SetPlayMode(mode);
            ide.play->text = mode ? "Stop" : "Play";
            break;
        }
        case kUndo:
            GCore_Undo();
            ide.inspector_refresh_delay = 2;
            break;
        case kRedo:
            GCore_Redo();
            ide.inspector_refresh_delay = 2;
            break;
        case 8:
            set_view_mode(ide, !ide.view_2d);
            break;
        case 9:
            reset_ide_layout(ide);
            ide.toast->show("Layout reset", 2.0f);
            break;
        default:
            break;
    }
}

void update_application_menu(IdeUI& ide) {
    if (!ide.menu_bar || !ide.file_menu) return;
    if (!ide.menu_bar->open) {
        ide.file_menu->set_open(false);
        return;
    }
    switch (ide.menu_bar->selected) {
        case 0:
            ide.file_menu->items =
                "Save Scene\nSave Layout\nReset Layout\nExit";
            break;
        case 1:
            ide.file_menu->items = "Undo\nRedo";
            break;
        case 2:
            ide.file_menu->items = "Toggle 2D / 3D View\nReset Layout";
            break;
        case 3:
            ide.file_menu->items = "Play / Stop\nReload Scene";
            break;
        default:
            ide.file_menu->items = "GryceIDE - GryceUI Debug Host";
            break;
    }
    if (!ide.file_menu->visible) ide.file_menu->set_open(true);
    int w = 1280;
    int h = 720;
    GWindow_GetSize(&w, &h);
    layout_ide(ide, w, h);
}

void execute_menu_command(IdeUI& ide, int menu, int item,
                          bool& reload_requested, const std::string& layout_path,
                          const std::string& scene_path) {
    if (menu == 0) {
        if (item == 0) {
            GEdit_SaveScene(scene_path.c_str());
            ide.toast->show("Scene save queued", 2.0f,
                            gryce_engine::components::UIToastSeverity::Success);
        } else if (item == 1) {
            save_ide_layout(ide, layout_path);
            ide.toast->show("Layout saved", 2.0f,
                            gryce_engine::components::UIToastSeverity::Success);
        } else if (item == 2) {
            reset_ide_layout(ide);
            ide.toast->show("Layout reset", 2.0f);
        } else if (item == 3) {
            GWindow_RequestClose();
        }
    } else if (menu == 1) {
        if (item == 0) GCore_Undo();
        if (item == 1) GCore_Redo();
        ide.inspector_refresh_delay = 2;
    } else if (menu == 2) {
        if (item == 0) set_view_mode(ide, !ide.view_2d);
        if (item == 1) reset_ide_layout(ide);
    } else if (menu == 3) {
        if (item == 0) {
            const int mode = GCore_IsPlaying() ? 0 : 1;
            GEdit_SetPlayMode(mode);
            ide.play->text = mode ? "Stop" : "Play";
        }
        if (item == 1) reload_requested = true;
    }
}

void process_ui_actions(IdeUI& ide, bool& reload_requested,
                        const std::string& layout_path,
                        const std::string& scene_path) {
    apply_reflection_commit(ide);
    update_application_menu(ide);
    if (ide.file_menu->command >= 0) {
        const int command = ide.file_menu->command;
        ide.file_menu->command = -1;
        execute_menu_command(ide, ide.menu_bar->selected, command,
                             reload_requested, layout_path, scene_path);
        ide.menu_bar->open = false;
    }
    if (ide.palette && ide.palette->visible) {
        refresh_palette(ide);
        if (ide.palette_list->activated >= 0) {
            const int command_index = ide.palette_list->activated;
            ide.palette_list->activated = -1;
            set_palette_open(ide, false);
            const int command =
                command_index >= 0 &&
                command_index < static_cast<int>(ide.palette_commands.size())
                    ? ide.palette_commands[static_cast<std::size_t>(command_index)]
                    : 0;
            run_palette_command(ide, command, reload_requested,
                                layout_path, scene_path);
        }
    }
    const int shortcut = ide.root->runtime().take_shortcut_command();
    if (shortcut == kCommandPalette) set_palette_open(ide, !ide.palette->visible);
    else if (shortcut == kClosePalette) set_palette_open(ide, false);
    else if (shortcut == kReloadScene) reload_requested = true;
    else if (shortcut == kSaveScene) {
        run_palette_command(ide, kSaveActiveDocument, reload_requested,
                            layout_path, scene_path);
    }
    else if (shortcut == kSaveLayout) {
        save_ide_layout(ide, layout_path);
        ide.toast->show("Layout saved", 2.0f,
                        gryce_engine::components::UIToastSeverity::Success);
    } else if (shortcut != 0) {
        run_palette_command(ide, shortcut, reload_requested,
                            layout_path, scene_path);
    }
    if (ide.bottom_tabs && ide.bottom_content) {
        ide.asset_grid->visible = ide.bottom_tabs->selected == 0;
        ide.console_view->visible = ide.bottom_tabs->selected == 1;
        ide.problems_view->visible = ide.bottom_tabs->selected == 2;
        switch (ide.bottom_tabs->selected) {
            case 0:
                break;
            case 1:
                break;
            case 2:
                break;
            default:
                break;
        }
    }
    if (ide.asset_grid && ide.asset_grid->visible &&
        ide.asset_grid->activated >= 0) {
        const int index = ide.asset_grid->activated;
        ide.asset_grid->activated = -1;
        const auto entries = gryce_engine::components::ui_split_lines(
            ide.asset_grid->items);
        if (index >= 0 && index < static_cast<int>(entries.size())) {
            const std::string& relative = entries[static_cast<std::size_t>(index)];
            if (relative.size() >= 3 &&
                relative.compare(relative.size() - 3, 3, ".js") == 0) {
                const std::string path =
                    (std::filesystem::path(ide.project_root) / relative).string();
                if (ide.code_editor->load_file(path)) {
                    activate_center_tab(ide, 4);
                    append_console(ide, "[info] Opened " + relative);
                } else {
                    append_console(ide, "[error] Cannot open " + relative);
                }
            } else {
                append_console(ide, "[info] Selected " + relative);
            }
        }
    }
    if (ide.center_tabs && ide.center_tabs->selected != ide.center_tab_index) {
        activate_center_tab(ide, ide.center_tabs->selected);
    }
    if (ide.hierarchy_tree && ide.hierarchy_tree->selection_changed) {
        ide.hierarchy_tree->selection_changed = false;
        const int index = ide.hierarchy_tree->selected;
        if (index >= 0 &&
            index < static_cast<int>(ide.hierarchy_handles.size())) {
            GEdit_SelectEntity(ide.hierarchy_handles[static_cast<std::size_t>(index)]);
            ide.inspector_refresh_delay = 2;
        }
    }
    if (ide.reload->clicked) {
        ide.reload->clicked = false;
        reload_requested = true;
    }
    if (ide.view_mode->clicked) {
        ide.view_mode->clicked = false;
        set_view_mode(ide, !ide.view_2d);
    }
    if (ide.undo->clicked) {
        ide.undo->clicked = false;
        GCore_Undo();
        ide.inspector_refresh_delay = 2;
        ide.hierarchy_refresh_delay = 2;
    }
    if (ide.redo->clicked) {
        ide.redo->clicked = false;
        GCore_Redo();
        ide.inspector_refresh_delay = 2;
        ide.hierarchy_refresh_delay = 2;
    }
    if (ide.play->clicked) {
        ide.play->clicked = false;
        const int mode = GCore_IsPlaying() ? 0 : 1;
        GEdit_SetPlayMode(mode);
        ide.play->text = mode ? "Stop" : "Play";
        ide.pause->text = "Pause";
    }
    if (ide.pause->clicked) {
        ide.pause->clicked = false;
        GEdit_SetPlayMode(2);
        ide.pause->text = GCore_IsPaused() ? "Pause" : "Resume";
        ide.status->text = GCore_IsPaused() ? "Playback resumed" : "Playback paused";
    }
    if (ide.step->clicked) {
        ide.step->clicked = false;
        if (GCore_IsPlaying() && GCore_IsPaused()) GEdit_StepFrame(1);
        else ide.status->text = "Pause playback before stepping";
    }
    if (ide.apply->clicked) {
        ide.apply->clicked = false;
        const GEntityHandle selected = GEntity_GetSelected();
        if (selected) {
            GVec3 pos{};
            pos.x = std::strtof(ide.position_x->text.c_str(), nullptr);
            pos.y = std::strtof(ide.position_y->text.c_str(), nullptr);
            pos.z = std::strtof(ide.position_z->text.c_str(), nullptr);
            GEdit_SetTransform(selected, &pos, nullptr, nullptr);
            ide.status->text = "Transform queued";
        }
    }
    if (ide.new_entity->clicked) {
        ide.new_entity->clicked = false;
        GEdit_CreateEntity("Entity", 0);
        ide.hierarchy_refresh_delay = 2;
        append_console(ide, "[info] Entity creation queued");
        ide.toast->show("Entity creation queued", 1.8f);
    }
    if (ide.duplicate_entity->clicked) {
        ide.duplicate_entity->clicked = false;
        duplicate_selected_entity(ide);
    }
    if (ide.delete_entity->clicked) {
        ide.delete_entity->clicked = false;
        const GEntityHandle selected = GEntity_GetSelected();
        if (selected) {
            GEdit_DestroyEntity(selected);
            ide.hierarchy_refresh_delay = 2;
            append_console(ide, "[info] Entity deletion queued");
            ide.toast->show("Entity deletion queued", 1.8f);
        }
    }
    if (ide.rename_entity->clicked) {
        ide.rename_entity->clicked = false;
        const GEntityHandle selected = GEntity_GetSelected();
        if (selected && !ide.inspector_name->text.empty()) {
            GEdit_RenameEntity(selected, ide.inspector_name->text.c_str());
            ide.hierarchy_refresh_delay = 2;
            ide.status->text = "Rename queued";
            append_console(ide, "[info] Rename queued");
        }
    }
    if (ide.save_scene->clicked) {
        ide.save_scene->clicked = false;
        GEdit_SaveScene(scene_path.c_str());
        append_console(ide, "[info] Scene save queued");
        ide.toast->show("Scene save queued", 2.0f,
                        gryce_engine::components::UIToastSeverity::Success);
    }
}

void update_viewport_texture(IdeUI& ide) {
    auto update = [&](UIViewportPanel* panel, bool mode_2d) {
        if (!panel) return;
        GRenderTextureInfo info{};
        const int ok = mode_2d
                           ? GRender_GetCanvas2DViewTextureInfo(&info)
                           : GRender_GetViewportTextureInfo(&info);
        if (!ok) return;
        if (panel->texture.index == info.index &&
            panel->texture.generation == info.generation &&
            panel->texture.width == info.width &&
            panel->texture.height == info.height) {
            return;
        }
        panel->texture = {info.index, info.generation, info.width, info.height};
        std::printf("[ide] viewport texture mode=%s index=%u generation=%u %dx%d\n",
                    mode_2d ? "2d" : "3d", panel->texture.index,
                    panel->texture.generation, panel->texture.width,
                    panel->texture.height);
        ide.root->runtime().invalidate();
    };
    update(ide.viewport, ide.split_view ? false : ide.view_2d);
    if (ide.split_view) update(ide.viewport_2d, true);
    if (ide.center_tab_index == 3 && ide.game_view) {
        GRenderTextureInfo info{};
        if (GRender_GetGameViewTextureInfo(&info)) {
            ide.game_view->texture = {
                info.index, info.generation, info.width, info.height,
            };
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    const Options options = parse_options(argc, argv);
    const GRenderAPI api = parse_api(options.api);
    const std::string project_root = std::filesystem::absolute(options.project).string();
    const std::string layout_path = ide_layout_path(project_root);

    GCore_SetAutoLoadMainScene(false);
    GCoreInitDesc core_desc{};
    core_desc.version = sizeof(GCoreInitDesc);
    core_desc.project_root = project_root.c_str();
    core_desc.enable_reflection = true;
    if (GCore_Init(&core_desc) != 0) {
        std::fprintf(stderr, "[ide] GCore_Init failed\n");
        return 1;
    }
    if (!options.scene.empty()) GScene_Load(options.scene.c_str());

    if (GWindow_CreateEx("GryceIDE", options.width, options.height,
                         GWINDOW_MODE_WINDOWED, api) != 0) {
        std::fprintf(stderr, "[ide] window creation failed\n");
        GCore_Shutdown();
        return 1;
    }
    GRender_SetDebugUI(false);
    GRender_SetFPSOverlay(false);
    GRender_SetVSync(false);
    GRender_SetViewportOutput(true);

    GRenderInitDesc render_desc{};
    render_desc.version = sizeof(GRenderInitDesc);
    render_desc.native_window = GWindow_GetRenderHandle();
    render_desc.api = api;
    render_desc.viewport_w = options.width;
    render_desc.viewport_h = options.height;
    render_desc.sync_mode = false;
    if (GRender_Init(&render_desc) != 0) {
        std::fprintf(stderr, "[ide] GRender_Init failed\n");
        GWindow_Destroy();
        GCore_Shutdown();
        return 1;
    }

    IdeUI ide;
    if (!build_ui(ide, options.width, options.height)) {
        std::fprintf(stderr, "[ide] UI construction failed\n");
        GRender_Shutdown();
        GWindow_Destroy();
        GCore_Shutdown();
        return 1;
    }
    ide.project_root = project_root;
    refresh_assets(ide);
    if (!ide.hierarchy_handles.empty()) {
        GEdit_SelectEntity(ide.hierarchy_handles.front());
        ide.inspector_refresh_delay = 2;
    }
    if (load_ide_layout(ide, layout_path)) {
        ide.status->text = "Layout restored";
        ide.status->right_text = "GryceIDE | Saved layout";
    }
    if (ide.code_editor) {
        ide.code_editor->load_file(
            (std::filesystem::path(project_root) / "main.js").string());
    }
    if (options.split) {
        set_split_view(ide, true);
    } else if (options.game) {
        activate_center_tab(ide, 3);
    } else if (options.script) {
        activate_center_tab(ide, 4);
    } else {
        set_view_mode(ide, options.view_2d);
    }
    ide.bottom_tabs->selected = options.bottom_tab;
    ide.asset_grid->visible = options.bottom_tab == 0;
    ide.console_view->visible = options.bottom_tab == 1;
    ide.problems_view->visible = options.bottom_tab == 2;
    gryce_engine::ui::set_clipboard_callbacks(read_system_clipboard,
                                               write_system_clipboard);
    install_shortcuts(ide);
    if (options.palette) set_palette_open(ide, true);

    int frames = 0;
    int flush_frames = -1;
    bool reload_requested = false;
    int window_width = options.width;
    int window_height = options.height;
    auto last = std::chrono::steady_clock::now();
    while (!GWindow_ShouldClose()) {
        GWindow_PollEvents();
        GInput_SyncToCore();
        int current_width = window_width;
        int current_height = window_height;
        GWindow_GetSize(&current_width, &current_height);
        if (current_width != window_width || current_height != window_height) {
            window_width = current_width;
            window_height = current_height;
            layout_ide(ide, window_width, window_height);
        }
        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        dt = std::max(0.0005f, std::min(dt, 0.1f));

        GCore_BeginFrame(dt);
        if (ide.hierarchy_refresh_delay > 0 &&
            --ide.hierarchy_refresh_delay == 0) {
            refresh_hierarchy(ide);
        }
        if (ide.inspector_refresh_delay > 0 &&
            --ide.inspector_refresh_delay == 0) {
            refresh_reflection_rows(ide);
        }
        update_viewport_texture(ide);
        GRender_BeginFrame();
        if (ide.center_tab_index == 3) GRender_RenderGameView();
        GRender_RenderWorld();
        process_ui_actions(ide, reload_requested, layout_path, options.scene);
        ide.toast->update(dt);
        GRender_EndFrame();
        GCore_EndFrame();
        ++frames;

        if (reload_requested) {
            const int center_tab = ide.center_tab_index;
            GScene_Load(options.scene.c_str());
            build_ui(ide, window_width, window_height);
            ide.project_root = project_root;
            refresh_assets(ide);
            load_ide_layout(ide, layout_path);
            install_shortcuts(ide);
            if (ide.code_editor) {
                ide.code_editor->load_file(
                    (std::filesystem::path(project_root) / "main.js").string());
            }
            activate_center_tab(ide, center_tab);
            if (!ide.hierarchy_handles.empty()) {
                GEdit_SelectEntity(ide.hierarchy_handles.front());
                ide.inspector_refresh_delay = 2;
            }
            reload_requested = false;
        }

        if (flush_frames < 0 && options.frames > 0 && frames >= options.frames) {
            if (!options.shot.empty()) GRender_SaveScreenshot(options.shot.c_str());
            flush_frames = options.shot.empty() ? 0 : 30;
        }
        if (!options.shot.empty()) {
            std::error_code error;
            if (std::filesystem::exists(options.shot, error)) break;
        }
        if (flush_frames == 0) break;
        if (flush_frames > 0 && --flush_frames == 0) break;
    }

    save_ide_layout(ide, layout_path);
    GCore_Shutdown();
    GRender_Shutdown();
    GWindow_Destroy();
    std::printf("[ide] exit frames=%d project=%s\n", frames, project_root.c_str());
    return 0;
}
