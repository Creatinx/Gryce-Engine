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

private:
    static UIManager* ui_manager_;
    static ScriptVM* vm_;
};

} // namespace GryceEngineUtils::ui