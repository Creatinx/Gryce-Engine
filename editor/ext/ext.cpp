// ===========================================================================
// editor/ext/ext.cpp — 装配层
//
// 只做三件事：初始化各模块、把模块结果放进轮转缓冲、把上游参数透下去。
// 任何"判断逻辑"都不该写在这里 —— 一旦 ext.cpp 开始变长，说明它开始抢
// 子模块的活，后续加能力时会先在装配层堆一堆 if。
// ===========================================================================

#include "ext.h"

#include "asset/asset_index.h"
#include "command/commands.h"
#include "input/shortcuts.h"
#include "panel/panel.h"
#include "session/session.h"
#include "ui/ui.h"
#include "view/gizmo.h"
#include "view/viewport.h"

#include "api/core_api.h"
#include "api/edit_api.h"
#include "api/render_api.h"
#include "api/scene_api.h"

#include <string>

namespace gryce::editor {

namespace {

// 轮转缓冲：声明里所有 const char* 返回值都指向这里。
// 8 个槽位是为了让"一次拼 JSON 里嵌三次子调用"不会互相覆盖 —— 面板数据
// 常见这种嵌套（assets_json 里塞 scan 的结果）。JS 桥拿到后立刻复制成
// JS 字符串，所以只需要在当次调用内有效。
constexpr int kScratchSlots = 8;
std::string g_scratch[kScratchSlots];
int g_scratch_cursor = 0;

const char* hold(std::string value) {
    g_scratch_cursor = (g_scratch_cursor + 1) % kScratchSlots;
    g_scratch[g_scratch_cursor] = std::move(value);
    return g_scratch[g_scratch_cursor].c_str();
}

} // namespace

// ===========================================================================
// 生命周期
// ===========================================================================

void init() {
    session::reset();
    commands::reset();
    view::reset();
    gizmo::reset();
    asset::reset();
    panel::reset();
    shortcuts::reset();
    // UI 最后初始化：它要在引擎创建窗口之前把回调注册上去，引擎据此决定
    // 是否接管 ImGui。前面几个模块的 reset 与 UI 无关，放在前面便于阅读。
    ui::init();
    for (auto& s : g_scratch) s.clear();
    g_scratch_cursor = 0;
}

void shutdown() {
    // 先摘掉 UI 回调：回调里会读面板模型，模型析构之后再来一帧就会读到空。
    ui::shutdown();
    // 拖到一半退出（Alt-F4）时引擎侧还开着分组，不收尾会让快照永久合并。
    if (gizmo::dragging()) gizmo::end_drag();
    commands::abort_edit();
    session::close_doc();
}

void tick(float dt) {
    // 顺序有讲究：先让命令层确认上一帧的创建（它会把事件塞进队列），
    // 再清选择集里的失效句柄，最后让面板缓存失效——面板这一帧重新读到的
    // 才是清理后的状态。
    commands::tick();
    session::tick(dt);
    view::tick();
    panel::tick();
    // UI 放在最后：它这一帧执行意图、消费鼠标、重建面板数据，用的都是上面
    // 几个模块刚校正过的状态。
    ui::tick(dt);
}

const char* version() { return "editor-ext 1.0 (gryce)"; }

const char* pending_events_json() { return hold(commands::take_events_json()); }

// ===========================================================================
// 会话
// ===========================================================================

int open(const char* path) {
    if (!session::open_doc(path)) return 0;
    // 换文档后所有派生状态归零：面板缓存、资产缓存都可能指着旧文档的路径。
    panel::reset();
    asset::reset();
    commands::reset();
    return 1;
}

void close() { session::close_doc(); }

int save(const char* path) { return session::save_doc(path); }

const char* session_json() { return hold(session::to_json()); }

// ===========================================================================
// 选择
// ===========================================================================

int select(GEntityHandle handle) { return session::select(handle); }
int select_add(GEntityHandle handle) { return session::select_add(handle); }
int select_toggle(GEntityHandle handle) { return session::select_toggle(handle); }
int select_remove(GEntityHandle handle) { return session::select_remove(handle); }
void select_clear() { session::clear_selection(); }

const char* selection_json() {
    const auto& sel = session::selection();
    std::string out = "{\"primary\":";
    out += std::to_string(sel.primary);
    out += ",\"count\":";
    out += std::to_string(sel.items.size());
    out += ",\"items\":[";
    for (std::size_t i = 0; i < sel.items.size(); ++i) {
        if (i) out += ',';
        out += std::to_string(sel.items[i]);
    }
    out += "]}";
    return hold(std::move(out));
}

int delete_selection() { return commands::delete_selection(); }

GEntityHandle pick(float sx, float sy) {
    // 用视口相机自己发射射线，不走 GScene_PickScreen：后者用的是场景里的
    // Camera 实体，而编辑器视口的朝向来自 GViewport_SetEditorTransform，
    // 两者可以完全无关。用错相机的表现是"点左边选中右边"，很难查出原因。
    GVec3 origin{};
    GVec3 dir{};
    if (!view::screen_ray(sx, sy, &origin, &dir)) return 0;
    return GScene_PickRay(&origin, &dir, 0.0f);
}

const char* focus_json() { return hold(commands::focus_json()); }

// ===========================================================================
// 编辑命令
// ===========================================================================

int create(const char* name, GEntityHandle parent) {
    return commands::create(name, parent);
}
int destroy(GEntityHandle handle) { return commands::destroy(handle); }
int rename(GEntityHandle handle, const char* name) {
    return commands::rename(handle, name);
}
int reparent(GEntityHandle handle, GEntityHandle parent) {
    return commands::reparent(handle, parent);
}

int set_position(GEntityHandle handle, float x, float y, float z) {
    return commands::set_position(handle, x, y, z);
}
int set_rotation(GEntityHandle handle, float x, float y, float z, float w) {
    return commands::set_rotation(handle, x, y, z, w);
}
int set_scale(GEntityHandle handle, float x, float y, float z) {
    return commands::set_scale(handle, x, y, z);
}
int translate(GEntityHandle handle, float dx, float dy, float dz) {
    return commands::translate(handle, dx, dy, dz);
}

int set_property_number(GEntityHandle handle, uint64_t type_hash,
                        const char* prop, double value) {
    return commands::set_property_number(handle, type_hash, prop, value);
}

int set_property_bool(GEntityHandle handle, uint64_t type_hash,
                      const char* prop, int value) {
    return commands::set_property_bool(handle, type_hash, prop, value);
}

int set_property_vec(GEntityHandle handle, uint64_t type_hash,
                     const char* prop, const float* values, int count) {
    return commands::set_property_vec(handle, type_hash, prop, values, count);
}

int set_property_string(GEntityHandle handle, uint64_t type_hash,
                        const char* prop, const char* value) {
    return commands::set_property_string(handle, type_hash, prop, value);
}

int add_component(GEntityHandle handle, const char* type_name) {
    return commands::add_component(handle, type_name);
}
int remove_component(GEntityHandle handle, uint64_t type_hash) {
    return commands::remove_component(handle, type_hash);
}

int set_material(GEntityHandle handle, const char* mesh_path,
                 float r, float g, float b, float roughness, float metallic) {
    return commands::set_material(handle, mesh_path, r, g, b, roughness, metallic);
}

int import_asset(const char* path) {
    if (!commands::import_asset(path)) return 0;
    asset::invalidate();   // 新资产会带侧车，列表要重扫
    return 1;
}

// ===========================================================================
// 历史
// ===========================================================================

int undo() {
    const int ok = GCore_Undo();
    if (ok) commands::tick();   // 撤销后句柄可能失效，立刻校正选择集
    return ok;
}

int redo() {
    const int ok = GCore_Redo();
    if (ok) commands::tick();
    return ok;
}

const char* history_json() {
    const int u = GCore_GetUndoCount();
    const int r = GCore_GetRedoCount();
    // canUndo/canRedo 输出布尔而不是 0/1：JS 侧 `if (h.canUndo)` 才自然，
    // 数字 0 在 JS 里虽然是假值，但 `=== false` 比较会静默失败。
    std::string out = "{\"undo\":";
    out += std::to_string(u);
    out += ",\"redo\":";
    out += std::to_string(r);
    out += ",\"canUndo\":";
    out += (u > 0 ? "true" : "false");
    out += ",\"canRedo\":";
    out += (r > 0 ? "true" : "false");
    out += ",\"group\":\"";
    out += commands::current_group_label();
    out += "\"}";
    return hold(std::move(out));
}

void begin_edit(const char* label) { commands::begin_edit(label); }
void end_edit() { commands::end_edit(); }

void clear_history() {
    // 先收掉开着的分组：否则清空历史后 EndEdit 会去提交一个已经不存在的基线。
    commands::abort_edit();
    GCore_ClearUndoHistory();
}

// ===========================================================================
// 播放
// ===========================================================================

int set_play(int play) { return commands::set_play(play); }
int toggle_play() { return commands::toggle_play(); }
int step_frame() { return commands::step_frame(); }

// ===========================================================================
// 视口
// ===========================================================================

int view_enable_output(int enabled) { return view::enable_output(enabled); }
int view_set_size(int w, int h) { return view::set_size(w, h); }

void view_set_editor_camera(float x, float y, float z,
                            float yaw_deg, float pitch_deg, float fov_deg) {
    view::set_editor_camera(x, y, z, yaw_deg, pitch_deg, fov_deg);
}

const char* view_info_json() { return hold(view::info_json()); }

int view_set_display_mode(const char* mode) { return view::set_display_mode(mode); }
int view_display_mode_supported(const char* mode) {
    return view::display_mode_supported(mode);
}

// ===========================================================================
// Gizmo
// ===========================================================================

int gizmo_enable(int enabled) { return gizmo::enable(enabled); }
int gizmo_set_mode(int operation, int space) {
    return gizmo::set_mode(operation, space);
}
const char* gizmo_mode_json() { return hold(gizmo::mode_json()); }
int gizmo_hit_test(float sx, float sy) { return gizmo::hit_test(sx, sy); }
int gizmo_begin_drag(int axis) { return gizmo::begin_drag(axis); }
int gizmo_drag(float delta_px) { return gizmo::drag(delta_px); }
int gizmo_end_drag() { return gizmo::end_drag(); }

// ===========================================================================
// 资产
// ===========================================================================

const char* asset_scan(const char* dir, int recursive) {
    return asset::scan(dir, recursive);
}

GAssetHandle asset_import(const char* path) { return asset::import(path); }

const char* asset_meta_json(const char* path) { return hold(asset::meta_json(path)); }

// ===========================================================================
// 面板
// ===========================================================================

const char* panel_hierarchy_json() { return hold(panel::hierarchy_json()); }
const char* panel_inspector_json(GEntityHandle handle) {
    return hold(panel::inspector_json(handle));
}
const char* panel_assets_json(const char* dir) { return hold(panel::assets_json(dir)); }
const char* panel_stats_json() { return hold(panel::stats_json()); }

// ===========================================================================
// 快捷键
// ===========================================================================

int shortcut_bind(const char* name, int key, int mods) {
    return shortcuts::bind(name, key, mods);
}

const char* shortcut_poll_json() { return hold(shortcuts::poll_json()); }

// ===========================================================================
// 界面
// ===========================================================================

int ui_available() { return ui::available(); }
int ui_set_enabled(int enabled) { return ui::set_enabled(enabled); }
int ui_frame_count() { return ui::frame_count(); }
int ui_pointer_in_view() { return ui::pointer_in_view(); }
const char* ui_state_json() { return hold(ui::state_json()); }

} // namespace gryce::editor
