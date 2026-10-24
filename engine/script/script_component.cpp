// ===========================================================================
// engine/script/script_component.cpp
// ScriptComponent 实现。设计口径见 script_component.h。
// ===========================================================================

#include "script/script_component.h"

#include <fstream>
#include <sstream>

#include "resources/resource_path.h"
#include "runtime/engine_context.h"
#include "scene/entity.h"
#include "script/js_value.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

ScriptComponent::ScriptComponent(std::string script_path_) : script_path(std::move(script_path_)) {}

// ---------------------------------------------------------------------------
// 组件树访问（engine.component.* 的数据源）
// ---------------------------------------------------------------------------
int ScriptComponent::host_component_count() const {
    if (!owner()) return 0;
    return static_cast<int>(owner()->user_components().size());
}

Component* ScriptComponent::host_component_at(int index) const {
    if (!owner() || index < 0) return nullptr;
    auto comps = owner()->user_components();
    if (index >= static_cast<int>(comps.size())) return nullptr;
    return comps[static_cast<size_t>(index)];
}

// ---------------------------------------------------------------------------
// 序列化
// ---------------------------------------------------------------------------
void ScriptComponent::serialize(nlohmann::json& out) const {
    out["script"] = script_path;
    out["props"] = props;
}

void ScriptComponent::deserialize(const nlohmann::json& in) {
    script_path = in.value("script", std::string{});
    if (in.contains("props") && in["props"].is_object()) {
        props = in["props"];
    } else {
        props = nlohmann::json::object();
    }
    // 反序列化后尚未挂载/编译：on_awake 或首次 on_update 时装载。
    ready_ = false;
    started_ = false;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
void ScriptComponent::on_attach(scene::Entity* owner) {
    // 基类记录 owner_；此处无额外动作（实体句柄经 engine.entity.self 暴露）。
    Component::on_attach(owner);
}

void ScriptComponent::on_awake() {
    if (script_path.empty()) return; // 未配置：静默（编辑器可随后补路径）
    if (load_and_compile()) {
        // 编译成功才进 awake 钩子：加载失败时脚本侧无从执行（错误已记录）。
        call_export("awake", nullptr, 0);
    }
}

void ScriptComponent::on_start() {
    if (!ready_) return;
    started_ = true;
    call_export("start", nullptr, 0);
}

void ScriptComponent::on_update(float dt) {
    dt_ = dt;
    ++frame_;
    elapsed_ += dt;

    // 热重载轮询放在派发之前：无论当前是否 ready（含"文件稍后才出现"的情形）
    // 都先给一次装载机会；装载成功则本帧照常派发 update。
    poll_hot_reload(dt);
    if (!ready_) return; // 仍未装载 / 重载失败：保持 failed 语义

    // 输入事件派发（不消费；宿主在帧起点清理 input_events）。
    const auto& events = gryce_core::g_input_state.input_events;
    if (!events.empty() && exports_.find("_input") != exports_.end()) {
        for (const auto& ev : events) {
            JSValue args[4] = {script::to_js(ctx_, ev.type), script::to_js(ctx_, ev.a),
                               script::to_js(ctx_, ev.b), script::to_js(ctx_, ev.c)};
            call_export("_input", args, 4);
            for (JSValue& a : args) {
                JS_FreeValue(ctx_, a);
            }
            if (!ready_) return; // _input 抛错：本帧中断执行
        }
    }

    JSValue dt_arg = script::to_js(ctx_, dt);
    call_export("update", &dt_arg, 1);
    JS_FreeValue(ctx_, dt_arg);
    if (!ready_) return;

    // Promise 微任务（v1 无独立事件循环，每帧末排空）。
    script::ScriptRuntime::instance().run_pending_jobs();
}

void ScriptComponent::on_destroy() {
    if (ready_) {
        call_export("destroy", nullptr, 0);
    }
    release_context();
    ready_ = false;
    started_ = false;
}

// ---------------------------------------------------------------------------
// 装载 / 编译
// ---------------------------------------------------------------------------
bool ScriptComponent::load_and_compile() {
    release_context();

    if (script_path.empty()) return false;

    resolved_path_ = resources::ResourcePath::resolve(script_path);

    std::ifstream file(resolved_path_, std::ios::binary);
    if (!file) {
        report_error("load", "cannot open script file: " + resolved_path_);
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string source = ss.str();

    // 记录 mtime / 大小（热重载基准）；失败不阻塞装载。
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(resolved_path_, ec);
    have_mtime_ = !ec;
    if (have_mtime_) {
        source_mtime_ = mtime;
        source_size_ = std::filesystem::file_size(resolved_path_, ec);
    }

    auto& runtime = script::ScriptRuntime::instance();
    ctx_ = runtime.create_context();
    if (!ctx_) {
        report_error("load", "JS_NewContext failed (runtime unavailable)");
        return false;
    }
    script::install_engine_bindings(ctx_, this);

    module_ = runtime.eval_module(ctx_, source, resolved_path_);
    if (JS_IsException(module_)) {
        const std::string err = script::take_exception_string(ctx_);
        report_error("compile", err);
        release_context();
        return false;
    }

    // 提取导出（未定义 = 该钩子不存在，正常）。
    static const char* k_hooks[] = {"awake", "start", "update", "destroy", "_input"};
    for (const char* hook : k_hooks) {
        JSValue fn = JS_GetPropertyStr(ctx_, module_, hook);
        if (!JS_IsUndefined(fn)) {
            exports_.emplace(hook, fn);
        } else {
            JS_FreeValue(ctx_, fn);
        }
    }

    // this 对象：注入 props（json -> JS 值）。
    self_obj_ = JS_NewObject(ctx_);
    JS_SetPropertyStr(ctx_, self_obj_, "props", script::to_js(ctx_, props));

    ready_ = true;
    failed_ = false;
    error_repeats_ = 0;
    last_error_.clear();

    // 重新编译（热重载）后若已 start 过，立即补一次 start。
    if (started_) {
        call_export("start", nullptr, 0);
    }
    return true;
}

void ScriptComponent::release_context() {
    if (ctx_) {
        for (auto& kv : exports_) {
            JS_FreeValue(ctx_, kv.second);
        }
        exports_.clear();
        JS_FreeValue(ctx_, self_obj_);
        self_obj_ = JS_UNDEFINED;
        JS_FreeValue(ctx_, module_);
        module_ = JS_UNDEFINED;
        script::ScriptRuntime::instance().destroy_context(ctx_);
        ctx_ = nullptr;
    }
    ready_ = false;
}

// ---------------------------------------------------------------------------
// 钩子调用 / 错误隔离
// ---------------------------------------------------------------------------
bool ScriptComponent::call_export(const char* name, JSValue* args, int argc) {
    if (!ready_ || !ctx_) return false;
    auto it = exports_.find(name);
    if (it == exports_.end()) return true; // 钩子未定义：正常路径

    JSValue result = script::ScriptRuntime::instance().call(ctx_, it->second, self_obj_, argc, args);
    if (JS_IsException(result)) {
        const std::string err = script::take_exception_string(ctx_);
        JS_FreeValue(ctx_, result);
        report_error(name, err);
        return false;
    }
    JS_FreeValue(ctx_, result);
    return true;
}

void ScriptComponent::report_error(const char* stage, const std::string& message) {
    failed_ = true;
    if (message != last_error_) {
        last_error_ = message;
        error_repeats_ = 0;
        GLOG_ERROR("[script] {} ({}): {}", script_path.empty() ? "<unnamed>" : script_path.c_str(),
                   stage, message);
    } else if (++error_repeats_ % 100 == 1) {
        GLOG_ERROR("[script] {} ({}): {} (x{} identical)", script_path.c_str(), stage, message,
                   error_repeats_);
    }
}

// ---------------------------------------------------------------------------
// 命令面
// ---------------------------------------------------------------------------
void ScriptComponent::set_script_path(std::string path) {
    if (path == script_path && ready_) return;
    script_path = std::move(path);
    resolved_path_.clear();
    have_mtime_ = false;
    source_size_ = 0;
    started_ = false;
    if (script_path.empty()) {
        // 清空路径 = 卸载脚本：释放上下文，保留 props（再次填路径可复用）。
        release_context();
        failed_ = false;
        last_error_.clear();
        return;
    }
    if (load_and_compile()) {
        call_export("awake", nullptr, 0);
    }
}

void ScriptComponent::reload_now() {
    if (script_path.empty()) return;
    resolved_path_.clear();
    have_mtime_ = false;
    // load_and_compile 内部对「已 start 过」的组件会自动补一次 start；
    // 未 start 过的（编辑器里手动重载）按刚装载处理，补一次 awake。
    if (!load_and_compile()) return;
    if (!started_) call_export("awake", nullptr, 0);
}

// ---------------------------------------------------------------------------
// 热重载
// ---------------------------------------------------------------------------
void ScriptComponent::poll_hot_reload(float dt) {
    if (reload_poll_interval <= 0.0f || script_path.empty()) return;
    reload_accum_ += dt;
    if (reload_accum_ < reload_poll_interval) return;
    reload_accum_ = 0.0f;

    if (resolved_path_.empty()) {
        resolved_path_ = resources::ResourcePath::resolve(script_path);
    }

    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(resolved_path_, ec);
    if (ec) return; // 文件仍不存在：保持现状
    if (!have_mtime_) {
        // 此前装载失败（打开时文件缺失）：文件出现即装载，而不是只记基准——
        // 只记基准会让本帧与后续帧都认为"无变化"，文件永远加载不上来。
        GLOG_INFO("[script] load (file appeared): {}", resolved_path_);
        load_and_compile();
        return;
    }

    // 双判据：mtime 之外还比大小。部分文件系统（FAT/exFAT、网络盘、某些
    // 虚拟盘）的写入时间粒度达 1~2 秒，编辑器连续保存会落在同一 tick 里，
    // 只比 mtime 会漏掉重载；同 tick 内的改动通常长度也变，大小判据能兜住。
    std::error_code size_ec;
    const auto size = std::filesystem::file_size(resolved_path_, size_ec);
    const bool size_changed = !size_ec && size != source_size_;
    if (mtime == source_mtime_ && !size_changed) return;

    GLOG_INFO("[script] reload: {}", resolved_path_);
    load_and_compile();
}

} // namespace gryce_engine::components
