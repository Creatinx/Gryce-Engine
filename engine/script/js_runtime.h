// ===========================================================================
// engine/script/js_runtime.h
// QuickJS 运行时封装：进程级单例。
//
// 所有权模型：一个 JSRuntime 全局共享（堆 / 原子表 / GC），每个
// ScriptComponent 持有一个 JSContext（天然全局对象隔离，Unity
// MonoBehaviour 式心智模型）。不采用每组件独立 runtime——原子表与堆按
// runtime 分配，数百实体下内存与启动开销不可接受。
//
// 执行预算（v1 沙箱）：每次脚本入口（模块求值、钩子调用）各自计时——
// opcode 增量计数（每次中断回调计 kOpsPerTick）+ 墙钟兜底，默认 64M /
// 50ms，超预算的入口被中断（抛 INTERRUPTED）。语义是"单次入口有界"而非
// "整帧有界"：一个钩子里的死循环会在预算内中断，不会波及其他入口
// （v1 口径，见 docs/zh/scripting.md 排错表）。
// ===========================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <chrono>

extern "C" {
#include "quickjs.h"
}

namespace gryce_engine::script {

class ScriptRuntime {
public:
    static ScriptRuntime& instance();

    JSRuntime* raw() const { return rt_; }

    // 必须在创建任何 context 之前调用（运行时创建时即应用）。默认 256MB。
    void set_memory_limit(size_t bytes);

    // 每次脚本入口的执行预算：opcode 计数 + 墙钟兜底。默认 64M ops / 50ms。
    void set_execution_budget(int64_t ops, double deadline_ms);

    // 驱动 runtime 的 pending job（Promise 微任务）。返回执行的 job 数；
    // 超过 max_jobs 仍未排空返回 -1（自激 Promise 链保护）。
    int run_pending_jobs(int max_jobs = 1024);

    // 手动 GC（低负载时机调用；不引入后台线程——Emscripten 无 pthread）。
    void gc();

    JSContext* create_context();
    void destroy_context(JSContext* ctx);

    // 以 ES module 语义求值源码：成功返回模块命名空间对象（调用方持有并负责
    // JS_FreeValue），失败返回 JS_EXCEPTION（异常挂起，用 take_exception_string
    // 取消息）。filename 仅用于堆栈显示。
    JSValue eval_module(JSContext* ctx, const std::string& source, const std::string& filename);

    // 带执行预算的函数调用：JS_Call 的薄封装，调用前重置预算。
    JSValue call(JSContext* ctx, JSValueConst func, JSValueConst this_obj, int argc, JSValue* argv);

    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

private:
    ScriptRuntime();

    void arm_budget();

    static int interrupt_handler(JSRuntime* rt, void* opaque);

    JSRuntime* rt_ = nullptr;
    size_t memory_limit_ = static_cast<size_t>(256) * 1024 * 1024;
    int64_t op_budget_ = static_cast<int64_t>(64) * 1024 * 1024;
    double deadline_ms_ = 50.0;
    // 每次中断回调近似计数的 opcode 量（QuickJS 未公开回调频率，
    // 取保守估值；精确计数不影响语义，只影响预算粒度）。
    static constexpr int64_t kOpsPerTick = 4096;
    int64_t ops_used_ = 0;
    std::chrono::steady_clock::time_point deadline_{};
};

} // namespace gryce_engine::script
