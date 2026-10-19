// ===========================================================================
// test/script_tests.cpp
// 脚本系统（QuickJS）单元测试：只核对行为，不依赖 GPU / 窗口 / 引擎 DLL。
//
// 被测源码直接编译进本可执行文件（engine/script/* + runtime/engine_context +
// glog + resources 路径解析），与 GryceMathTests 同构：CI 上毫秒级出结果，
// 与驱动、显卡、无头环境无关（详见根 CMakeLists 测试节注释）。
//
// 每个 suite 在 CMake 里注册成一条独立 CTest：
//   ctest 任一条失败 -> 进程返回非零 -> CI 判定不通过。
// 用 --suite <name> 可单独跑某个 suite，--list 列出全部。
// ===========================================================================

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "script/js_bindings.h"
#include "script/js_runtime.h"
#include "script/js_value.h"
#include "script/extension_host.h"
#include "script/script_app.h"
#include "script/script_component.h"
#include "resources/project.h"
#include "runtime/engine_context.h"
#include "utils/glog/glog_lib.h"

using gryce_engine::components::ScriptComponent;
using gryce_engine::script::ScriptHost;
using gryce_engine::script::ScriptRuntime;

namespace {

// ===========================================================================
// 迷你断言框架（与 math_tests.cpp 同构）
// ===========================================================================
struct Case {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)()) {
        registry().push_back(Case{suite, name, fn});
    }
};

int g_checks = 0;
int g_checks_failed = 0;
int g_case_checks = 0;
int g_case_failed = 0;

void begin_case(const std::string& suite, const std::string& name) {
    g_case_checks = 0;
    g_case_failed = 0;
}

void fail_at(const char* file, int line, const std::string& msg) {
    ++g_case_failed;
    ++g_checks_failed;
    std::printf("      FAIL %s:%d  %s\n", file, line, msg.c_str());
    std::fflush(stdout);
}

} // namespace

#define TEST(suite, name)                                                              \
    static void suite##__##name##_impl();                                              \
    static ::Registrar suite##__##name##_reg(#suite, #name, &suite##__##name##_impl);  \
    static void suite##__##name##_impl()

