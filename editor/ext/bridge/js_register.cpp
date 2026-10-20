// ===========================================================================
// editor/ext/bridge/js_register.cpp — 把 ext.h 的声明挂到 engine.editor.*
//
// 这是唯一出现"JS 名字"的地方。ext.h 用下划线（C 侧习惯），这里映射成
// 驼峰（JS 侧习惯），一一对应、不加包装逻辑。
//
// 为什么全部平铺在 engine.editor 下：扩展 ABI 的 add_function 只能挂
// `engine.<ns>` 一级，二级命名空间要扩展自己造对象，而函数表里没有
// "新建对象"这个能力。用前缀分组（view_* / gizmo_* / panel_*）替代，
// 代价是多打几个字符，好处是不用改 ABI。
//
// 注册会重放：main.js 热重载会重建 JSContext 并再次调用本入口。所以
// g_api / g_ctx 必须在入口里**每次刷新** —— 老指针仍可解引用，但它的
// js_ctx 指向已销毁的 context，经它注册的函数会装进死对象。
// ===========================================================================

#include "ext.h"

#include "bridge/js_args.h"

#include <string>

namespace {

using gryce::editor::bridge::Args;

GryceExtensionApi* g_api = nullptr;
void* g_ctx = nullptr;

GJsValue ret_num(double v) { return g_api->ret_number(g_ctx, v); }
GJsValue ret_bool(int v) { return g_api->ret_bool(g_ctx, v != 0); }
GJsValue ret_str(const char* s) { return g_api->ret_string(g_ctx, s ? s : ""); }
GJsValue ret_undef() { return g_api->undefined(g_ctx); }
GJsValue fail(const char* msg) { return g_api->throw_error(g_ctx, msg); }

Args args_of(int argc, const GJsValue* argv) { return Args(g_api, g_ctx, argc, argv); }

// 统一的"读一个句柄，读不到就抛错"助手。句柄类接口几乎都这个形状，
// 每处重写一遍判断会让漏判率随接口数量线性上升。
bool want_handle(const Args& a, int index, GEntityHandle* out, const char* sig) {
    if (a.handle(index, out)) return true;
    (void)sig;
    return false;
}

#define FN(name) GJsValue name(void*, GJsValue, int argc, const GJsValue* argv)

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

FN(js_version) {
    (void)argc;
    (void)argv;
    return ret_str(gryce::editor::version());
}

FN(js_tick) {
    Args a = args_of(argc, argv);
    double dt = 0.0;
    if (!a.number(0, &dt)) dt = 1.0 / 60.0;
    gryce::editor::tick(static_cast<float>(dt));
    return ret_str(gryce::editor::pending_events_json());
}

// ---------------------------------------------------------------------------
// 会话
// ---------------------------------------------------------------------------

FN(js_session) {
    (void)argc;
    (void)argv;
    return ret_str(gryce::editor::session_json());
}

FN(js_open) {
    Args a = args_of(argc, argv);
    std::string path;
    if (!a.str(0, &path)) return fail("editor.open(path) expects a path string");
    return ret_bool(gryce::editor::open(path.c_str()));
}

FN(js_close) {
    (void)argc;
    (void)argv;
    gryce::editor::close();
    return ret_undef();
}

FN(js_save) {
    Args a = args_of(argc, argv);
    std::string path;
    // 允许不传参数：表示保存到当前文档路径。
    if (argc > 0 && !a.str(0, &path)) return fail("editor.save([path]) expects a string");
    return ret_bool(gryce::editor::save(path.c_str()));
}

// ---------------------------------------------------------------------------
// 选择
// ---------------------------------------------------------------------------

FN(js_select) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    if (!want_handle(a, 0, &h, "editor.select(handle)")) {
        return fail("editor.select(handle) expects entity id (0 clears)");
    }
    return ret_bool(gryce::editor::select(h));
}

FN(js_select_add) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    if (!want_handle(a, 0, &h, "editor.selectAdd")) {
        return fail("editor.selectAdd(handle) expects entity id");
    }
    return ret_bool(gryce::editor::select_add(h));
}

