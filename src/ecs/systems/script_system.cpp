#include "ecs/systems/script_system.h"

#include "components/script_component.h"
#include "assets/asset_manager.h"
#include "resources/resource_path.h"
#include "scene/entity.h"
#include "scene/scene.h"
#include "ecs/world.h"
#include "runtime/engine_context.h"
#include "script/runtime/script_context.h"
#include "script/bindings/engine_bridge.h"
#include "script/bindings/math_bridge.h"
#include "script/bindings/big_bridge.h"
#include "utils/glog/glog_lib.h"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace gryce_engine::ecs {

namespace {

inline bool contains(const std::vector<components::ScriptComponent*>& v,
                     components::ScriptComponent* c) {
    return std::find(v.begin(), v.end(), c) != v.end();
}

} // namespace

// ============================================================================
// 全局 ScriptVM 实例（延迟初始化，首次加载脚本时自动创建）
// ============================================================================

GryceEngineUtils::script::ScriptVM& ScriptSystem::vm() {
    static GryceEngineUtils::script::ScriptVM instance;
    static bool initialized = false;
    if (!initialized) {
        if (instance.init()) {
            // 注册 engine.* / math.* / big.* 绑定
            GryceEngineUtils::script::register_engine_bindings(instance.context());
            GryceEngineUtils::script::register_math_bindings(instance.context());
            GryceEngineUtils::script::register_big_bindings(instance.context());
            initialized = true;
            GLOG_INFO("ScriptSystem: global ScriptVM initialized with engine.* / math.* / big.* bindings");
        } else {
            GLOG_ERROR("ScriptSystem: failed to initialize global ScriptVM");
        }
    }
    return instance;
}

// ============================================================================
// on_update
// ============================================================================

void ScriptSystem::on_update(scene::Scene& scene, float dt) {
    auto& rt = ScriptSystem::vm();
    if (!rt.initialized()) return;

    auto& ctx = GryceEngineUtils::script::ScriptContext::instance();
    ctx.set_delta_time(dt);
    ctx.set_current_scene(&scene);
    ctx.reset_frame();
    // 将核心本轮已同步的"按下键集合"镜像进脚本上下文，使 engine.input.key_down 生效
    ctx.set_held_keys(gryce_core::g_core_state.input.keys_down);
    seen_.clear();

    // 输入事件分发
    dispatch_input_events();

    // 快照遍历
    std::vector<scene::Entity*> entities;
    scene.root()->foreach([&](scene::Entity* e) {
        entities.push_back(e);
    });

    // 按 process_priority 降序收集
    std::vector<components::ScriptComponent*> comps;
    for (scene::Entity* e : entities) {
        auto* comp = e->get_component<components::ScriptComponent>();
        if (!comp) continue;
        comps.push_back(comp);
        seen_.push_back(comp);
    }
    std::stable_sort(comps.begin(), comps.end(),
        [](components::ScriptComponent* a, components::ScriptComponent* b) {
            return a->process_priority > b->process_priority;
        });
    for (components::ScriptComponent* comp : comps) {
        process_entity(comp, dt);
    }

    // 清理已移除的组件
    loaded_.erase(std::remove_if(loaded_.begin(), loaded_.end(),
        [&](components::ScriptComponent* c) {
            if (contains(seen_, c)) return false;
            unload(c);
            return true;
        }),
        loaded_.end());
}

void ScriptSystem::process_entity(components::ScriptComponent* comp, float dt) {
    if (!comp) return;

    if (!comp->enabled || comp->script_path.empty()) {
        if (comp->script_loaded) unload(comp);
        return;
    }

    if (!comp->script_loaded) {
        if (!load(comp)) {
            handle_error(comp);
            return;
        }
    }

    // 全局暂停过滤
    if (gryce_core::g_core_state.paused && !comp->pause_mode) {
        return;
    }

    // 错误暂停：on_start/on_update 抛异常后停止驱动，直到重载
    if (comp->paused_on_error) {
        return;
    }

    call_method(comp, "on_update", dt, true);
}

