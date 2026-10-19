// ===========================================================================
// engine/script/script_app.h
// ScriptApplication — 进程级入口脚本（main.js）。
//
// 与 ScriptComponent 的关系：同构但不同层。ScriptComponent 挂在实体上、随
// 场景生命周期走；ScriptApplication 属于宿主本身，一个进程一个，负责
// "应用入口"——建窗口、载场景、驱动主循环里的每帧逻辑。
//
// 钩子（模块导出即钩子，均可省略）：
//   on_start()        入口脚本装载成功后一次；热重载成功后重放
//   on_update(dt)     每帧（宿主在 GCore_BeginFrame 之后、渲染之前调用，
//                     因此 engine.input.* 读到的是本帧输入）
//   on_destroy()      宿主退出前一次
//
// 错误隔离：编译失败或任一钩子抛错 -> 记日志（name: message + stack）、
// 置 failed_ 并停止派发；热重载轮询继续跑，文件修好即恢复。
// 执行预算沿用 ScriptRuntime（每次入口独立计时，见 js_runtime.h）。
// ===========================================================================

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>

extern "C" {
#include "quickjs.h"
}

#include "export.h"
#include "script/js_bindings.h"

namespace gryce_engine::script {

class GRYCE_API ScriptApplication : public ScriptHost {
public:
    // 每次（重新）创建 JSContext 后的回调：宿主在这里安装自己那层的绑定
    // （窗口 / 场景 / 渲染等）。绑定时长与 context 一致，重载后重新安装。
    using ContextHook = std::function<void(JSContext*)>;

    ScriptApplication() = default;
    ~ScriptApplication() override;

    ScriptApplication(const ScriptApplication&) = delete;
    ScriptApplication& operator=(const ScriptApplication&) = delete;

    // 读取并求值入口模块（res:/ 路径或绝对路径）。失败返回 false，错误写入
    // error（同时记日志）；成功后模块处于"已链接未启动"状态。
    bool load(const std::string& path, std::string* error = nullptr);

    // 派发 on_start（尚未 load 时为空操作）。
    void start();

    // 每帧：先轮询热重载，再派发 on_update(dt)，最后排空 Promise 微任务。
    void tick(float dt);

    // 派发 on_destroy 并释放 context（可重复调用）。
    void shutdown();

    // 忽略 mtime 判据强制重载（调试用）。
    bool reload_now();

    bool loaded() const { return ready_; }
    bool failed() const { return failed_; }
    const std::string& last_error() const { return last_error_; }
    const std::string& path() const { return entry_path_; }
    const std::string& resolved_path() const { return resolved_path_; }
    JSContext* context() const { return ctx_; }

    // 热重载轮询周期（秒）；<=0 关闭轮询。
    float reload_poll_interval = 0.5f;
    ContextHook on_context_created;

    // ---- ScriptHost（入口脚本没有宿主实体）----
    scene::Entity* host_entity() override { return nullptr; }
    float host_dt() const override { return dt_; }
    uint64_t host_frame() const override { return frame_; }
    float host_elapsed() const override { return elapsed_; }

private:
    bool compile_and_link(std::string* error);
    bool call_hook(const char* name, JSValue* args, int argc);
    void report_error(const char* stage, const std::string& message);
    void poll_hot_reload(float dt);
    void release();

    // 运行时状态
    JSContext* ctx_ = nullptr;
    JSValue module_ = JS_UNDEFINED;
    JSValue self_obj_ = JS_UNDEFINED;
    std::unordered_map<std::string, JSValue> hooks_;
    bool ready_ = false;
    bool started_ = false;
    bool failed_ = false;
    std::string last_error_;
    int error_repeats_ = 0;

    // 时钟（ScriptHost 数据源）
    float dt_ = 0.0f;
    uint64_t frame_ = 0;
    float elapsed_ = 0.0f;

    // 源码 / 热重载
    std::string entry_path_;
    std::string resolved_path_;
    std::filesystem::file_time_type source_mtime_{};
    std::uintmax_t source_size_ = 0;
    bool have_mtime_ = false;
    float reload_accum_ = 0.0f;
};

} // namespace gryce_engine::script