FN(js_select_toggle) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    if (!want_handle(a, 0, &h, "editor.selectToggle")) {
        return fail("editor.selectToggle(handle) expects entity id");
    }
    return ret_bool(gryce::editor::select_toggle(h));
}

FN(js_select_remove) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    if (!want_handle(a, 0, &h, "editor.selectRemove")) {
        return fail("editor.selectRemove(handle) expects entity id");
    }
    return ret_bool(gryce::editor::select_remove(h));
}

FN(js_select_clear) {
    (void)argc;
    (void)argv;
    gryce::editor::select_clear();
    return ret_undef();
}

FN(js_selection) {
    (void)argc;
    (void)argv;
    return ret_str(gryce::editor::selection_json());
}

FN(js_delete_selection) {
    (void)argc;
    (void)argv;
    return ret_bool(gryce::editor::delete_selection());
}

FN(js_pick) {
    Args a = args_of(argc, argv);
    float x = 0.0f;
    float y = 0.0f;
    if (!a.number(0, &x) || !a.number(1, &y)) {
        return fail("editor.pick(x, y) expects screen coordinates");
    }
    return ret_num(static_cast<double>(gryce::editor::pick(x, y)));
}

FN(js_focus) {
    (void)argc;
    (void)argv;
    return ret_str(gryce::editor::focus_json());
}

// ---------------------------------------------------------------------------
// 编辑命令
// ---------------------------------------------------------------------------

FN(js_create) {
    Args a = args_of(argc, argv);
    std::string name = "Entity";
    GEntityHandle parent = 0;
    // 名字可省；parent 也可省（挂在场景根）。
    if (argc > 0) a.str(0, &name);
    if (argc > 1) a.handle(1, &parent);
    return ret_bool(gryce::editor::create(name.c_str(), parent));
}

FN(js_destroy) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    if (!want_handle(a, 0, &h, "editor.destroy")) {
        return fail("editor.destroy(handle) expects entity id");
    }
    return ret_bool(gryce::editor::destroy(h));
}

FN(js_rename) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    std::string name;
    if (!want_handle(a, 0, &h, "editor.rename") || !a.str(1, &name)) {
        return fail("editor.rename(handle, name) expects entity id and a non-empty name");
    }
    return ret_bool(gryce::editor::rename(h, name.c_str()));
}

FN(js_reparent) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    GEntityHandle parent = 0;
    if (!want_handle(a, 0, &h, "editor.reparent")) {
        return fail("editor.reparent(handle, parent) expects entity id");
    }
    if (argc > 1) a.handle(1, &parent);
    return ret_bool(gryce::editor::reparent(h, parent));
}

FN(js_set_position) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    if (!want_handle(a, 0, &h, "editor.setPosition")
        || !a.number(1, &x) || !a.number(2, &y) || !a.number(3, &z)) {
        return fail("editor.setPosition(handle, x, y, z) expects 4 arguments");
    }
    return ret_bool(gryce::editor::set_position(h, x, y, z));
}

FN(js_set_rotation) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    if (!want_handle(a, 0, &h, "editor.setRotation")
        || !a.number(1, &x) || !a.number(2, &y) || !a.number(3, &z)
        || !a.number(4, &w)) {
        return fail("editor.setRotation(handle, x, y, z, w) expects 5 arguments");
    }
    return ret_bool(gryce::editor::set_rotation(h, x, y, z, w));
}

FN(js_set_scale) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    float x = 1.0f, y = 1.0f, z = 1.0f;
    if (!want_handle(a, 0, &h, "editor.setScale")
        || !a.number(1, &x) || !a.number(2, &y) || !a.number(3, &z)) {
        return fail("editor.setScale(handle, x, y, z) expects 4 arguments");
    }
    return ret_bool(gryce::editor::set_scale(h, x, y, z));
}

FN(js_translate) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    if (!want_handle(a, 0, &h, "editor.translate")
        || !a.number(1, &x) || !a.number(2, &y) || !a.number(3, &z)) {
        return fail("editor.translate(handle, dx, dy, dz) expects 4 arguments");
    }
    return ret_bool(gryce::editor::translate(h, x, y, z));
}