void ScriptSystem::dispatch_input_events() {
    auto& state = gryce_core::g_core_state;
    if (state.input.input_events.empty()) return;

    std::vector<components::ScriptComponent*> comps;
    if (state.world) {
        scene::Scene* scene = state.world->scene();
        if (scene) {
            scene->root()->foreach([&](scene::Entity* e) {
                auto* c = e->get_component<components::ScriptComponent>();
                if (c && c->script_loaded) comps.push_back(c);
            });
        }
    }

    for (const auto& ev : state.input.input_events) {
        for (components::ScriptComponent* comp : comps) {
            call_method_int(comp, "_input", ev.type, ev.a, ev.b, ev.c);
        }
    }
    state.input.input_events.clear();
}

// ============================================================================
// 加载 & 卸载
// ============================================================================

bool ScriptSystem::load(components::ScriptComponent* comp) {
    auto& rt = ScriptSystem::vm();
    JSContext* ctx = rt.context();
    if (!ctx) return false;

    comp->last_error.clear();
    comp->reported_error = false;

    const std::string full = assets::AssetManager::instance().resolve_for_reading(comp->script_path);
    if (full.empty()) {
        comp->last_error = "cannot resolve script: " + comp->script_path;
        return false;
    }

    std::string src;
    auto it = source_cache_.find(comp->script_path);
    if (it != source_cache_.end()) {
        src = it->second;
    } else {
        std::ifstream in(full, std::ios::binary);
        if (!in) {
            comp->last_error = "cannot open script file: " + full;
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        src = ss.str();
        source_cache_[comp->script_path] = src;
    }
    if (src.empty()) {
        comp->last_error = "empty script: " + comp->script_path;
        return false;
    }

    // 记住旧 prop 值用于热重载
    const auto old_props = comp->props;

    // 加载模块
    JSValue module_ns = JS_UNDEFINED;
    std::string module_filename = comp->script_path;
    // 替换路径分隔符，使文件名在错误信息中可读
    auto result = rt.eval_module(src, module_filename, &module_ns);

    if (!result.success) {
        comp->last_error = result.error_msg;
        if (!JS_IsUndefined(module_ns)) JS_FreeValue(ctx, module_ns);
        return false;
    }

    // 存储模块命名空间
    module_ns_map_[comp] = JS_DupValue(ctx, module_ns);
    JS_FreeValue(ctx, module_ns);

    comp->script_loaded = true;
    comp->start_called = false;
    comp->reported_error = false;
    comp->paused_on_error = false;

    // 登记到 loaded_，使 reload_all / on_shutdown 能真正卸载并释放模块
    if (!contains(loaded_, comp)) loaded_.push_back(comp);

    // 调用 on_start
    call_method(comp, "on_start");
    if (comp->paused_on_error) {
        comp->last_error = "on_start failed: " + comp->last_error;
    }

    // 同步 props
    sync_props_from_env(comp);

    // 热重载：恢复旧 prop 值
    for (const auto& old : old_props) {
        for (auto& p : comp->props) {
            if (p.name == old.name && p.type == old.type) {
                if (p.type == 1) {
                    p.s = old.s;
                    write_prop_to_env(comp, old.name.c_str(), old.s);
                } else {
                    p.f = old.f;
                    write_prop_to_env(comp, old.name.c_str(), old.f);
                }
                break;
            }
        }
    }

    return true;
}

void ScriptSystem::unload(components::ScriptComponent* comp) {
    if (comp->script_loaded) {
        call_method(comp, "on_destroy");
    }

    auto& rt = ScriptSystem::vm();
    JSContext* ctx = rt.context();
    if (ctx) {
        auto it = module_ns_map_.find(comp);
        if (it != module_ns_map_.end()) {
            JS_FreeValue(ctx, it->second);
            module_ns_map_.erase(it);
        }
    }

    comp->script_loaded = false;
    comp->start_called = false;
    comp->paused_on_error = false;
}

// ============================================================================
// 方法调用
// ============================================================================

void ScriptSystem::call_method(components::ScriptComponent* comp,
                                const char* method, float arg, bool has_arg) {
    auto& rt = ScriptSystem::vm();
    JSContext* ctx = rt.context();
    if (!ctx) return;

    auto it = module_ns_map_.find(comp);
    if (it == module_ns_map_.end()) return;

    // 生命周期方法可选：模块未导出时静默跳过（不视为错误）
    JSValue func = JS_GetPropertyStr(ctx, it->second, method);
    const bool has_method = JS_IsFunction(ctx, func);
    JS_FreeValue(ctx, func);
    if (!has_method) return;

    // 设置当前实体上下文（使用 GEntityHandle）
    {
        auto* owner = comp->owner();
        int handle = 0;
        if (owner) {
            handle = gryce_core::g_core_state.entity_map.lookup(owner->uuid());
        }
        GryceEngineUtils::script::ScriptContext::instance().set_current_entity(handle);
    }

    std::vector<GryceEngineUtils::script::JSValueWrapper> args;
    if (has_arg) args.push_back(GryceEngineUtils::script::JSValueWrapper(arg));

    auto result = rt.call_module_function(it->second, method, args);
    if (!result.success) {
        comp->last_error = result.error_msg;
        handle_error(comp);
        // 生命周期方法抛异常则暂停该实体（on_destroy 除外，避免阻塞卸载）
        if (std::strcmp(method, "on_destroy") != 0) {
            comp->paused_on_error = true;
        }
    }
}

void ScriptSystem::call_method_int(components::ScriptComponent* comp,
                                    const char* method, int type, int a, int b, int c) {
    auto& rt = ScriptSystem::vm();
    JSContext* ctx = rt.context();
    if (!ctx) return;

    auto it = module_ns_map_.find(comp);
    if (it == module_ns_map_.end()) return;

    // 事件方法可选：模块未导出时静默跳过
    JSValue func = JS_GetPropertyStr(ctx, it->second, method);
    const bool has_method = JS_IsFunction(ctx, func);
    JS_FreeValue(ctx, func);
    if (!has_method) return;

    // 设置当前实体上下文（使用 GEntityHandle）
    {
        auto* owner = comp->owner();
        int handle = 0;
        if (owner) {
            handle = gryce_core::g_core_state.entity_map.lookup(owner->uuid());
        }
        GryceEngineUtils::script::ScriptContext::instance().set_current_entity(handle);
    }

    auto result = rt.call_module_function(it->second, method, {
        GryceEngineUtils::script::JSValueWrapper(type),
        GryceEngineUtils::script::JSValueWrapper(a),
        GryceEngineUtils::script::JSValueWrapper(b),
        GryceEngineUtils::script::JSValueWrapper(c)
    });
    if (!result.success) {
        comp->last_error = result.error_msg;
        handle_error(comp);
    }
}

// ============================================================================
// Props 同步
// ============================================================================

void ScriptSystem::sync_props_from_env(components::ScriptComponent* comp) {
    auto& rt = ScriptSystem::vm();
    JSContext* ctx = rt.context();
    if (!ctx) return;

    auto it = module_ns_map_.find(comp);
    if (it == module_ns_map_.end()) return;

    comp->props.clear();

    // 读取模块的 props 导出
    JSValue props_val = JS_GetPropertyStr(ctx, it->second, "props");
    if (JS_IsObject(props_val)) {
        JSPropertyEnum* props_tab = nullptr;
        uint32_t props_len = 0;
        int ret = JS_GetOwnPropertyNames(ctx, &props_tab, &props_len, props_val,
                                          JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY);
        if (ret == 0) {
            for (uint32_t i = 0; i < props_len; ++i) {
                const char* key = JS_AtomToCString(ctx, props_tab[i].atom);
                if (!key) continue;

                JSValue val = JS_GetProperty(ctx, props_val, props_tab[i].atom);
                components::ScriptProp p;
                p.name = key;

                if (JS_IsNumber(val)) {
                    p.type = 0;
                    double d;
                    JS_ToFloat64(ctx, &d, val);
                    p.f = static_cast<float>(d);
                } else if (JS_IsString(val)) {
                    p.type = 1;
                    const char* s = JS_ToCString(ctx, val);
                    if (s) { p.s = s; JS_FreeCString(ctx, s); }
                }

                comp->props.push_back(std::move(p));
                JS_FreeValue(ctx, val);
                JS_FreeCString(ctx, key);
            }
            js_free(ctx, props_tab);
        }
    }
    JS_FreeValue(ctx, props_val);
}

bool ScriptSystem::get_prop(components::ScriptComponent* comp, const char* name,
                            int& out_type, float& out_f, std::string& out_s) {
    if (!comp || !name) return false;
    for (const auto& p : comp->props) {
        if (p.name == name) {
            out_type = p.type;
            out_f = p.f;
            out_s = p.s;
            return true;
        }
    }
    return false;
}

bool ScriptSystem::set_prop(components::ScriptComponent* comp, const char* name, float value) {
    if (!comp || !name) return false;
    for (auto& p : comp->props) {
        if (p.name == name && p.type == 0) {
            p.f = value;
            write_prop_to_env(comp, name, value);
            return true;
        }
    }
    return false;
}

bool ScriptSystem::set_prop(components::ScriptComponent* comp, const char* name,
                            const std::string& value) {
    if (!comp || !name) return false;
    for (auto& p : comp->props) {
        if (p.name == name && p.type == 1) {
            p.s = value;
            write_prop_to_env(comp, name, value);
            return true;
        }
    }
    return false;
}

void ScriptSystem::write_prop_to_env(components::ScriptComponent* comp,
                                     const char* name, float value) {
    auto& rt = ScriptSystem::vm();
    JSContext* ctx = rt.context();
    if (!ctx) return;

    auto it = module_ns_map_.find(comp);
    if (it == module_ns_map_.end()) return;

    // 写入模块导出的 props 对象（不存在则创建）
    JSValue props = JS_GetPropertyStr(ctx, it->second, "props");
    if (!JS_IsObject(props)) {
        JS_FreeValue(ctx, props);
        props = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, it->second, "props", JS_DupValue(ctx, props));
    }
    JS_SetPropertyStr(ctx, props, name, JS_NewFloat64(ctx, value));
    JS_FreeValue(ctx, props);
}

