#include "engine_bridge.h"

#include <cstring>
#include <string>

#include "GryceCore/core_api.h"
#include "GryceCore/types.h"

#include "script/runtime/script_vm.h"
#include "script/bindings/engine_bridge.h"
#include "script/bindings/math_bridge.h"
#include "script/bindings/big_bridge.h"

#include "script_vm.h"
#include "GryceEngineUtils/ui/ui.h"
#include "GryceEngineUtils/ui/label.h"
#include "GryceEngineUtils/ui/widget.h"
#include "GryceEngineUtils/ecs/world.h"
#include "GryceEngineUtils/ecs/entity.h"
#include "GryceEngineUtils/renderer.h"
#include "GryceEngineUtils/math.h"

#include "components/mesh_renderer.h"
#include "components/transform.h"

#include <cstdint>
#include <unordered_map>

#include "utils/glog/glog_lib.h"

namespace GryceEngineUtils::ui {

// ============================================================================
// 静态成员
// ============================================================================

UIManager* EngineBridge::ui_manager_ = nullptr;
ScriptVM* EngineBridge::vm_ = nullptr;
GryceEngineUtils::GameRuntime EngineBridge::game_runtime_;

namespace {

// engine.game.* 桥接 —— 实体 id 映射与按键名映射
struct GameState {
    int64_t next_id = 1;
    std::unordered_map<int64_t, gryce_engine::scene::Entity*> entities;
};
GameState g_game_state;

const std::unordered_map<std::string, int>& game_key_map() {
    // 名字 -> GLFW 键码
    static const std::unordered_map<std::string, int> k = {
        {"left", 263}, {"right", 262}, {"up", 265}, {"down", 264},
        {"space", 32}, {"enter", 257},
        {"a", 65}, {"d", 68}, {"w", 87}, {"s", 83}, {"r", 82}, {"x", 88}, {"shift", 340},
    };
    return k;
}

gryce_engine::scene::Entity* game_find(int64_t id) {
    auto it = g_game_state.entities.find(id);
    return it == g_game_state.entities.end() ? nullptr : it->second;
}

} // namespace

// ============================================================================
// engine.game.* —— 让 QuickJS 玩法脚本驱动 3D ECS 世界 / 输入 / UI 控件
// ============================================================================

// game.create(name, sx, sy, sz, r, g, b) -> id
static JSValue js_game_create(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    if (!EngineBridge::game_runtime().world) {
        return JS_ThrowTypeError(ctx, "game.create: no game world injected");
    }
    if (argc < 7) {
        return JS_ThrowTypeError(ctx, "game.create: name, scale, color args required");
    }
    double sx = 0, sy = 0, sz = 0, cr = 1, cg = 1, cb = 1;
    JS_ToFloat64(ctx, &sx, argv[1]);
    JS_ToFloat64(ctx, &sy, argv[2]);
    JS_ToFloat64(ctx, &sz, argv[3]);
    JS_ToFloat64(ctx, &cr, argv[4]);
    JS_ToFloat64(ctx, &cg, argv[5]);
    JS_ToFloat64(ctx, &cb, argv[6]);
    const char* nm = JS_ToCString(ctx, argv[0]);

    int64_t id = g_game_state.next_id++;
    auto* w = EngineBridge::game_runtime().world;
    auto* e = w->create_entity(nm ? nm : "obj");
    e->transform()->position = math::Vector3f::zero();
    e->transform()->scale = math::Vector3f((float)sx, (float)sy, (float)sz);
    if (auto* mr = e->add_component<gryce_engine::components::MeshRenderer>("res:/models/cube_pbr.obj");
        mr && mr->material) {
        mr->material->set_albedo(math::Vector3f((float)cr, (float)cg, (float)cb));
        mr->material->set_roughness(0.65f);
        mr->material->set_metallic(0.0f);
    }
    if (nm) {
        JS_FreeCString(ctx, nm);
    }
    g_game_state.entities[id] = e;
    return JS_NewInt64(ctx, id);
}

// game.move(id, x, y, z)
static JSValue js_game_move(JSContext* ctx, JSValueConst this_val,
                            int argc, JSValueConst* argv) {
    int64_t id = 0;
    if (JS_ToInt64(ctx, &id, argv[0]) < 0) {
        return JS_ThrowTypeError(ctx, "game.*: id must be an integer");
    }
    double x = 0, y = 0, z = 0;
    JS_ToFloat64(ctx, &x, argv[1]);
    JS_ToFloat64(ctx, &y, argv[2]);
    JS_ToFloat64(ctx, &z, argv[3]);
    if (auto* e = game_find(id)) {
        e->transform()->position = math::Vector3f((float)x, (float)y, (float)z);
    }
    return JS_UNDEFINED;
}

// game.pos(id) -> {x, y, z}
static JSValue js_game_pos(JSContext* ctx, JSValueConst this_val,
                           int argc, JSValueConst* argv) {
    int64_t id = 0;
    if (JS_ToInt64(ctx, &id, argv[0]) < 0) {
        return JS_ThrowTypeError(ctx, "game.*: id must be an integer");
    }
    auto* e = game_find(id);
    if (!e) {
        return JS_NULL;
    }
    math::Vector3f p = e->transform()->position;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "x", JS_NewFloat64(ctx, (double)p.x));
    JS_SetPropertyStr(ctx, o, "y", JS_NewFloat64(ctx, (double)p.y));
    JS_SetPropertyStr(ctx, o, "z", JS_NewFloat64(ctx, (double)p.z));
    return o;
}

