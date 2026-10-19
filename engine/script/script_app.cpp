// ===========================================================================
// engine/script/script_app.cpp
// ScriptApplication 实现。设计口径见 script_app.h。
// ===========================================================================

#include "script/script_app.h"

#include <fstream>
#include <sstream>

#include "resources/resource_path.h"
#include "script/js_runtime.h"
#include "script/js_value.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::script {

namespace {
constexpr const char* kHookNames[] = {"on_start", "on_update", "on_destroy"};
}

ScriptApplication::~ScriptApplication() {
    shutdown();
}

// ---------------------------------------------------------------------------
// 装载 / 编译
// ---------------------------------------------------------------------------
bool ScriptApplication::load(const std::string& path, std::string* error) {
    entry_path_ = path;
    if (!compile_and_link(error)) return false;
    return true;
}

bool ScriptApplication::compile_and_link(std::string* error) {
    release();

    if (entry_path_.empty()) {
        if (error) *error = "entry path is empty";
        return false;
    }

    resolved_path_ = resources::ResourcePath::resolve(entry_path_);
    std::ifstream file(resolved_path_, std::ios::binary);
    if (!file) {
        report_error("load", "cannot open entry script: " + resolved_path_);
        if (error) *error = last_error_;
        return false;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    const std::string source = ss.str();

    // 热重载基准：mtime + 大小（见 poll_hot_reload 的双判据说明）
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(resolved_path_, ec);
    have_mtime_ = !ec;
    if (have_mtime_) {
        source_mtime_ = mtime;
        source_size_ = std::filesystem::file_size(resolved_path_, ec);
    }

    auto& runtime = ScriptRuntime::instance();
    ctx_ = runtime.create_context();
    if (!ctx_) {
        report_error("load", "JS_NewContext failed (runtime unavailable)");
        if (error) *error = last_error_;
        return false;
    }

    // 基础绑定：engine.log / time / input / entity。宿主附加绑定（窗口、
    // 场景、渲染）通过 on_context_created 挂进来，顺序在基础绑定之后。
    install_engine_bindings(ctx_, this);
    if (on_context_created) on_context_created(ctx_);

    module_ = runtime.eval_module(ctx_, source, resolved_path_);
    if (JS_IsException(module_)) {
        const std::string err = take_exception_string(ctx_);
        report_error("compile", err);
        if (error) *error = err;
        release();
        return false;
    }

    for (const char* name : kHookNames) {
        JSValue fn = JS_GetPropertyStr(ctx_, module_, name);
        if (!JS_IsUndefined(fn)) {
            hooks_.emplace(name, fn);
        } else {
            JS_FreeValue(ctx_, fn);
        }
    }

    self_obj_ = JS_NewObject(ctx_);

    ready_ = true;
    failed_ = false;
    error_repeats_ = 0;
    last_error_.clear();

    GLOG_INFO("[script] app entry loaded: {}", resolved_path_);

    // 热重载后已 start 过：补一次 on_start，让脚本重新建立运行时状态。
    if (started_) {
        call_hook("on_start", nullptr, 0);
    }
    return true;
}

void ScriptApplication::release() {
    if (!ctx_) return;
    for (auto& kv : hooks_) JS_FreeValue(ctx_, kv.second);
    hooks_.clear();
    if (!JS_IsUndefined(self_obj_)) {
        JS_FreeValue(ctx_, self_obj_);
        self_obj_ = JS_UNDEFINED;
    }
    if (!JS_IsUndefined(module_)) {
        JS_FreeValue(ctx_, module_);
        module_ = JS_UNDEFINED;
    }
    ScriptRuntime::instance().destroy_context(ctx_);
    ctx_ = nullptr;
    ready_ = false;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
void ScriptApplication::start() {
    if (!ready_ || failed_) return;
    started_ = true;
    call_hook("on_start", nullptr, 0);
}

void ScriptApplication::tick(float dt) {
    dt_ = dt;
    ++frame_;
    elapsed_ += dt;

    poll_hot_reload(dt);
    if (!ready_ || failed_) return;

    JSValue dt_arg = to_js(ctx_, dt);
    call_hook("on_update", &dt_arg, 1);
    JS_FreeValue(ctx_, dt_arg);

    if (!ready_) return;
    ScriptRuntime::instance().run_pending_jobs();
}

void ScriptApplication::shutdown() {
    if (ready_ && ctx_) {
        call_hook("on_destroy", nullptr, 0);
    }
    release();
    started_ = false;
}

bool ScriptApplication::reload_now() {
    if (entry_path_.empty()) return false;
    resolved_path_.clear();
    have_mtime_ = false;
    if (!compile_and_link(nullptr)) return false;
    if (!started_) {
        started_ = true;
        call_hook("on_start", nullptr, 0);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 钩子调用 / 错误隔离
// ---------------------------------------------------------------------------
bool ScriptApplication::call_hook(const char* name, JSValue* args, int argc) {
    if (!ready_ || !ctx_) return false;
    auto it = hooks_.find(name);
    if (it == hooks_.end()) return true; // 未定义即正常

    JSValue result = ScriptRuntime::instance().call(ctx_, it->second, self_obj_, argc, args);
    if (JS_IsException(result)) {
        const std::string err = take_exception_string(ctx_);
        JS_FreeValue(ctx_, result);
        report_error(name, err);
        return false;
    }
    JS_FreeValue(ctx_, result);
    return true;
}

void ScriptApplication::report_error(const char* stage, const std::string& message) {
    failed_ = true;
    if (message != last_error_) {
        last_error_ = message;
        error_repeats_ = 0;
        GLOG_ERROR("[script] {} ({}): {}", entry_path_.empty() ? "<none>" : entry_path_.c_str(),
                   stage, message);
    } else if (++error_repeats_ % 100 == 1) {
        GLOG_ERROR("[script] {} ({}): {} (x{} identical)", entry_path_.c_str(), stage, message,
                   error_repeats_);
    }
}

// ---------------------------------------------------------------------------
// 热重载
// ---------------------------------------------------------------------------
void ScriptApplication::poll_hot_reload(float dt) {
    if (reload_poll_interval <= 0.0f || entry_path_.empty()) return;
    reload_accum_ += dt;
    if (reload_accum_ < reload_poll_interval) return;
    reload_accum_ = 0.0f;

    if (resolved_path_.empty()) {
        resolved_path_ = resources::ResourcePath::resolve(entry_path_);
    }

    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(resolved_path_, ec);
    if (ec) return;
    if (!have_mtime_) {
        // 首次装载失败（文件当时不存在）：文件出现即装载。
        GLOG_INFO("[script] app entry appeared: {}", resolved_path_);
        started_ = false;
        compile_and_link(nullptr);
        return;
    }

    std::error_code size_ec;
    const auto size = std::filesystem::file_size(resolved_path_, size_ec);
    const bool size_changed = !size_ec && size != source_size_;
    if (mtime == source_mtime_ && !size_changed) return;

    GLOG_INFO("[script] app entry reload: {}", resolved_path_);
    compile_and_link(nullptr);
}

} // namespace gryce_engine::script