void ScriptSystem::write_prop_to_env(components::ScriptComponent* comp,
                                     const char* name, const std::string& value) {
    auto& rt = ScriptSystem::vm();
    JSContext* ctx = rt.context();
    if (!ctx) return;

    auto it = module_ns_map_.find(comp);
    if (it == module_ns_map_.end()) return;

    // 写入模块导出的 props 对象（不存在则创建）
    JSValue props = JS_GetPropertyStr(ctx, it->second, "props");
    if (!JS_IsObject(props)) {
        JS_FreeValue(ctx, props);
        props = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, it->second, "props", JS_DupValue(ctx, props));
    }
    JS_SetPropertyStr(ctx, props, name, JS_NewString(ctx, value.c_str()));
    JS_FreeValue(ctx, props);
}

// ============================================================================
// 错误处理
// ============================================================================

void ScriptSystem::handle_error(components::ScriptComponent* comp) {
    if (comp->reported_error) return;
    comp->reported_error = true;
    GLOG_ERROR("ScriptSystem: script error on '{}': {}",
               comp->owner() ? comp->owner()->name() : "?",
               comp->last_error);
}

// ============================================================================
// 热重载 & 关闭
// ============================================================================

void ScriptSystem::reload_all() {
    source_cache_.clear();
    for (auto* comp : loaded_) unload(comp);
    loaded_.clear();
    seen_.clear();
}

void ScriptSystem::on_shutdown(scene::Scene& scene) {
    (void)scene;
    reload_all();
    GryceEngineUtils::script::ScriptContext::instance().set_current_scene(nullptr);
}

} // namespace gryce_engine::ecs
