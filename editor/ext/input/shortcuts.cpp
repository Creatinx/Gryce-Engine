// ===========================================================================
// editor/ext/input/shortcuts.cpp — 快捷键实现
// ===========================================================================

#include "input/shortcuts.h"

#include "ui/ui.h"

#include "api/input_api.h"

#include "common/json_writer.h"

#include <cstring>
#include <string>
#include <vector>

namespace gryce::editor::shortcuts {

namespace {

// 只查左修饰键。编辑器不区分左右 Ctrl，查左键足够，也避免"按住右 Shift
// 快捷键不生效"这种只有少数人才会遇到的怪异表现。
constexpr int kKeyLeftShift = 340;
constexpr int kKeyLeftControl = 341;
constexpr int kKeyLeftAlt = 342;

struct Binding {
    std::string name;
    int key = 0;
    int mods = 0;
};

std::vector<Binding> g_bindings;

bool mods_match(int want) {
    const bool ctrl = GInput_IsKeyHeld(kKeyLeftControl);
    const bool shift = GInput_IsKeyHeld(kKeyLeftShift);
    const bool alt = GInput_IsKeyHeld(kKeyLeftAlt);
    return ctrl == ((want & kCtrl) != 0) &&
           shift == ((want & kShift) != 0) &&
           alt == ((want & kAlt) != 0);
}

} // namespace

void reset() { g_bindings.clear(); }

int bind(const char* name, int key, int mods) {
    if (!name || !name[0]) return 0;
    if (key <= 0 || key >= 512) return 0;      // 引擎的键表大小
    if (mods < 0 || mods > 7) return 0;

    for (auto& b : g_bindings) {
        if (b.name == name) {
            b.key = key;
            b.mods = mods;
            return 1;
        }
        // 同一组合不能绑两个动作：按下时到底执行哪个没有正确答案，
        // 与其猜，不如让调用方改键位。
        if (b.key == key && b.mods == mods) return 0;
    }

    g_bindings.push_back(Binding{name, key, mods});
    return 1;
}

std::string poll_json() {
    // 界面正在吃键盘（输入框聚焦）时一律不派发：否则在名字输入框里打
    // "Wire" 会顺手中途切走移动/旋转/缩放三个工具。
    if (ui::wants_keyboard()) return "[]";

    Json j;
    j.begin_array();
    for (const auto& b : g_bindings) {
        // 先查边沿再查修饰键：修饰键不是每帧都变，先判边沿能少查三次
        // GInput_IsKeyHeld（每次都要拿平台锁）。
        if (!GInput_IsKeyPressed(b.key)) continue;
        if (!mods_match(b.mods)) continue;
        j.value(b.name);
    }
    j.end_array();
    return j.str();
}

std::string table_json() {
    Json j;
    j.begin_array();
    for (const auto& b : g_bindings) {
        j.begin_object();
        j.kv("name", b.name);
        j.kv("key", b.key);
        j.kv("mods", b.mods);
        j.end_object();
    }
    j.end_array();
    return j.str();
}

} // namespace gryce::editor::shortcuts