FN(js_set_property_number) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    uint64_t hash = 0;
    std::string prop;
    double value = 0.0;
    if (!want_handle(a, 0, &h, "editor.setProperty")
        || !a.u64(1, &hash) || !a.str(2, &prop) || !a.number(3, &value)) {
        return fail("editor.setProperty(handle, typeHash, propName, value) "
                    "expects (id, hash, string, number)");
    }
    return ret_bool(gryce::editor::set_property_number(h, hash, prop.c_str(), value));
}

FN(js_set_property_bool) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    uint64_t hash = 0;
    std::string prop;
    bool value = false;
    if (!want_handle(a, 0, &h, "editor.setPropertyBool")
        || !a.u64(1, &hash) || !a.str(2, &prop) || !a.boolean(3, &value)) {
        return fail("editor.setPropertyBool(handle, typeHash, propName, value) "
                    "expects (id, hash, string, bool)");
    }
    return ret_bool(gryce::editor::set_property_bool(h, hash, prop.c_str(), value ? 1 : 0));
}

FN(js_add_component) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    std::string type_name;
    if (!want_handle(a, 0, &h, "editor.addComponent") || !a.str(1, &type_name)) {
        return fail("editor.addComponent(handle, typeName) expects entity id and a type name");
    }
    return ret_bool(gryce::editor::add_component(h, type_name.c_str()));
}

FN(js_remove_component) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    uint64_t hash = 0;
    if (!want_handle(a, 0, &h, "editor.removeComponent") || !a.u64(1, &hash)) {
        return fail("editor.removeComponent(handle, typeHash) expects entity id and a hash");
    }
    return ret_bool(gryce::editor::remove_component(h, hash));
}

FN(js_set_material) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    std::string mesh;
    float r = 1.0f, g = 1.0f, b = 1.0f, rough = 0.5f, metal = 0.0f;
    if (!want_handle(a, 0, &h, "editor.setMaterial") || !a.str(1, &mesh)
        || !a.number(2, &r) || !a.number(3, &g) || !a.number(4, &b)
        || !a.number(5, &rough) || !a.number(6, &metal)) {
        return fail("editor.setMaterial(handle, meshPath, r, g, b, roughness, metallic) "
                    "expects 7 arguments");
    }
    return ret_bool(gryce::editor::set_material(h, mesh.c_str(), r, g, b, rough, metal));
}

FN(js_import_asset) {
    Args a = args_of(argc, argv);
    std::string path;
    if (!a.str(0, &path)) return fail("editor.importAsset(path) expects a path");
    return ret_bool(gryce::editor::import_asset(path.c_str()));
}

// ---------------------------------------------------------------------------
// 历史
// ---------------------------------------------------------------------------

FN(js_undo) {
    (void)argc;
    (void)argv;
    return ret_bool(gryce::editor::undo());
}

FN(js_redo) {
    (void)argc;
    (void)argv;
    return ret_bool(gryce::editor::redo());
}

FN(js_history) {
    (void)argc;
    (void)argv;
    return ret_str(gryce::editor::history_json());
}

FN(js_begin_edit) {
    Args a = args_of(argc, argv);
    std::string label;
    if (!a.str(0, &label)) label = "edit";
    gryce::editor::begin_edit(label.c_str());
    return ret_undef();
}

FN(js_end_edit) {
    (void)argc;
    (void)argv;
    gryce::editor::end_edit();
    return ret_undef();
}

FN(js_clear_history) {
    (void)argc;
    (void)argv;
    gryce::editor::clear_history();
    return ret_undef();
}

// ---------------------------------------------------------------------------
// 播放
// ---------------------------------------------------------------------------

FN(js_set_play) {
    Args a = args_of(argc, argv);
    bool play = false;
    if (!a.boolean(0, &play)) return fail("editor.setPlay(bool) expects a boolean");
    return ret_bool(gryce::editor::set_play(play ? 1 : 0));
}

FN(js_toggle_play) {
    (void)argc;
    (void)argv;
    return ret_bool(gryce::editor::toggle_play());
}

FN(js_step_frame) {
    (void)argc;
    (void)argv;
    return ret_bool(gryce::editor::step_frame());
}