// game.scale(id, x, y, z)
static JSValue js_game_scale(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    int64_t id = 0;
    if (JS_ToInt64(ctx, &id, argv[0]) < 0) {
        return JS_ThrowTypeError(ctx, "game.*: id must be an integer");
    }
    double x = 0, y = 0, z = 0;
    JS_ToFloat64(ctx, &x, argv[1]);
    JS_ToFloat64(ctx, &y, argv[2]);
    JS_ToFloat64(ctx, &z, argv[3]);
    if (auto* e = game_find(id)) {
        e->transform()->scale = math::Vector3f((float)x, (float)y, (float)z);
    }
    return JS_UNDEFINED;
}

// game.destroy(id)
static JSValue js_game_destroy(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    int64_t id = 0;
    if (JS_ToInt64(ctx, &id, argv[0]) < 0) {
        return JS_ThrowTypeError(ctx, "game.*: id must be an integer");
    }
    if (auto it = g_game_state.entities.find(id); it != g_game_state.entities.end()) {
        if (EngineBridge::game_runtime().world && it->second) {
            EngineBridge::game_runtime().world->destroy_entity(it->second);
        }
        g_game_state.entities.erase(it);
    }
    return JS_UNDEFINED;
}

// game.clear() —— 销毁玩法脚本创建的所有实体（用于重开一局）
static JSValue js_game_clear(JSContext* ctx, JSValueConst this_val,
                             int argc, JSValueConst* argv) {
    auto* w = EngineBridge::game_runtime().world;
    if (w) {
        for (auto& [id, e] : g_game_state.entities) {
            if (e) {
                w->destroy_entity(e);
            }
        }
    }
    g_game_state.entities.clear();
    g_game_state.next_id = 1;
    return JS_UNDEFINED;
}

// game.held(name) -> bool —— 按住
static JSValue js_game_held(JSContext* ctx, JSValueConst this_val,
                            int argc, JSValueConst* argv) {
    auto* rndr = EngineBridge::game_runtime().renderer;
    if (!rndr) {
        return JS_FALSE;
    }
    const char* name = JS_ToCString(ctx, argv[0]);
    bool down = false;
    if (name) {
        auto it = game_key_map().find(name);
        down = it != game_key_map().end() && rndr->key_held(it->second);
        JS_FreeCString(ctx, name);
    }
    return JS_NewBool(ctx, down);
}

// game.pressed(name) -> bool —— 本帧刚按下
static JSValue js_game_pressed(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* rndr = EngineBridge::game_runtime().renderer;
    if (!rndr) {
        return JS_FALSE;
    }
    const char* name = JS_ToCString(ctx, argv[0]);
    bool down = false;
    if (name) {
        auto it = game_key_map().find(name);
        down = it != game_key_map().end() && rndr->key_pressed(it->second);
        JS_FreeCString(ctx, name);
    }
    return JS_NewBool(ctx, down);
}