#define CHECK_TRUE(expr)                                                               \
    do {                                                                               \
        ++::g_checks; ++::g_case_checks;                                               \
        if (!(expr)) {                                                                 \
            ::fail_at(__FILE__, __LINE__, "expected true: " #expr);                    \
        }                                                                              \
    } while (0)

#define CHECK_FALSE(expr)                                                              \
    do {                                                                               \
        ++::g_checks; ++::g_case_checks;                                               \
        if ((expr)) {                                                                  \
            ::fail_at(__FILE__, __LINE__, "expected false: " #expr);                   \
        }                                                                              \
    } while (0)

namespace {

// ===========================================================================
// 测试辅助
// ===========================================================================

// 每个 suite 一个临时工程根（Project 单例全局，suite 间顺序执行无并发）
struct TempProject {
    std::filesystem::path root;

    TempProject(const char* suite) {
        root = std::filesystem::temp_directory_path() / "gryce_script_tests" / suite;
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root / "scripts", ec);
        gryce_engine::resources::Project::instance().set_root(root.string());
    }

    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    void write(const char* rel, const std::string& content) {
        std::ofstream f(root / rel, std::ios::binary);
        f << content;
    }
};

// 读取脚本侧 globalThis.__calls（字符串数组）
std::vector<std::string> read_calls(JSContext* ctx) {
    std::vector<std::string> out;
    if (!ctx) return out;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue calls = JS_GetPropertyStr(ctx, global, "__calls");
    JSValue len_v = JS_GetPropertyStr(ctx, calls, "length");
    uint32_t len = 0;
    gryce_engine::script::from_js(ctx, len_v, len);
    JS_FreeValue(ctx, len_v);
    for (uint32_t i = 0; i < len; ++i) {
        JSValue e = JS_GetPropertyUint32(ctx, calls, i);
        std::string s;
        gryce_engine::script::from_js(ctx, e, s);
        out.push_back(std::move(s));
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, calls);
    JS_FreeValue(ctx, global);
    return out;
}

// 值转换测试用桩宿主
struct StubHost : ScriptHost {
    gryce_engine::scene::Entity* host_entity() override { return nullptr; }
    float host_dt() const override { return 0.25f; }
    uint64_t host_frame() const override { return 7; }
    float host_elapsed() const override { return 1.5f; }
};

// ===========================================================================
// suite: value_convert — 转换层往返与边界
// ===========================================================================
TEST(value_convert, scalars_roundtrip) {
    auto& rt = ScriptRuntime::instance();
    JSContext* ctx = rt.create_context();
    CHECK_TRUE(ctx != nullptr);

    bool b = false;
    CHECK_TRUE(gryce_engine::script::from_js(ctx, gryce_engine::script::to_js(ctx, true), b));
    CHECK_TRUE(b);
    CHECK_TRUE(gryce_engine::script::from_js(ctx, gryce_engine::script::to_js(ctx, false), b));
    CHECK_FALSE(b);

    int32_t i = 0;
    CHECK_TRUE(gryce_engine::script::from_js(ctx, gryce_engine::script::to_js(ctx, int32_t{-12345}), i));
    CHECK_TRUE(i == -12345);

    uint32_t u = 0;
    CHECK_TRUE(gryce_engine::script::from_js(ctx, gryce_engine::script::to_js(ctx, uint32_t{4000000000u}), u));
    CHECK_TRUE(u == 4000000000u);

    double d = 0.0;
    CHECK_TRUE(gryce_engine::script::from_js(ctx, gryce_engine::script::to_js(ctx, 3.5), d));
    CHECK_TRUE(d == 3.5);

    std::string s;
    CHECK_TRUE(gryce_engine::script::from_js(ctx, gryce_engine::script::to_js(ctx, std::string("hello")), s));
    CHECK_TRUE(s == "hello");

    // 嵌入式 NUL 往返
    const std::string with_nul("a\0b", 3);
    CHECK_TRUE(gryce_engine::script::from_js(ctx, gryce_engine::script::to_js(ctx, with_nul), s));
    CHECK_TRUE(s.size() == 3 && s[1] == '\0');

    // undefined / null -> false（不写 out）
    JSValue und = JS_UNDEFINED;
    i = 42;
    CHECK_FALSE(gryce_engine::script::from_js(ctx, und, i));
    CHECK_TRUE(i == 42); // 未被写脏

    rt.destroy_context(ctx);
}

TEST(value_convert, float_boundaries) {
    auto& rt = ScriptRuntime::instance();
    JSContext* ctx = rt.create_context();

    const double nan_v = std::numeric_limits<double>::quiet_NaN();
    const double inf_v = std::numeric_limits<double>::infinity();
    double d = 0.0;
    CHECK_TRUE(gryce_engine::script::from_js(ctx, gryce_engine::script::to_js(ctx, nan_v), d));
    CHECK_TRUE(std::isnan(d));
    CHECK_TRUE(gryce_engine::script::from_js(ctx, gryce_engine::script::to_js(ctx, inf_v), d));
    CHECK_TRUE(std::isinf(d));

    // NaN -> int32 为 0（ECMAScript ToInt32 语义）
    int32_t i = -1;
    CHECK_TRUE(gryce_engine::script::from_js(ctx, gryce_engine::script::to_js(ctx, nan_v), i));
    CHECK_TRUE(i == 0);

    rt.destroy_context(ctx);
}

TEST(value_convert, math_types_roundtrip) {
    using gryce_engine::math::Matrix4f;
    using gryce_engine::math::Quaternionf;
    using gryce_engine::math::Vector2f;
    using gryce_engine::math::Vector3f;

    auto& rt = ScriptRuntime::instance();
    JSContext* ctx = rt.create_context();

    Vector3f v3(1.5f, -2.25f, 3.0f);
    Vector3f v3_out;
    JSValue v3_js = gryce_engine::script::to_js(ctx, v3);
    CHECK_TRUE(gryce_engine::script::from_js(ctx, v3_js, v3_out));
    JS_FreeValue(ctx, v3_js); // 数组是堆对象：临时值必须显式释放
    CHECK_TRUE(v3_out.x == 1.5f && v3_out.y == -2.25f && v3_out.z == 3.0f);

    Quaternionf q(0.0f, 0.7071068f, 0.0f, 0.7071068f);
    Quaternionf q_out;
    JSValue q_js = gryce_engine::script::to_js(ctx, q);
    CHECK_TRUE(gryce_engine::script::from_js(ctx, q_js, q_out));
    JS_FreeValue(ctx, q_js);
    CHECK_TRUE(q_out.y == q.y && q_out.w == q.w);

    // 矩阵：列主序内存序往返（identity 不变）
    Matrix4f m_out;
    JSValue m_js = gryce_engine::script::to_js(ctx, Matrix4f::identity());
    CHECK_TRUE(gryce_engine::script::from_js(ctx, m_js, m_out));
    JS_FreeValue(ctx, m_js);
    bool same = true;
    for (int i = 0; i < 16; ++i) same = same && (m_out.m[i] == Matrix4f::identity().m[i]);
    CHECK_TRUE(same);

    // 短数组 -> false（Vector3 需要 3 个元素）
    JSValue short_arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, short_arr, 0, gryce_engine::script::to_js(ctx, 1.0));
    JS_SetPropertyUint32(ctx, short_arr, 1, gryce_engine::script::to_js(ctx, 2.0));
    CHECK_FALSE(gryce_engine::script::from_js(ctx, short_arr, v3_out));
    JS_FreeValue(ctx, short_arr);

    rt.destroy_context(ctx);
}

TEST(value_convert, json_roundtrip) {
    auto& rt = ScriptRuntime::instance();
    JSContext* ctx = rt.create_context();

    nlohmann::json j = {
        {"name", "demo"},
        {"speed", 2.5},
        {"count", 3},
        {"enabled", true},
        {"nothing", nullptr},
        {"tags", nlohmann::json::array({"a", "b"})},
        {"nested", {{"x", 1.0}}},
    };
    JSValue v = gryce_engine::script::to_js(ctx, j);
    nlohmann::json back = gryce_engine::script::from_js(ctx, v);
    JS_FreeValue(ctx, v);
    CHECK_TRUE(back == j);

    rt.destroy_context(ctx);
}

TEST(value_convert, bindings_smoke) {
    // 绑定层端到端：engine.time 走 ScriptHost 桩；engine.entity 无宿主时抛错；
    // engine.log 走 glog（不记入 __calls）。
    auto& rt = ScriptRuntime::instance();
    JSContext* ctx = rt.create_context();
    StubHost host;
    gryce_engine::script::install_engine_bindings(ctx, &host);

    const char* src =
        "globalThis.__calls = [];\n"
        "export function run() {\n"
        "  globalThis.__calls.push('dt=' + engine.time.dt());\n"
        "  globalThis.__calls.push('frame=' + engine.time.frame());\n"
        "  globalThis.__calls.push('elapsed=' + engine.time.elapsed());\n"
        "  engine.log.info('from-script');\n"
        "  console.log('from-console', 42);\n"
        "  try { engine.entity.position(); globalThis.__calls.push('no-throw'); }\n"
        "  catch (e) { globalThis.__calls.push('entity-throws'); }\n"
        "}\n";
    JSValue mod = rt.eval_module(ctx, src, "<bindings>");
    CHECK_FALSE(JS_IsException(mod));
    JSValue run = JS_GetPropertyStr(ctx, mod, "run");
    JSValue result = JS_Call(ctx, run, JS_UNDEFINED, 0, nullptr);
    CHECK_FALSE(JS_IsException(result));
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, run);
    JS_FreeValue(ctx, mod);

    const auto calls = read_calls(ctx);
    CHECK_TRUE(calls.size() == 4);
    if (calls.size() == 4) {
        CHECK_TRUE(calls[0] == "dt=0.25");
        CHECK_TRUE(calls[1] == "frame=7");
        CHECK_TRUE(calls[2] == "elapsed=1.5");
        CHECK_TRUE(calls[3] == "entity-throws");
    }

    rt.destroy_context(ctx);
}

