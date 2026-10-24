// ===========================================================================
// engine/script/script_component.h
// ScriptComponent — 挂在 Entity 上的 JavaScript 脚本组件。
//
// 模型（Unity MonoBehaviour 式）：每个组件实例独占一个 QuickJS JSContext
// （共享进程级 JSRuntime，见 js_runtime.h），模块导出函数即生命周期钩子：
//   awake()      组件挂载后（on_awake）
//   start()      场景开始、所有组件初始化后（on_start）
//   update(dt)   每帧（on_update）
//   destroy()    组件销毁前（on_destroy）
//   _input(type, a, b, c)  本帧输入事件（on_update 内、update 之前派发；
//               事件不消费，宿主在帧起点清理，见 engine_context.h）
// 脚本内 this 为每实例 self 对象，this.props 为组件序列化的 props（json）。
//
// 错误隔离：编译失败或钩子抛错 -> 记日志（含 stack）并置 failed_，后续帧
// 不再执行；热重载轮询仍在运行，修好文件即恢复（v1 语义，见 docs 排错表）。
//
// 序列化字段：script（res:/scripts/xxx.js）+ props（json 对象）。
// 运行时状态（context / 导出 / 句柄）不进序列化——快照由
// snapshot/restore_runtime_state 承担（v1 props 即全部需要保留的状态）。
// ===========================================================================

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "components/component.h"
#include "script/js_bindings.h"
#include "script/js_runtime.h"

namespace gryce_engine::scene {
class Entity;
}

namespace gryce_engine::components {

class ScriptComponent : public Component, public script::ScriptHost {
public:
    ScriptComponent() = default;
    explicit ScriptComponent(std::string script_path);

    const char* type() const override { return "Script"; }

    void serialize(nlohmann::json& out) const override;
    void deserialize(const nlohmann::json& in) override;

    // 组件生命周期（Component 基类钩子）
    void on_attach(scene::Entity* owner) override;
    void on_awake() override;
    void on_start() override;
    void on_update(float dt) override;
    void on_destroy() override;

    // ScriptHost 接口（js_bindings 经此访问宿主）
    scene::Entity* host_entity() override { return owner(); }
    float host_dt() const override { return dt_; }
    uint64_t host_frame() const override { return frame_; }
    float host_elapsed() const override { return elapsed_; }

    // 组件树访问（engine.component.* 的数据源）。
    // 下标口径与 C API 一致：宿主实体上的可见组件序列
    // （Entity::user_components()，过滤 Transform / ParentComponent /
    //  ChildrenComponent 三个内部组件）。
    int host_component_count() const override;
    Component* host_component_at(int index) const override;

    // ---- 序列化字段 ----
    std::string script_path; // res:/scripts/xxx.js（空 = 未配置）
    nlohmann::json props = nlohmann::json::object();

    // ---- 命令面（ECMD_SET_SCRIPT / ECMD_RELOAD_SCRIPTS 消费）----
    // 换脚本：改路径 + 立即重新装载（等价于组件重新挂载，props 保留）。
    void set_script_path(std::string path);
    // 强制重载当前脚本（忽略 mtime 判据）：编辑器"重新加载脚本"按钮。
    void reload_now();

    // ---- 可调参数 ----
    float reload_poll_interval = 0.5f; // 热重载轮询周期（秒）；<=0 关闭

    // ---- 运行时状态查询（调试 / 编辑器用）----
    bool running() const { return ready_; }
    bool failed() const { return failed_; }
    const std::string& last_error() const { return last_error_; }

    // 测试/调试用：当前 JSContext（未编译时为 nullptr）
    JSContext* debug_context() const { return ctx_; }

private:
    bool load_and_compile();       // 读源码 + eval module + 提取导出 + 注入 props
    void release_context();        // 释放 context / 导出 / self
    bool call_export(const char* name, JSValue* args, int argc);
    void report_error(const char* stage, const std::string& message);
    void poll_hot_reload(float dt);

    // 运行时状态
    JSContext* ctx_ = nullptr;
    JSValue module_ = JS_UNDEFINED;  // 模块命名空间（保活导出环境）
    JSValue self_obj_ = JS_UNDEFINED; // this 对象（挂 props）
    std::unordered_map<std::string, JSValue> exports_;
    bool ready_ = false;
    bool started_ = false;
    bool failed_ = false;
    std::string last_error_;
    int error_repeats_ = 0;

    // 帧时钟（ScriptHost 数据源）
    float dt_ = 0.0f;
    uint64_t frame_ = 0;
    float elapsed_ = 0.0f;

    // 源码 / 热重载（mtime + 文件大小双判据，见 poll_hot_reload 注释）
    std::string resolved_path_;
    std::filesystem::file_time_type source_mtime_{};
    std::uintmax_t source_size_ = 0;
    bool have_mtime_ = false;
    float reload_accum_ = 0.0f;
};

} // namespace gryce_engine::components