// game.set_label(id, text) —— 更新 Label 控件文本
static JSValue js_game_set_label(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* mgr = EngineBridge::ui_manager();
    if (!mgr || argc < 2) {
        return JS_UNDEFINED;
    }
    const char* wid = JS_ToCString(ctx, argv[0]);
    const char* text = JS_ToCString(ctx, argv[1]);
    if (wid && text && mgr->root()) {
        if (auto* wdg = mgr->root()->find_element_by_id(wid)) {
            if (auto* lb = dynamic_cast<Label*>(wdg)) {
                lb->set_text(text);
            }
        }
    }
    if (wid) {
        JS_FreeCString(ctx, wid);
    }
    if (text) {
        JS_FreeCString(ctx, text);
    }
    return JS_UNDEFINED;
}

// game.set_visible(id, visible) —— 显示/隐藏控件
static JSValue js_game_set_visible(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    auto* mgr = EngineBridge::ui_manager();
    if (!mgr || argc < 2) {
        return JS_UNDEFINED;
    }
    const char* wid = JS_ToCString(ctx, argv[0]);
    bool vis = JS_ToBool(ctx, argv[1]);
    if (wid && mgr->root()) {
        if (auto* wdg = mgr->root()->find_element_by_id(wid)) {
            wdg->set_visible(vis);
        }
    }
    if (wid) {
        JS_FreeCString(ctx, wid);
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
// EngineBridge::init — 使用统一脚本系统注册 engine.* + UI 专用函数
// ============================================================================

void EngineBridge::init(ScriptVM* vm, UIManager* ui_mgr) {
    if (!vm || !vm->initialized()) {
        GLOG_WARN("EngineBridge: ScriptVM not initialized, skipping bridge init");
        return;
    }

    vm_ = vm;
    ui_manager_ = ui_mgr;

    JSContext* ctx = vm->context();

    // 1. 由统一脚本系统注册完整的 engine.* API
    GryceEngineUtils::script::register_engine_bindings(ctx);
    GryceEngineUtils::script::register_math_bindings(ctx);
    GryceEngineUtils::script::register_big_bindings(ctx);

    // 2. 在现有 engine 对象上添加 UI 专用函数
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue engine = JS_GetPropertyStr(ctx, global, "engine");

    if (JS_IsObject(engine)) {
        JS_SetPropertyStr(ctx, engine, "showDialog",
                          JS_NewCFunction(ctx, js_engine_showDialog, "showDialog", 1));
        JS_SetPropertyStr(ctx, engine, "closeDialog",
                          JS_NewCFunction(ctx, js_engine_closeDialog, "closeDialog", 0));
        JS_SetPropertyStr(ctx, engine, "bind",
                          JS_NewCFunction(ctx, js_engine_bind, "bind", 2));

        // engine.game.* —— 玩法桥接（生成/移动/销毁 3D 实体、输入、UI 控件）
        JSValue game = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, game, "create",
                          JS_NewCFunction(ctx, js_game_create, "create", 7));
        JS_SetPropertyStr(ctx, game, "move",
                          JS_NewCFunction(ctx, js_game_move, "move", 4));
        JS_SetPropertyStr(ctx, game, "pos",
                          JS_NewCFunction(ctx, js_game_pos, "pos", 1));
        JS_SetPropertyStr(ctx, game, "scale",
                          JS_NewCFunction(ctx, js_game_scale, "scale", 4));
        JS_SetPropertyStr(ctx, game, "destroy",
                          JS_NewCFunction(ctx, js_game_destroy, "destroy", 1));
        JS_SetPropertyStr(ctx, game, "clear",
                          JS_NewCFunction(ctx, js_game_clear, "clear", 0));
        JS_SetPropertyStr(ctx, game, "held",
                          JS_NewCFunction(ctx, js_game_held, "held", 1));
        JS_SetPropertyStr(ctx, game, "pressed",
                          JS_NewCFunction(ctx, js_game_pressed, "pressed", 1));
        JS_SetPropertyStr(ctx, game, "setLabel",
                          JS_NewCFunction(ctx, js_game_set_label, "setLabel", 2));
        JS_SetPropertyStr(ctx, game, "setLabelVisible",
                          JS_NewCFunction(ctx, js_game_set_visible, "setLabelVisible", 2));
        JS_SetPropertyStr(ctx, engine, "game", game);
    }

    JS_FreeValue(ctx, engine);
    JS_FreeValue(ctx, global);

    GLOG_INFO("EngineBridge: unified engine.* + game.* + UI extensions registered");
}

} // namespace GryceEngineUtils::ui