TEST(value_convert, module_exports_visible) {
    // 回归守卫：模块求值路径必须回到「模块命名空间对象」。曾经直接取
    // JS_Eval(JS_EVAL_TYPE_MODULE) 的返回值，而它在支持 TLA 的版本里是执行
    // 后的 Promise（quickjs.c: js_evaluate_module），导致导出全部取不到。
    auto& rt = ScriptRuntime::instance();
    JSContext* ctx = rt.create_context();
    const char* src = "export const answer = 42;\nexport function fn() { return 1; }\n";
    JSValue ns = rt.eval_module(ctx, src, "<exports>");
    CHECK_FALSE(JS_IsException(ns));
    CHECK_TRUE(JS_IsObject(ns)); // 命名空间对象，不是 Promise / 模块记录

    JSValue answer = JS_GetPropertyStr(ctx, ns, "answer");
    JSValue fn = JS_GetPropertyStr(ctx, ns, "fn");
    int32_t v = 0;
    CHECK_TRUE(gryce_engine::script::from_js(ctx, answer, v));
    CHECK_TRUE(v == 42);
    CHECK_TRUE(JS_IsFunction(ctx, fn));

    JS_FreeValue(ctx, answer);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, ns);
    rt.destroy_context(ctx);
}

TEST(lifecycle, hooks_order_and_props) {
    TempProject tmp("lifecycle");
    tmp.write("scripts/lifecycle.js",
              "globalThis.__calls = [];\n"
              "export function awake() { globalThis.__calls.push('awake'); }\n"
              "export function start() { globalThis.__calls.push('start:speed=' + this.props.speed); }\n"
              "export function update(dt) { globalThis.__calls.push('f' + engine.time.frame() + ':dt' + dt); }\n"
              "export function destroy() { globalThis.__calls.push('destroy'); }\n");

    auto comp = std::make_unique<ScriptComponent>("res:/scripts/lifecycle.js");
    comp->props = nlohmann::json{{"speed", 2.5}};
    comp->on_attach(nullptr);
    comp->on_awake();
    CHECK_TRUE(comp->running());
    CHECK_FALSE(comp->failed());

    comp->on_start();
    comp->on_update(0.5f);
    comp->on_update(0.25f);
    comp->on_update(0.25f);

    const auto calls = read_calls(comp->debug_context());
    CHECK_TRUE(calls.size() == 5);
    if (calls.size() == 5) {
        CHECK_TRUE(calls[0] == "awake");
        CHECK_TRUE(calls[1] == "start:speed=2.5"); // props 已注入 this.props
        CHECK_TRUE(calls[2] == "f1:dt0.5");        // frame 从 1 起算
        CHECK_TRUE(calls[3] == "f2:dt0.25");
        CHECK_TRUE(calls[4] == "f3:dt0.25");
    }

    comp->on_destroy();
    CHECK_FALSE(comp->running());
    // destroy 钩子的调用验证见 destroy_hook_invoked（on_destroy 会释放 context，
    // 之后无法再读脚本侧状态，只能观察组件的 failed/last_error）。
    CHECK_TRUE(comp->debug_context() == nullptr);
}

