#pragma once

// EngineBridge — 将引擎核心 SDK 函数注册到 ScriptVM 的 JS 全局作用域
//
// 注册的 JS 对象:
//   engine.loadScene(path)    — 加载场景
//   engine.playSound(name)    — 播放音效（预留）
//   engine.showDialog(id)     — 显示对话框
//   engine.closeDialog()      — 关闭对话框
//   engine.getString(key)     — 获取本地化字符串（预留）
//   engine.bind(id, callback) — 绑定事件回调（预留）
//
// 在 UIManager 初始化 ScriptVM 后调用:
//   EngineBridge::init(vm, ui_manager);

#include <string>

#include <quickjs/quickjs.h>

namespace GryceEngineUtils {
class Renderer;
namespace ecs { class World; }
namespace ui { class UIManager; }

// 游戏玩法桥接运行时：宿主把依赖注入给引擎，供 QuickJS 脚本透过
// engine.game.* 生成/移动/销毁 3D 实体、读取输入、改 UI 控件。
// 由宿主调用 EngineBridge::set_game_runtime 填充（可留空字段）。
struct GameRuntime {
    ecs::World* world = nullptr;      // 3D 实体所在 ECS World
    Renderer*   renderer = nullptr;   // 输入查询与渲染窗口
};

using UIManager = ui::UIManager;
} // namespace GryceEngineUtils

namespace GryceEngineUtils::ui {

class ScriptVM;
class UIManager;

class EngineBridge {
public:
    // 注册 engine.* 函数到 ScriptVM 的 JS 全局作用域
    static void init(ScriptVM* vm, UIManager* ui_mgr);

    // 设置 UIManager 指针（用于事件回调中访问）
    static void set_ui_manager(UIManager* mgr) { ui_manager_ = mgr; }
    static UIManager* ui_manager() { return ui_manager_; }

    // 获取当前 vm（用于事件回调中调用 JS 函数）
    static ScriptVM* vm() { return vm_; }

    // 注入游戏玩法桥接运行时（engine.game.* 依赖它）
    static void set_game_runtime(const GryceEngineUtils::GameRuntime& rt) { game_runtime_ = rt; }
    static const GryceEngineUtils::GameRuntime& game_runtime() { return game_runtime_; }

private:
    static UIManager* ui_manager_;
    static ScriptVM* vm_;
    static GryceEngineUtils::GameRuntime game_runtime_;
};

} // namespace GryceEngineUtils::ui