// ---------------------------------------------------------------------------
// 视口
// ---------------------------------------------------------------------------

FN(js_view_enable_output) {
    Args a = args_of(argc, argv);
    bool on = true;
    if (!a.boolean(0, &on)) return fail("editor.view_enableOutput(bool) expects a boolean");
    return ret_bool(gryce::editor::view_enable_output(on ? 1 : 0));
}

FN(js_view_set_size) {
    Args a = args_of(argc, argv);
    int w = 0;
    int h = 0;
    if (!a.i32(0, &w) || !a.i32(1, &h)) {
        return fail("editor.view_setSize(w, h) expects two integers");
    }
    return ret_bool(gryce::editor::view_set_size(w, h));
}

FN(js_view_set_editor_camera) {
    Args a = args_of(argc, argv);
    float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f, pitch = 0.0f, fov = 60.0f;
    if (!a.number(0, &x) || !a.number(1, &y) || !a.number(2, &z)
        || !a.number(3, &yaw) || !a.number(4, &pitch) || !a.number(5, &fov)) {
        return fail("editor.view_setEditorCamera(x, y, z, yaw, pitch, fov) "
                    "expects 6 numbers");
    }
    gryce::editor::view_set_editor_camera(x, y, z, yaw, pitch, fov);
    return ret_undef();
}

FN(js_view_info) {
    (void)argc;
    (void)argv;
    return ret_str(gryce::editor::view_info_json());
}

FN(js_view_set_display_mode) {
    Args a = args_of(argc, argv);
    std::string mode;
    if (!a.str(0, &mode)) return fail("editor.view_setDisplayMode(mode) expects a name");
    return ret_bool(gryce::editor::view_set_display_mode(mode.c_str()));
}

FN(js_view_display_mode_supported) {
    Args a = args_of(argc, argv);
    std::string mode;
    if (!a.str(0, &mode)) return fail("editor.view_displayModeSupported(mode) expects a name");
    return ret_bool(gryce::editor::view_display_mode_supported(mode.c_str()));
}

// ---------------------------------------------------------------------------
// Gizmo
// ---------------------------------------------------------------------------

FN(js_gizmo_enable) {
    Args a = args_of(argc, argv);
    bool on = true;
    if (!a.boolean(0, &on)) return fail("editor.gizmo_enable(bool) expects a boolean");
    return ret_bool(gryce::editor::gizmo_enable(on ? 1 : 0));
}

FN(js_gizmo_set_mode) {
    Args a = args_of(argc, argv);
    int op = 0;
    int space = 0;
    if (!a.i32(0, &op) || !a.i32(1, &space)) {
        return fail("editor.gizmo_setMode(operation, space) expects two integers");
    }
    return ret_bool(gryce::editor::gizmo_set_mode(op, space));
}

FN(js_gizmo_mode) {
    (void)argc;
    (void)argv;
    return ret_str(gryce::editor::gizmo_mode_json());
}

FN(js_gizmo_hit_test) {
    Args a = args_of(argc, argv);
    float x = 0.0f;
    float y = 0.0f;
    if (!a.number(0, &x) || !a.number(1, &y)) {
        return fail("editor.gizmo_hitTest(x, y) expects screen coordinates");
    }
    return ret_num(static_cast<double>(gryce::editor::gizmo_hit_test(x, y)));
}

FN(js_gizmo_begin_drag) {
    Args a = args_of(argc, argv);
    int axis = -1;
    if (!a.i32(0, &axis)) return fail("editor.gizmo_beginDrag(axis) expects an axis");
    return ret_bool(gryce::editor::gizmo_begin_drag(axis));
}

FN(js_gizmo_drag) {
    Args a = args_of(argc, argv);
    float delta = 0.0f;
    if (!a.number(0, &delta)) return fail("editor.gizmo_drag(deltaPx) expects a number");
    return ret_bool(gryce::editor::gizmo_drag(delta));
}

FN(js_gizmo_end_drag) {
    (void)argc;
    (void)argv;
    return ret_bool(gryce::editor::gizmo_end_drag());
}