TEST(lifecycle, destroy_hook_invoked) {
    TempProject tmp("destroy");
    tmp.write("scripts/destroy.js",
              "export function destroy() { throw new Error('bye'); }\n");

    auto comp = std::make_unique<ScriptComponent>("res:/scripts/destroy.js");
    comp->on_attach(nullptr);
    comp->on_awake();
    CHECK_TRUE(comp->running());
    CHECK_FALSE(comp->failed());

    comp->on_destroy(); // destroy 钩子抛错 -> failed + last_error 记录
    CHECK_TRUE(comp->failed());
    CHECK_TRUE(comp->last_error().find("bye") != std::string::npos);
    CHECK_TRUE(comp->debug_context() == nullptr);
}

TEST(lifecycle, disabled_and_empty_path) {
    // 未配置路径：全程静默 no-op，不崩
    auto comp = std::make_unique<ScriptComponent>();
    comp->on_attach(nullptr);
    comp->on_awake();
    CHECK_FALSE(comp->running());
    comp->on_start();
    comp->on_update(0.1f);
    comp->on_destroy();
    CHECK_TRUE(true);
}

TEST(lifecycle, input_events_dispatched) {
    TempProject tmp("input");
    tmp.write("scripts/input.js",
              "globalThis.__calls = [];\n"
              "export function _input(type, a, b, c) { globalThis.__calls.push(type + ':' + a + ':' + b + ':' + c); }\n"
              "export function update(dt) { globalThis.__calls.push('update'); }\n");

    auto& st = gryce_core::g_input_state;
    st.input_events.clear();
    st.input_events.push_back({3, 11, 22, 0}); // MOUSE_MOVE
    st.input_events.push_back({1, 65, 0, 0});  // KEY_DOWN 'A'

    auto comp = std::make_unique<ScriptComponent>("res:/scripts/input.js");
    comp->on_attach(nullptr);
    comp->on_awake();
    comp->on_update(0.1f);

    const auto calls = read_calls(comp->debug_context());
    CHECK_TRUE(calls.size() == 3);
    if (calls.size() == 3) {
        CHECK_TRUE(calls[0] == "3:11:22:0"); // _input 先于 update
        CHECK_TRUE(calls[1] == "1:65:0:0");
        CHECK_TRUE(calls[2] == "update");
    }

    // 事件不消费：第二帧仍在（宿主帧起点才清理）。帧 1 派发 2 个事件 + update，
    // 帧 2 派发 3 个事件（2 旧 + 1 新）+ update。
    st.input_events.push_back({1, 66, 0, 0});
    comp->on_update(0.1f);
    const auto calls2 = read_calls(comp->debug_context());
    CHECK_TRUE(calls2.size() == 7);

    st.input_events.clear();
    comp->on_destroy();
}

