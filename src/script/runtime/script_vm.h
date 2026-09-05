#pragma once

// ScriptVM — QuickJS 脚本引擎封装（扩展版）
//
// 提供：
// 1. Init / Eval / CallFunction / RegisterFunction
// 2. 模块作用域执行（eval_module / call_module_function）
// 3. console.log 重定向到引擎日志
// 4. 异常处理与错误信息获取
//
// 用法：
//   ScriptVM vm;
//   vm.init();
//   vm.eval("console.log('Hello World');");
//   vm.register_function("add", js_add_func);
//   vm.call_function("myFunc");
//   vm.shutdown();

#include <cstdint>
#include <string>
#include <vector>

#include <quickjs/quickjs.h>

#include "export.h"

namespace GryceEngineUtils::script {

// ---------------------------------------------------------------------------
// 脚本执行结果
// ---------------------------------------------------------------------------
struct ScriptResult {
    bool success = false;
    std::string result;      // 返回值字符串表示
    std::string error_msg;   // 错误信息（仅 success=false 时有效）
};

// ---------------------------------------------------------------------------
// JSValue 包装（便于 C++ 侧传递参数）
// ---------------------------------------------------------------------------
class JSValueWrapper {
public:
    enum class Type { Undefined, Null, Bool, Int, Float, String };

    Type type = Type::Undefined;
    bool bool_val = false;
    int32_t int_val = 0;
    double float_val = 0.0;
    std::string str_val;

    JSValueWrapper() = default;
    explicit JSValueWrapper(bool v)         : type(Type::Bool),   bool_val(v) {}
    explicit JSValueWrapper(int32_t v)      : type(Type::Int),    int_val(v) {}
    explicit JSValueWrapper(double v)       : type(Type::Float),  float_val(v) {}
    explicit JSValueWrapper(const char* v)  : type(Type::String), str_val(v ? v : "") {}
    explicit JSValueWrapper(const std::string& v) : type(Type::String), str_val(v) {}
};

// ---------------------------------------------------------------------------
// ScriptVM — QuickJS 运行时封装
// ---------------------------------------------------------------------------
class GRYCE_API ScriptVM {
public:
    ScriptVM() = default;
    ~ScriptVM();

    ScriptVM(const ScriptVM&) = delete;
    ScriptVM& operator=(const ScriptVM&) = delete;

    // 初始化 QuickJS 运行时和上下文，注册 console.log 到引擎日志
    bool init();

    // 关闭并释放所有资源
    void shutdown();

    bool initialized() const { return initialized_; }

    // 执行 JavaScript 代码（全局作用域）
    ScriptResult eval(const std::string& code, const std::string& filename = "<eval>");

    // 以模块模式执行 JavaScript 代码，返回模块命名空间对象
    // 脚本内容应使用 export function/const 导出
    ScriptResult eval_module(const std::string& code, const std::string& filename,
                             JSValue* out_module_ns);

    // 调用模块命名空间导出的函数
    ScriptResult call_module_function(JSValue module_ns, const char* func_name,
                                      const std::vector<JSValueWrapper>& args = {});

    // 获取模块导出对象（module_ns 或 exports 对象）
    JSValue get_module_exports(JSValue module_ns);

    // 调用全局 JavaScript 函数
    ScriptResult call_function(const std::string& name,
                                const std::vector<JSValueWrapper>& args = {});

    // 注册 C++ 函数到 JavaScript 全局作用域
    // func 必须是 JSCFunction* 类型
    void register_function(const std::string& name, JSCFunction* func);

    // 格式化异常信息
    std::string format_exception(JSValue exception_val);

    // 获取当前异常（如果有）并清除
    std::string consume_exception();

    // 获取 JSContext（用于扩展绑定）
    JSContext* context() const { return ctx_; }
    JSRuntime* runtime() const { return rt_; }

private:
    // 将 JSValueWrapper 转换为 JSValue
    JSValue to_js_value(JSContext* ctx, const JSValueWrapper& wrapper);

    // 将 JSValue 转换为 ScriptResult
    ScriptResult jsvalue_to_result(JSContext* ctx, JSValue val);

    JSRuntime* rt_ = nullptr;
    JSContext* ctx_ = nullptr;
    bool initialized_ = false;
};

} // namespace GryceEngineUtils::script