// ---------------------------------------------------------------------------
// 资产
// ---------------------------------------------------------------------------

FN(js_asset_scan) {
    Args a = args_of(argc, argv);
    std::string dir;
    bool recursive = false;
    if (!a.str(0, &dir)) return fail("editor.asset_scan(dir[, recursive]) expects a path");
    if (argc > 1) a.boolean(1, &recursive);
    return ret_str(gryce::editor::asset_scan(dir.c_str(), recursive ? 1 : 0));
}

FN(js_asset_import) {
    Args a = args_of(argc, argv);
    std::string path;
    if (!a.str(0, &path)) return fail("editor.asset_import(path) expects a path");
    return ret_num(static_cast<double>(gryce::editor::asset_import(path.c_str())));
}

FN(js_asset_meta) {
    Args a = args_of(argc, argv);
    std::string path;
    if (!a.str(0, &path)) return fail("editor.asset_meta(path) expects a path");
    return ret_str(gryce::editor::asset_meta_json(path.c_str()));
}

// ---------------------------------------------------------------------------
// 面板
// ---------------------------------------------------------------------------

FN(js_panel_hierarchy) {
    (void)argc;
    (void)argv;
    return ret_str(gryce::editor::panel_hierarchy_json());
}

FN(js_panel_inspector) {
    Args a = args_of(argc, argv);
    GEntityHandle h = 0;
    if (!a.handle(0, &h)) return fail("editor.panel_inspector(handle) expects entity id");
    return ret_str(gryce::editor::panel_inspector_json(h));
}

FN(js_panel_assets) {
    Args a = args_of(argc, argv);
    std::string dir = "res:/models";
    if (argc > 0) a.str(0, &dir);
    return ret_str(gryce::editor::panel_assets_json(dir.c_str()));
}

FN(js_panel_stats) {
    (void)argc;
    (void)argv;
    return ret_str(gryce::editor::panel_stats_json());
}

// ---------------------------------------------------------------------------
// 快捷键
// ---------------------------------------------------------------------------

FN(js_shortcut_bind) {
    Args a = args_of(argc, argv);
    std::string name;
    int key = 0;
    int mods = 0;
    if (!a.str(0, &name) || !a.i32(1, &key)) {
        return fail("editor.shortcut_bind(name, key[, mods]) expects a name and a key code");
    }
    if (argc > 2) a.i32(2, &mods);
    return ret_bool(gryce::editor::shortcut_bind(name.c_str(), key, mods));
}

FN(js_shortcut_poll) {
    (void)argc;
    (void)argv;
    return ret_str(gryce::editor::shortcut_poll_json());
}

// ---------------------------------------------------------------------------
// 界面
// ---------------------------------------------------------------------------

FN(js_ui_available) {
    (void)argc;
    (void)argv;
    // 返回布尔而不是 1/0：JS 侧写 `if (E.ui_available())` 时两者都能用，
    // 但 `=== true` 只有布尔成立（面板那边的教训，见 README 第 6 节）。
    return ret_bool(gryce::editor::ui_available());
}

FN(js_ui_set_enabled) {
    Args a = args_of(argc, argv);
    bool on = true;
    if (argc > 0) a.boolean(0, &on);
    return ret_bool(gryce::editor::ui_set_enabled(on ? 1 : 0));
}

FN(js_ui_frame_count) {
    (void)argc;
    (void)argv;
    return ret_num(static_cast<double>(gryce::editor::ui_frame_count()));
}

FN(js_ui_pointer_in_view) {
    (void)argc;
    (void)argv;
    return ret_bool(gryce::editor::ui_pointer_in_view());
}

FN(js_ui_state) {
    (void)argc;
    (void)argv;
    return ret_str(gryce::editor::ui_state_json());
}

} // namespace

// ===========================================================================
// 注册入口
//
// 每次注册都重建整张表：JSContext 换了，旧函数对象已经跟着旧 context 一起
// 销毁；这里只是把新 context 的函数装进新的 engine 对象。
// ===========================================================================

