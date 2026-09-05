#include "engine_bridge.h"

#include <cstring>
#include <string>

#include "GryceCore/core_api.h"
#include "GryceCore/types.h"

#include "script_vm.h"
#include "GryceEngineUtils/ui/ui.h"
#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils::ui {

// ============================================================================
// 静态成员
// ============================================================================

UIManager* EngineBridge::ui_manager_ = nullptr;
ScriptVM* EngineBridge::vm_ = nullptr;

// ============================================================================
// engine.loadScene(path)
// 将路径推送到核心命令队列加载场景
// ============================================================================

static JSValue js_engine_loadScene(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "loadScene: path argument required");
    }
    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) {
        return JS_ThrowTypeError(ctx, "loadScene: path must be a string");
    }

    GCommand cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.type = ECMD_LOAD_SCENE;
    std::strncpy(reinterpret_cast<char*>(cmd.payload), path, GCMD_PAYLOAD_SIZE - 1);
    GCore_PushCommand(&cmd);

    GLOG_INFO("[JS] engine.loadScene(\"{}\")", path);
    JS_FreeCString(ctx, path);
    return JS_UNDEFINED;
}

// ============================================================================
// engine.playSound(name)
// 预留：播放音效（音频播放 API 尚未实现）
// ============================================================================

static JSValue js_engine_playSound(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "playSound: name argument required");
    }
    const char* name = JS_ToCString(ctx, argv[0]);
    if (name) {
        GLOG_WARN("[JS] engine.playSound(\"{}\") not implemented yet", name);
        JS_FreeCString(ctx, name);
    }
    return JS_UNDEFINED;
}

// ============================================================================
// engine.showDialog(id)
// 通过 ID 查找控件，并作为模态对话框推入
// ============================================================================

static JSValue js_engine_showDialog(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    auto* mgr = EngineBridge::ui_manager();
    if (!mgr) {
        return JS_ThrowTypeError(ctx, "showDialog: UIManager not available");
    }
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "showDialog: id argument required");
    }
    const char* id = JS_ToCString(ctx, argv[0]);
    if (!id) {
        return JS_ThrowTypeError(ctx, "showDialog: id must be a string");
    }

    Widget* root = mgr->root();
    Widget* dialog = root ? root->find_element_by_id(id) : nullptr;
    if (dialog) {
        mgr->push_modal(dialog);
        dialog->set_visible(true);
        GLOG_INFO("[JS] engine.showDialog(\"{}\")", id);
    } else {
        GLOG_WARN("[JS] engine.showDialog(\"{}\") widget not found", id);
    }

    JS_FreeCString(ctx, id);
    return JS_UNDEFINED;
}

// ============================================================================
// engine.closeDialog()
// 关闭当前模态对话框
// ============================================================================

static JSValue js_engine_closeDialog(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* mgr = EngineBridge::ui_manager();
    if (!mgr) {
        return JS_ThrowTypeError(ctx, "closeDialog: UIManager not available");
    }

    // 隐藏当前模态并弹出
    std::vector<Widget*>* modals = nullptr;
    // 通过 UIManager 的接口操作
    // 注意：UIManager 没有直接暴露 modals_ 的访问器，需要通过 pop_modal 操作
    // 我们通过 pop_modal 来实现关闭
    Widget* top = mgr->hit_test_top(-1.0f, -1.0f); // 在 (-1,-1) 处没有命中控件
    // 直接通过 UIManager 的模态栈操作
    // 先获取最后一个模态
    mgr->pop_modal();
    GLOG_INFO("[JS] engine.closeDialog()");
    return JS_UNDEFINED;
}

// ============================================================================
// engine.getString(key)
// 预留：获取本地化字符串（目前返回 key 本身）
// ============================================================================

static JSValue js_engine_getString(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "getString: key argument required");
    }
    const char* key = JS_ToCString(ctx, argv[0]);
    if (!key) {
        return JS_ThrowTypeError(ctx, "getString: key must be a string");
    }
    JSValue ret = JS_NewString(ctx, key);
    JS_FreeCString(ctx, key);
    return ret;
}

// ============================================================================
// engine.bind(eventId, callback)
// 预留：绑定事件回调（用于数据绑定系统）
// ============================================================================

static JSValue js_engine_bind(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "bind: eventId and callback arguments required");
    }
    const char* event_id = JS_ToCString(ctx, argv[0]);
    if (!event_id) {
        return JS_ThrowTypeError(ctx, "bind: eventId must be a string");
    }

    GLOG_INFO("[JS] engine.bind(\"{}\", <callback>) not fully implemented yet", event_id);
    JS_FreeCString(ctx, event_id);
    return JS_UNDEFINED;
}

// ============================================================================
// EngineBridge::init — 注册所有 engine.* 函数到 JS 全局作用域
// ============================================================================

void EngineBridge::init(ScriptVM* vm, UIManager* ui_mgr) {
    if (!vm || !vm->initialized()) {
        GLOG_WARN("EngineBridge: ScriptVM not initialized, skipping bridge init");
        return;
    }

    vm_ = vm;
    ui_manager_ = ui_mgr;

    JSContext* ctx = vm->context();

    // 创建 engine 对象
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue engine = JS_NewObject(ctx);

    JS_SetPropertyStr(ctx, engine, "loadScene",
                      JS_NewCFunction(ctx, js_engine_loadScene, "loadScene", 1));
    JS_SetPropertyStr(ctx, engine, "playSound",
                      JS_NewCFunction(ctx, js_engine_playSound, "playSound", 1));
    JS_SetPropertyStr(ctx, engine, "showDialog",
                      JS_NewCFunction(ctx, js_engine_showDialog, "showDialog", 1));
    JS_SetPropertyStr(ctx, engine, "closeDialog",
                      JS_NewCFunction(ctx, js_engine_closeDialog, "closeDialog", 0));
    JS_SetPropertyStr(ctx, engine, "getString",
                      JS_NewCFunction(ctx, js_engine_getString, "getString", 1));
    JS_SetPropertyStr(ctx, engine, "bind",
                      JS_NewCFunction(ctx, js_engine_bind, "bind", 2));

    JS_SetPropertyStr(ctx, global, "engine", engine);
    JS_FreeValue(ctx, global);

    GLOG_INFO("EngineBridge: registered engine.* SDK functions");
}

} // namespace GryceEngineUtils::ui