// ===========================================================================
// suite: error_isolation — 编译/运行期错误不崩引擎，failed 后停执行
// ===========================================================================
TEST(error_isolation, syntax_error) {
    TempProject tmp("syntax");
    tmp.write("scripts/bad.js", "export function update(dt) { \n"); // 语法错误

    auto comp = std::make_unique<ScriptComponent>("res:/scripts/bad.js");
    comp->on_attach(nullptr);
    comp->on_awake();
    CHECK_FALSE(comp->running());
    CHECK_TRUE(comp->failed());
    CHECK_FALSE(comp->last_error().empty());

    comp->on_update(0.1f); // 不得崩溃
    CHECK_TRUE(comp->failed());
    comp->on_destroy();
}

TEST(error_isolation, runtime_throw_stops_execution) {
    TempProject tmp("throw");
    tmp.write("scripts/throw.js",
              "globalThis.__calls = [];\n"
              "export function update(dt) { globalThis.__calls.push('update'); throw new Error('boom'); }\n");

    auto comp = std::make_unique<ScriptComponent>("res:/scripts/throw.js");
    comp->on_attach(nullptr);
    comp->on_awake();
    CHECK_TRUE(comp->running());

    comp->on_update(0.1f);
    CHECK_TRUE(comp->failed());
    const auto calls = read_calls(comp->debug_context());
    CHECK_TRUE(calls.size() == 1); // 抛错那次记了一次，之后不再执行

    comp->on_update(0.1f);
    comp->on_update(0.1f);
    CHECK_TRUE(comp->failed());

    comp->on_destroy();
}

TEST(error_isolation, infinite_loop_interrupted) {
    TempProject tmp("loop");
    tmp.write("scripts/loop.js", "export function update(dt) { while (true) {} }\n");

    auto& rt = ScriptRuntime::instance();
    rt.set_execution_budget(8192, 10000.0); // 仅靠 opcode 预算中断（确定性）

    auto comp = std::make_unique<ScriptComponent>("res:/scripts/loop.js");
    comp->on_attach(nullptr);
    comp->on_awake();
    CHECK_TRUE(comp->running());

    comp->on_update(0.1f); // 死循环被中断 -> 异常 -> failed
    CHECK_TRUE(comp->failed());

    comp->on_update(0.1f); // 已 failed：不再进入死循环
    CHECK_TRUE(comp->failed());

    comp->on_destroy();
    rt.set_execution_budget(64 * 1024 * 1024, 50.0); // 恢复默认
}

