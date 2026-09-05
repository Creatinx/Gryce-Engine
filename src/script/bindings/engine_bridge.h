#pragma once

// EngineBridge — 将引擎核心 SDK 函数注册到 ScriptVM 的 JS 全局作用域
//
// 将 engine.* 对象注册到 ScriptVM 的 JS 全局作用域，包含：
// - engine.log.info / warn / error
// - engine.version()
// - engine.self()
// - engine.entity.* (get_name, find, find_all, create, destroy, aabb, get_transform, set_transform)
// - engine.component.* (has, get, set)
// - engine.state.* (get, set, has)
// - engine.input.* (key_down, mouse_pos, mouse_down)
// - engine.time.* (delta, elapsed)
// - engine.scene.* (load, current)
// - engine.audio.play_on
// - engine.fx.burst
// - engine.json.read
// - engine.physics.* (set_gravity, get_gravity)
// - engine.showDialog (UI)
// - engine.closeDialog (UI)
// - engine.bind (UI)

#include <string>

#include <quickjs/quickjs.h>

#include "export.h"

namespace GryceEngineUtils::script {

class ScriptVM;

// 注册所有 engine.* 绑定到 ScriptVM 的 JS 上下文
	GRYCE_API void register_engine_bindings(JSContext* ctx);

} // namespace GryceEngineUtils::script