// ===========================================================================
// editor/ext/ui/ui.h — 编辑器界面（绘制层）
//
// 这一层是编辑器里唯一 include 引擎 UI 接口（api/ui_api.h）的地方，也是唯一
// 知道"I 面板长什么样"的地方。它只读面板数据模型（panel/model.h），不读
// 引擎状态、不下发命令 —— 用户操作统一记成意图（ui/intent.h），由 tick 在
// 锁外执行。
//
// 为什么界面不在 ext 的声明层里：ext.h 声明的是"给脚本用的能力"，界面是
// 宿主内部实现。脚本能问"界面开着没有、画了几帧"（ui_available / ui_state），
// 但不需要也不应该逐控件驱动 UI。
// ===========================================================================

#ifndef GRYCE_EDITOR_UI_H
#define GRYCE_EDITOR_UI_H

#include <string>

namespace gryce::editor::ui {

void reset();

// 注册引擎 UI 回调。必须在 GRender_Init 之前（扩展在宿主启动时就 init 了）。
void init();
void shutdown();

// 每帧：执行上一帧攒下的意图 → 消费视口输入 → 重建面板数据 → 取视口纹理。
void tick(float dt);

int enabled();
int set_enabled(int on);

// 引擎是否接管了 ImGui（关掉 ImGui 的构建里恒为 0，此时界面整块不生效）。
int available();

// 回调被调用的帧数 / 真正画出内容（有 ImGui 上下文）的帧数。测试用它确认
// "界面确实被宿主渲染了"，而不是只看注册成功。
int frame_count();

// 界面是否"吃掉了"这一帧的键鼠。快捷键与相机导航据此让路：在输入框里
// 打字时按 W 不该切到移动工具。读的是上一帧回调记下的状态。
int wants_keyboard();
int wants_mouse();

// 指针是否在视口矩形内。JS 侧的右键转视角只在视口内生效，否则在层级树
// 上右键也会把相机转走。
int pointer_in_view();

std::string state_json();

} // namespace gryce::editor::ui

#endif // GRYCE_EDITOR_UI_H