// ===========================================================================
// suite: hot_reload — 文件变更触发重编译，props 保留
// ===========================================================================
TEST(hot_reload, file_change_recompiles) {
    TempProject tmp("reload");
    tmp.write("scripts/spin.js",
              "globalThis.__calls = [];\n"
              "export function update(dt) { globalThis.__calls.push('v1'); }\n");

    auto comp = std::make_unique<ScriptComponent>("res:/scripts/spin.js");
    comp->props = nlohmann::json{{"speed", 3.0}};
    comp->on_attach(nullptr);
    comp->on_awake();
    comp->on_start();
    comp->on_update(0.6f); // 越过轮询周期：记录基准 mtime，无变化不重载
    {
        const auto calls = read_calls(comp->debug_context());
        CHECK_TRUE(calls.size() == 1);
        if (!calls.empty()) CHECK_TRUE(calls[0] == "v1");
    }

    tmp.write("scripts/spin.js",
              "globalThis.__calls = [];\n"
              "export function start() { globalThis.__calls.push('v2:start'); }\n"
              "export function update(dt) { globalThis.__calls.push('v2:speed=' + this.props.speed); }\n");

    comp->on_update(0.6f); // 检出 mtime 变化 -> 重编译 -> started_ 为真补 start
    const auto calls = read_calls(comp->debug_context());
    CHECK_TRUE(calls.size() == 2);
    if (calls.size() == 2) {
        CHECK_TRUE(calls[0] == "v2:start");
        CHECK_TRUE(calls[1] == "v2:speed=3"); // props 跨重载保留
    }
    CHECK_TRUE(comp->running());
    CHECK_FALSE(comp->failed());

    comp->on_destroy();
}

TEST(hot_reload, missing_file_then_appears) {
    TempProject tmp("appear");
    // 不写文件：on_awake 装载失败但不崩，轮询重试后文件出现即恢复
    auto comp = std::make_unique<ScriptComponent>("res:/scripts/late.js");
    comp->on_attach(nullptr);
    comp->on_awake();
    CHECK_FALSE(comp->running());
    CHECK_TRUE(comp->failed());

    comp->on_update(0.6f);
    CHECK_FALSE(comp->running());

    tmp.write("scripts/late.js",
              "globalThis.__calls = [];\n"
              "export function update(dt) { globalThis.__calls.push('late'); }\n");
    comp->on_update(0.6f); // 文件出现：装载成功（started_ 为假，不补 start）
    CHECK_TRUE(comp->running());
    CHECK_FALSE(comp->failed());
    {
        const auto calls = read_calls(comp->debug_context());
        // 注意：本帧 update 在 poll 之后执行
        CHECK_TRUE(calls.size() == 1);
        if (!calls.empty()) CHECK_TRUE(calls[0] == "late");
    }

    comp->on_destroy();
}

TEST(hot_reload, manual_reload_ignores_mtime) {
    // ECMD_RELOAD_SCRIPTS 的内核：强制重载不看 mtime / 大小，立即生效。
    TempProject tmp("manual");
    tmp.write("scripts/m.js",
              "globalThis.__calls = [];\n"
              "export function update(dt) { globalThis.__calls.push('a'); }\n");

    auto comp = std::make_unique<ScriptComponent>("res:/scripts/m.js");
    comp->on_attach(nullptr);
    comp->on_awake();
    comp->on_update(0.1f);

    // 同长度改写：mtime / size 判据都可能不变，只有强制重载能发现
    tmp.write("scripts/m.js",
              "globalThis.__calls = [];\n"
              "export function update(dt) { globalThis.__calls.push('b'); }\n");
    comp->reload_now();
    CHECK_TRUE(comp->running());
    comp->on_update(0.1f);

    const auto calls = read_calls(comp->debug_context());
    CHECK_TRUE(calls.size() == 1);
    if (calls.size() == 1) CHECK_TRUE(calls[0] == "b");

    comp->on_destroy();
}

TEST(hot_reload, set_script_path_switches) {
    // ECMD_SET_SCRIPT 的内核：换脚本立即装载，props 保留。
    TempProject tmp("switch");
    tmp.write("scripts/a.js",
              "globalThis.__calls = [];\n"
              "export function update(dt) { globalThis.__calls.push('a'); }\n");
    tmp.write("scripts/b.js",
              "globalThis.__calls = [];\n"
              "export function update(dt) { globalThis.__calls.push('b:' + this.props.speed); }\n");

    auto comp = std::make_unique<ScriptComponent>("res:/scripts/a.js");
    comp->props = nlohmann::json{{"speed", 4.0}};
    comp->on_attach(nullptr);
    comp->on_awake();
    comp->on_update(0.1f);

    comp->set_script_path("res:/scripts/b.js");
    CHECK_TRUE(comp->running());
    CHECK_FALSE(comp->failed());
    comp->on_update(0.1f);

    const auto calls = read_calls(comp->debug_context());
    CHECK_TRUE(calls.size() == 1);
    if (calls.size() == 1) CHECK_TRUE(calls[0] == "b:4");

    comp->on_destroy();
}

