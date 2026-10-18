// ===========================================================================
// engine/script/js_runtime.cpp
// QuickJS 运行时封装实现（进程级单例）。设计口径见 js_runtime.h。
// ===========================================================================

#include "script/js_runtime.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::script {

ScriptRuntime& ScriptRuntime::instance() {
    static ScriptRuntime runtime;
    return runtime;
}

ScriptRuntime::ScriptRuntime() {
    rt_ = JS_NewRuntime();
    if (!rt_) {
        GLOG_ERROR("ScriptRuntime: JS_NewRuntime failed");
        return;
    }
    JS_SetMemoryLimit(rt_, memory_limit_);
    JS_SetGCThreshold(rt_, memory_limit_ / 4);
    JS_SetMaxStackSize(rt_, static_cast<size_t>(1024) * 1024);
    JS_SetInterruptHandler(rt_, &ScriptRuntime::interrupt_handler, this);
}

ScriptRuntime::~ScriptRuntime() {
    if (rt_) {
        JS_FreeRuntime(rt_);
        rt_ = nullptr;
    }
}

void ScriptRuntime::set_memory_limit(size_t bytes) {
    memory_limit_ = bytes;
    if (rt_) JS_SetMemoryLimit(rt_, memory_limit_);
}

void ScriptRuntime::set_execution_budget(int64_t ops, double deadline_ms) {
    op_budget_ = ops;
    deadline_ms_ = deadline_ms;
}

void ScriptRuntime::arm_budget() {
    ops_used_ = 0;
    deadline_ = std::chrono::steady_clock::now() +
                std::chrono::microseconds(static_cast<int64_t>(deadline_ms_ * 1000.0));
}

int ScriptRuntime::interrupt_handler(JSRuntime* rt, void* opaque) {
    (void)rt;
    auto* self = static_cast<ScriptRuntime*>(opaque);
    if (!self) return 0;
    self->ops_used_ += kOpsPerTick;
    if (self->ops_used_ > self->op_budget_) return 1;
    if (std::chrono::steady_clock::now() > self->deadline_) return 1;
    return 0;
}

int ScriptRuntime::run_pending_jobs(int max_jobs) {
    if (!rt_) return 0;
    int executed = 0;
    JSContext* ctx = nullptr;
    while (executed < max_jobs) {
        arm_budget(); // job 也是脚本执行入口，同样受预算约束
        int ret = JS_ExecutePendingJob(rt_, &ctx);
        if (ret < 0) {
            // job 抛错：异常在 ctx 上挂起；交由调用方（ScriptComponent）记录
            return executed;
        }
        if (ret == 0) break;
        ++executed;
    }
    return executed;
}

void ScriptRuntime::gc() {
    if (rt_) JS_RunGC(rt_);
}

JSContext* ScriptRuntime::create_context() {
    if (!rt_) return nullptr;
    return JS_NewContext(rt_);
}

void ScriptRuntime::destroy_context(JSContext* ctx) {
    if (ctx) JS_FreeContext(ctx);
}

JSValue ScriptRuntime::eval_module(JSContext* ctx, const std::string& source,
                                   const std::string& filename) {
    // 分三步，不能直接用 JS_Eval 的模块模式：该路径在支持 TLA 的版本里
    // js_evaluate_module() 返回的是**执行后的 Promise**（quickjs.c:
    // js_evaluate_module -> JS_DupValue(ctx, m->promise)），不是模块记录。
    //
    // 1) 编译不执行：拿到 JS_TAG_MODULE 的模块记录（此时已完成 resolve/link），
    //    编译错误在这里就以异常形式返回。
    arm_budget();
    JSValue module = JS_Eval(ctx, source.c_str(), source.size(), filename.c_str(),
                             JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(module)) return module;

    // 2) 保留一份模块记录引用用于事后取命名空间：JS_EvalFunction 会消费传入值。
    auto* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(module));
    JSValue module_keep = JS_DupValue(ctx, module);

    // 3) 执行模块体（消耗模块记录所有权）。成功返回 Promise（TLA 语义），
    //    同步抛错则返回 JS_EXCEPTION 且异常已挂起；Promise 由每帧末的
    //    run_pending_jobs() 结算。
    arm_budget();
    JSValue result = JS_EvalFunction(ctx, module);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, module_keep);
        return JS_EXCEPTION;
    }
    JS_FreeValue(ctx, result);

    // 4) 命名空间对象：导出绑定的活视图，必须在模块求值之后构建——构建过程
    //    会解引用导出项的局部绑定（未求值时为空指针，提前调用会崩）。
    JSValue ns = JS_GetModuleNamespace(ctx, def);
    JS_FreeValue(ctx, module_keep);
    return ns;
}

JSValue ScriptRuntime::call(JSContext* ctx, JSValueConst func, JSValueConst this_obj, int argc,
                            JSValue* argv) {
    arm_budget();
    return JS_Call(ctx, func, this_obj, argc, argv);
}

} // namespace gryce_engine::script
