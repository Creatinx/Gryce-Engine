// ===========================================================================
// editor/ext/ui/intent.cpp — 界面意图的执行
//
// 队列里的每一条 Intent 都在这里翻译成对 commands:: / session:: / view:: /
// gizmo:: 的调用。这样"界面能做什么"就只有一处定义：加一个新按钮时只加一个
// 分支，不会出现某个面板绕开校验直接调引擎的情况。
// ===========================================================================

#include "ui/intent.h"

#include "asset/asset_index.h"
#include "command/commands.h"
#include "session/session.h"
#include "view/gizmo.h"
#include "view/viewport.h"

#include "api/core_api.h"

#include <vector>

namespace gryce::editor::ui {
namespace intent {

namespace {

std::vector<Intent> g_queue;

void run(const Intent& it) {
    switch (it.kind) {
        case kSelect:       session::select(it.handle); break;
        case kSelectAdd:    session::select_add(it.handle); break;
        case kSelectToggle: session::select_toggle(it.handle); break;
        case kSelectClear:  session::clear_selection(); break;

        case kCreate:
            // 建完自动选中：commands 的 pending 队列会在下一帧确认新句柄。
            commands::create(it.a.empty() ? "Entity" : it.a.c_str(), it.other);
            break;
        case kDestroy:  commands::destroy(it.handle); break;
        case kRename:   commands::rename(it.handle, it.a.c_str()); break;
        case kReparent: commands::reparent(it.handle, it.other); break;

        case kSetPosition: commands::set_position(it.handle, it.f0, it.f1, it.f2); break;
        case kSetRotation: commands::set_rotation(it.handle, it.f0, it.f1, it.f2, it.f3); break;
        case kSetScale:    commands::set_scale(it.handle, it.f0, it.f1, it.f2); break;

        case kSetPropertyNumber:
            commands::set_property_number(it.handle, it.hash, it.a.c_str(), it.number);
            break;
        case kSetPropertyBool:
            commands::set_property_bool(it.handle, it.hash, it.a.c_str(), it.flag);
            break;
        case kSetPropertyVec:
            commands::set_property_vec(it.handle, it.hash, it.a.c_str(), it.vec, it.flag);
            break;
        case kSetPropertyString:
            commands::set_property_string(it.handle, it.hash, it.a.c_str(), it.b.c_str());
            break;
        case kRemoveComponent:
            commands::remove_component(it.handle, it.hash);
            break;
        case kAddComponent:
            commands::add_component(it.handle, it.b.c_str());
            break;

        case kImportAsset:
            if (commands::import_asset(it.a.c_str())) asset::invalidate();
            break;

        case kSave: commands::save(it.a.empty() ? nullptr : it.a.c_str()); break;
        case kUndo:
            if (GCore_Undo()) commands::tick();   // 撤销后句柄可能失效
            break;
        case kRedo:
            if (GCore_Redo()) commands::tick();
            break;
        case kPlay:  commands::set_play(it.flag); break;
        case kStep:  commands::step_frame(); break;

        case kBeginEdit: commands::begin_edit(it.a.c_str()); break;
        case kEndEdit:   commands::end_edit(); break;

        case kGizmoMode:
            // other 非 0 表示"局部空间"，与引擎的 space 参数一致。
            gizmo::set_mode(it.flag, it.other > 0 ? 1 : 0);
            break;
        case kDisplayMode: view::set_display_mode(it.a.c_str()); break;

        default: break;
    }
}

} // namespace

void push_select(int kind, GEntityHandle handle) {
    // 选择类意图在同一帧内只保留最后一条：鼠标扫过整个层级树时不排十条命令。
    for (auto& it : g_queue) {
        if (it.kind == kSelect || it.kind == kSelectAdd || it.kind == kSelectToggle) {
            it.kind = kind;
            it.handle = handle;
            return;
        }
    }
    Intent it;
    it.kind = kind;
    it.handle = handle;
    g_queue.push_back(std::move(it));
}

void push(const Intent& intent) { g_queue.push_back(intent); }

void flush() {
    if (g_queue.empty()) return;
    // 先挪出来再执行：执行过程中可能有新的意图入队，边遍历边 push 会让
    // 迭代器失效（而且那些新意图本来就该下一帧执行）。
    std::vector<Intent> batch;
    batch.swap(g_queue);
    for (const Intent& it : batch) run(it);
}

void reset() { g_queue.clear(); }

int pending_count() { return static_cast<int>(g_queue.size()); }

} // namespace intent
} // namespace gryce::editor::ui