// ===========================================================================
// suite: app_entry — 入口脚本（main.js）与原生扩展 ABI
// ===========================================================================
int read_global_int(JSContext* ctx, const char* name) {
    if (!ctx) return -1;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue v = JS_GetPropertyStr(ctx, global, name);
    int32_t out = -1;
    gryce_engine::script::from_js(ctx, v, out);
    JS_FreeValue(ctx, v);
    JS_FreeValue(ctx, global);
    return out;
}

TEST(app_entry, lifecycle_hooks) {
    TempProject tmp("app_entry_lifecycle");
    tmp.write("main.js",
              "globalThis.__calls = [];\n"
              "export function on_start() { __calls.push('start'); }\n"
              "export function on_update(dt) { __calls.push('update:' + dt.toFixed(2)); }\n"
              "export function on_destroy() { __calls.push('destroy'); }\n");

    gryce_engine::script::ScriptApplication app;
    app.reload_poll_interval = 0.0f;   // 本用例不测热重载
    std::string err;
    CHECK_TRUE(app.load("res:/main.js", &err));
    CHECK_TRUE(app.loaded());
    CHECK_TRUE(app.context() != nullptr);

    JSContext* ctx = app.context();
    app.start();
    auto calls = read_calls(ctx);
    CHECK_TRUE(calls.size() == 1);
    if (calls.size() == 1) CHECK_TRUE(calls[0] == "start");

    app.tick(0.5f);
    calls = read_calls(ctx);
    CHECK_TRUE(calls.size() == 2);
    if (calls.size() == 2) CHECK_TRUE(calls[1] == "update:0.50");

    app.shutdown();
    CHECK_TRUE(!app.loaded());
    CHECK_TRUE(app.context() == nullptr);
}

TEST(app_entry, hot_reload_swaps_module) {
    TempProject tmp("app_entry_reload");
    tmp.write("main.js",
              "globalThis.__v = 1;\n"
              "export function on_update() { globalThis.__v = 1; }\n");

    gryce_engine::script::ScriptApplication app;
    app.reload_poll_interval = 0.01f;   // 轮询周期压到最短，两帧内即可命中
    std::string err;
    CHECK_TRUE(app.load("res:/main.js", &err));
    app.start();
    app.tick(0.02f);
    CHECK_TRUE(read_global_int(app.context(), "__v") == 1);

    // 内容长度不同：即使文件系统写入时间粒度较大，大小判据也能命中
    //（同长度 + 同一 mtime tick 的改动无法被双判据发现，见 docs 排错表）。
    tmp.write("main.js",
              "// v2\n"
              "globalThis.__v = 2;\n"
              "export function on_update() { globalThis.__v = 2; }\n");
    app.tick(0.02f);
    CHECK_TRUE(app.loaded());
    CHECK_TRUE(read_global_int(app.context(), "__v") == 2);

    app.tick(0.02f);
    CHECK_TRUE(read_global_int(app.context(), "__v") == 2);
    app.shutdown();
}

namespace {
GryceExtensionApi* g_test_ext_api = nullptr;

GJsValue test_ext_ping(void*, GJsValue, int, const GJsValue*) {
    return g_test_ext_api->ret_number(g_test_ext_api->js_ctx, 42.0);
}

GJsValue test_ext_greet(void*, GJsValue, int argc, const GJsValue* argv) {
    char name[32] = {};
    if (argc < 1 || g_test_ext_api->arg_string(g_test_ext_api->js_ctx, argv[0], name,
                                               static_cast<int>(sizeof(name))) < 0) {
        return g_test_ext_api->throw_error(g_test_ext_api->js_ctx, "greet(name)");
    }
    const std::string msg = std::string("hello ") + name;
    return g_test_ext_api->ret_string(g_test_ext_api->js_ctx, msg.c_str());
}
} // namespace

