// ===========================================================================
// editor/ext/session/session.cpp — 会话状态与选择集实现
// ===========================================================================

#include "session/session.h"

#include "api/core_api.h"
#include "api/edit_api.h"
#include "api/entity_api.h"
#include "api/scene_api.h"

#include "common/json_writer.h"

#include <algorithm>
#include <cstring>

namespace gryce::editor::session {

namespace {

std::string g_doc_path;
std::string g_doc_name;
Selection g_selection;

// 脏标记用"编辑代数"比对，不存场景副本：
//   GCore_GetUndoCount() 每次可撤销变更 +1，撤销 -1，重做 +1。
// 保存时记下当时的计数，之后计数不同就是脏。好处是"撤销回到保存点"会自动
// 变干净，不需要额外判断；代价是撤销栈满（默认 64 层）后计数饱和，此时
// 连续编辑不再改变计数，脏标记会漏报 —— 极端场景，用 SetUndoLimit 放大即可。
int g_saved_undo_count = 0;

// 引擎单选同步。GEdit_SelectEntity 也是入队的，所以这一帧的渲染可能还用着
// 旧的选中；一帧延迟在编辑器里不可见（16ms），换来的是选择走同一条命令通道，
// 不会出现"命令队列里改了变换、选择却已经指向别的实体"的错位。
void sync_primary_to_engine(GEntityHandle h) {
    GEdit_SelectEntity(h);
}

} // namespace

bool Selection::contains(GEntityHandle h) const {
    return std::find(items.begin(), items.end(), h) != items.end();
}

void Selection::add(GEntityHandle h) {
    if (h <= 0 || contains(h)) return;
    items.push_back(h);
    if (primary == 0) primary = h;
}

void Selection::remove(GEntityHandle h) {
    auto it = std::find(items.begin(), items.end(), h);
    if (it == items.end()) return;
    items.erase(it);
    if (primary == h) {
        primary = items.empty() ? 0 : items.front();
    }
}

void Selection::clear() {
    items.clear();
    primary = 0;
}

bool handle_alive(GEntityHandle handle) {
    if (handle <= 0) return false;
    char name[8];
    // GetName 对无效句柄返回 -1；这是最轻的存活探测（不构造实体对象）。
    return GEntity_GetName(handle, name, static_cast<int>(sizeof(name))) >= 0;
}

void reset() {
    g_doc_path.clear();
    g_doc_name.clear();
    g_selection.clear();
    g_saved_undo_count = GCore_GetUndoCount();
}

int open_doc(const char* path) {
    if (!path || !path[0]) return 0;
    if (GScene_Load(path) != 0) return 0;

    g_doc_path = path;
    const std::size_t slash = g_doc_path.find_last_of("/\\");
    g_doc_name = (slash == std::string::npos) ? g_doc_path : g_doc_path.substr(slash + 1);

    // 换文档：选择集与历史都清。GScene_Load 内部已经清过一次历史，
    // 这里再清一次是幂等的，但选择集必须显式清 —— 旧句柄在新场景里
    // 可能刚好撞上别的实体的号码。
    g_selection.clear();
    GCore_ClearUndoHistory();
    g_saved_undo_count = GCore_GetUndoCount();
    return 1;
}

void close_doc() {
    g_doc_path.clear();
    g_doc_name.clear();
    g_selection.clear();
    GCore_ClearUndoHistory();
    g_saved_undo_count = 0;
}

int save_doc(const char* path) {
    const char* target = (path && path[0]) ? path : g_doc_path.c_str();
    if (!target || !target[0]) return 0;
    if (!GEdit_SaveScene(target)) return 0;

    // 保存后重设基线：此后计数不再变化就是"干净"。
    g_doc_path = target;
    const std::size_t slash = g_doc_path.find_last_of("/\\");
    g_doc_name = (slash == std::string::npos) ? g_doc_path : g_doc_path.substr(slash + 1);
    g_saved_undo_count = GCore_GetUndoCount();
    return 1;
}

const std::string& doc_path() { return g_doc_path; }

const char* doc_name() { return g_doc_name.c_str(); }

bool dirty() {
    return GCore_GetUndoCount() != g_saved_undo_count;
}

bool has_doc() { return !g_doc_path.empty(); }

void tick(float dt) {
    (void)dt;

    // 撤销 / 删除会让选择集里的句柄失效。每帧剔除一次比在每个调用点判空
    // 更省心 —— 句柄失效的实体在 Inspector 里会显示成空白行，看起来像 bug。
    if (!g_selection.items.empty()) {
        auto& items = g_selection.items;
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [](GEntityHandle h) { return !handle_alive(h); }),
                    items.end());
        if (g_selection.primary != 0 && !g_selection.contains(g_selection.primary)) {
            g_selection.primary = items.empty() ? 0 : items.front();
        }
    }
}

int select(GEntityHandle handle) {
    if (handle < 0) return 0;
    if (handle == 0) {
        clear_selection();
        return 1;
    }
    if (!handle_alive(handle)) return 0;
    g_selection.clear();
    g_selection.add(handle);
    g_selection.primary = handle;
    sync_primary_to_engine(handle);
    return 1;
}

int select_add(GEntityHandle handle) {
    if (handle <= 0 || !handle_alive(handle)) return 0;
    if (g_selection.contains(handle)) return 1;
    g_selection.add(handle);
    // 追加不改主选中：Inspector 保持在原来那个上，符合"按住 Ctrl 多选"的预期。
    sync_primary_to_engine(g_selection.primary);
    return 1;
}

int select_toggle(GEntityHandle handle) {
    if (handle <= 0 || !handle_alive(handle)) return 0;
    if (g_selection.contains(handle)) {
        return select_remove(handle);
    }
    return select_add(handle);
}

int select_remove(GEntityHandle handle) {
    if (handle <= 0) return 0;
    if (!g_selection.contains(handle)) return 1;
    g_selection.remove(handle);
    sync_primary_to_engine(g_selection.primary);
    return 1;
}

void clear_selection() {
    g_selection.clear();
    sync_primary_to_engine(0);
}

const Selection& selection() { return g_selection; }

std::string to_json() {
    Json j;
    j.begin_object();
    j.kv("path", g_doc_path);
    j.kv("name", g_doc_name);
    j.kv("dirty", dirty());
    j.kv("hasDoc", has_doc());
    j.kv("entities", GEntity_GetCount());
    j.kv("playing", GCore_IsPlaying());
    j.kv("paused", GCore_IsPaused());
    j.kv("selected", static_cast<long long>(g_selection.primary));
    j.kv("selectionCount", static_cast<long long>(g_selection.items.size()));
    j.kv("undo", GCore_GetUndoCount());
    j.kv("redo", GCore_GetRedoCount());
    j.end_object();
    return j.str();
}

std::string stats_json() {
    Json j;
    j.begin_object();
    j.kv("entities", GEntity_GetCount());
    j.kv("selected", static_cast<long long>(g_selection.primary));
    j.kv("selectionCount", static_cast<long long>(g_selection.items.size()));
    j.kv("undo", GCore_GetUndoCount());
    j.kv("redo", GCore_GetRedoCount());
    j.kv("dirty", dirty());
    j.end_object();
    return j.str();
}

} // namespace gryce::editor::session