GRYCE_EXTENSION_ENTRY {
    if (!api || api->version != GRYCE_EXTENSION_VERSION) return;

    g_api = api;
    g_ctx = api->js_ctx;

    gryce::editor::init();

    const char* ns = "editor";
    auto bind = [&](const char* name, GJsNativeFn fn, int argc) {
        api->add_function(api->js_ctx, ns, name, fn, argc);
    };

    // 生命周期
    bind("version", &js_version, 0);
    bind("tick", &js_tick, 1);
    bind("session", &js_session, 0);
    bind("open", &js_open, 1);
    bind("close", &js_close, 0);
    bind("save", &js_save, 1);

    // 选择
    bind("select", &js_select, 1);
    bind("selectAdd", &js_select_add, 1);
    bind("selectToggle", &js_select_toggle, 1);
    bind("selectRemove", &js_select_remove, 1);
    bind("selectClear", &js_select_clear, 0);
    bind("selection", &js_selection, 0);
    bind("deleteSelection", &js_delete_selection, 0);
    bind("pick", &js_pick, 2);
    bind("focusPoint", &js_focus, 0);

    // 编辑命令
    bind("create", &js_create, 2);
    bind("destroy", &js_destroy, 1);
    bind("rename", &js_rename, 2);
    bind("reparent", &js_reparent, 2);
    bind("setPosition", &js_set_position, 4);
    bind("setRotation", &js_set_rotation, 5);
    bind("setScale", &js_set_scale, 4);
    bind("translate", &js_translate, 4);
    bind("setProperty", &js_set_property_number, 4);
    bind("setPropertyBool", &js_set_property_bool, 4);
    bind("addComponent", &js_add_component, 2);
    bind("removeComponent", &js_remove_component, 2);
    bind("setMaterial", &js_set_material, 7);
    bind("importAsset", &js_import_asset, 1);

    // 历史
    bind("undo", &js_undo, 0);
    bind("redo", &js_redo, 0);
    bind("history", &js_history, 0);
    bind("beginEdit", &js_begin_edit, 1);
    bind("endEdit", &js_end_edit, 0);
    bind("clearHistory", &js_clear_history, 0);

    // 播放
    bind("setPlay", &js_set_play, 1);
    bind("togglePlay", &js_toggle_play, 0);
    bind("stepFrame", &js_step_frame, 0);

    // 视口
    bind("view_enableOutput", &js_view_enable_output, 1);
    bind("view_setSize", &js_view_set_size, 2);
    bind("view_setEditorCamera", &js_view_set_editor_camera, 6);
    bind("view_info", &js_view_info, 0);
    bind("view_setDisplayMode", &js_view_set_display_mode, 1);
    bind("view_displayModeSupported", &js_view_display_mode_supported, 1);

    // Gizmo
    bind("gizmo_enable", &js_gizmo_enable, 1);
    bind("gizmo_setMode", &js_gizmo_set_mode, 2);
    bind("gizmo_mode", &js_gizmo_mode, 0);
    bind("gizmo_hitTest", &js_gizmo_hit_test, 2);
    bind("gizmo_beginDrag", &js_gizmo_begin_drag, 1);
    bind("gizmo_drag", &js_gizmo_drag, 1);
    bind("gizmo_endDrag", &js_gizmo_end_drag, 0);

    // 资产
    bind("asset_scan", &js_asset_scan, 2);
    bind("asset_import", &js_asset_import, 1);
    bind("asset_meta", &js_asset_meta, 1);

    // 面板
    bind("panel_hierarchy", &js_panel_hierarchy, 0);
    bind("panel_inspector", &js_panel_inspector, 1);
    bind("panel_assets", &js_panel_assets, 1);
    bind("panel_stats", &js_panel_stats, 0);

    // 快捷键
    bind("shortcut_bind", &js_shortcut_bind, 3);
    bind("shortcut_poll", &js_shortcut_poll, 0);

    // 界面
    bind("ui_available", &js_ui_available, 0);
    bind("ui_setEnabled", &js_ui_set_enabled, 1);
    bind("ui_frameCount", &js_ui_frame_count, 0);
    bind("ui_pointerInView", &js_ui_pointer_in_view, 0);
    bind("ui_state", &js_ui_state, 0);
}