TEST(app_entry, extension_add_function) {
    auto& rt = ScriptRuntime::instance();
    JSContext* ctx = rt.create_context();
    CHECK_TRUE(ctx != nullptr);
    if (!ctx) return;

    StubHost host;
    gryce_engine::script::install_engine_bindings(ctx, &host);

    GryceExtensionApi api = gryce_engine::script::make_extension_api(ctx);
    CHECK_TRUE(api.version == GRYCE_EXTENSION_VERSION);
    g_test_ext_api = &api;
    api.add_function(api.js_ctx, "game", "ping", &test_ext_ping, 0);
    api.add_function(api.js_ctx, "game", "greet", &test_ext_greet, 1);

    JSValue mod = rt.eval_module(ctx,
                                 "export const p = engine.game.ping();\n"
                                 "export const g = engine.game.greet('gryce');\n",
                                 "extension_test");
    CHECK_FALSE(JS_IsException(mod));

    JSValue p = JS_GetPropertyStr(ctx, mod, "p");
    double pv = 0.0;
    CHECK_TRUE(gryce_engine::script::from_js(ctx, p, pv));
    CHECK_TRUE(pv == 42.0);
    JS_FreeValue(ctx, p);

    JSValue g = JS_GetPropertyStr(ctx, mod, "g");
    std::string gs;
    CHECK_TRUE(gryce_engine::script::from_js(ctx, g, gs));
    CHECK_TRUE(gs == "hello gryce");
    JS_FreeValue(ctx, g);

    JS_FreeValue(ctx, mod);
    g_test_ext_api = nullptr;
    rt.destroy_context(ctx);
}

} // namespace

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    std::vector<std::string> wanted;
    std::vector<std::string> wanted_cases;
    bool list_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--suite") == 0 && i + 1 < argc) {
            wanted.emplace_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) {
            wanted_cases.emplace_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--list") == 0) {
            list_only = true;
        } else {
            std::printf("usage: %s [--suite <name>]... [--case <name>]... [--list]\n", argv[0]);
            return 2;
        }
    }

    // 稳定的 suite 顺序（按注册顺序去重）
    std::vector<std::string> suites;
    for (const Case& c : registry()) {
        bool dup = false;
        for (const std::string& s : suites) {
            if (s == c.suite) { dup = true; break; }
        }
        if (!dup) suites.push_back(c.suite);
    }

    if (list_only) {
        for (const std::string& s : suites) {
            std::size_t n = 0;
            for (const Case& c : registry()) {
                if (c.suite == s) ++n;
            }
            std::printf("%-16s %zu cases\n", s.c_str(), n);
        }
        return 0;
    }

    int cases_total = 0;
    int cases_failed = 0;
    for (const std::string& s : suites) {
        if (!wanted.empty()) {
            bool sel = false;
            for (const std::string& w : wanted) {
                if (w == s) { sel = true; break; }
            }
            if (!sel) continue;
        }
        std::printf("[ SUITE ] %s\n", s.c_str());
        for (const Case& c : registry()) {
            if (c.suite != s) continue;
            if (!wanted_cases.empty()) {
                bool sel = false;
                for (const std::string& w : wanted_cases) {
                    if (w == c.name) { sel = true; break; }
                }
                if (!sel) continue;
            }
            begin_case(c.suite, c.name);
            c.fn();
            ++cases_total;
            if (g_case_failed > 0) {
                ++cases_failed;
                std::printf("  [FAIL] %s (%d/%d checks failed)\n",
                            c.name.c_str(), g_case_failed, g_case_checks);
            } else {
                std::printf("  [ OK  ] %s (%d checks)\n", c.name.c_str(), g_case_checks);
            }
        }
    }

    const int failed_checks = g_checks_failed;
    std::printf("\n--------------------------------------------------------------\n");
    std::printf("checks: %d total, %d passed, %d failed\n",
                g_checks, g_checks - failed_checks, failed_checks);
    std::printf("cases : %d total, %d passed, %d failed\n",
                cases_total, cases_total - cases_failed, cases_failed);
    if (cases_failed == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL\n");
    return 1;
}
