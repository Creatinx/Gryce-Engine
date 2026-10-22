// ===========================================================================
// test/ui_tests.cpp
// UI 操作队列的单元测试（OpQueue 本体 + GUi_* C ABI 两层）。
//
// 与 math / script / asset 测试同构：被测源码（api/ui_op_queue.cpp +
// api/ui_api.cpp）直接编译进本可执行文件，不链接 GryceRenderer.dll，
// 不碰 GPU / 窗口 —— CI 上毫秒级完成。
//
// ui_api.cpp 在**不定义 GRYCE_HAS_IMGUI** 的情况下编译：这同时验证了
// "关掉 ImGui 的构建里队列照样能用"（无窗口宿主也走这条路）。
// GRYCE_RENDERER_BUILDING 让导出宏变成 dllexport，静态 exe 内自链接。
//
// 每个 suite 在 CMake 里注册成一条独立 CTest：
//   ctest 任一条失败 -> 进程返回非零 -> CI 判定不通过。
// 用 --suite <name> 可单独跑某个 suite，--list 列出全部。
// ===========================================================================

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "api/ui_api.h"
#include "api/ui_op_queue.h"
#include "api/ui_op_queue_internal.h"

using gryce_ui::Op;
using gryce_ui::OpQueue;

namespace {

// ===========================================================================
// 迷你断言框架（无第三方依赖，与 math_tests.cpp 同风格）
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

void fail_at(const char* file, int line, const std::string& msg) {
    ++g_case_failed;
    ++g_checks_failed;
    std::printf("      FAIL %s:%d  %s\n", file, line, msg.c_str());
    std::fflush(stdout);
}

} // namespace

#define TEST(suite, name)                                                            \
    static void suite##__##name##_impl();                                            \
    static ::Registrar suite##__##name##_reg(#suite, #name, &suite##__##name##_impl); \
    static void suite##__##name##_impl()

#define CHECK_TRUE(expr)                                                             \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        if (!(expr)) ::fail_at(__FILE__, __LINE__, "expected true: " #expr);          \
    } while (0)

#define CHECK_FALSE(expr)                                                            \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        if (expr) ::fail_at(__FILE__, __LINE__, "expected false: " #expr);            \
    } while (0)

#define CHECK_EQ(actual, expected)                                                   \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        const long long a_ = static_cast<long long>(actual);                          \
        const long long b_ = static_cast<long long>(expected);                        \
        if (a_ != b_) ::fail_at(__FILE__, __LINE__,                                   \
                               std::string(#actual) + " = " + std::to_string(a_) +    \
                               ", expected " + std::to_string(b_));                   \
    } while (0)

#define CHECK_NEAR(actual, expected, eps)                                            \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        const double a_ = static_cast<double>(actual);                                \
        const double b_ = static_cast<double>(expected);                              \
        if (!(std::fabs(a_ - b_) <= static_cast<double>(eps))) {                      \
            char buf_[256];                                                          \
            std::snprintf(buf_, sizeof(buf_), "%s => %.8g, expected %.8g", #actual,   \
                          a_, b_);                                                   \
            ::fail_at(__FILE__, __LINE__, buf_);                                     \
        }                                                                            \
    } while (0)

// ===========================================================================
// 辅助
// ===========================================================================
namespace {

Op make_op(uint32_t type, uint32_t key, int64_t arg, float v0 = 0.0f) {
    Op o;
    o.type = type;
    o.key = key;
    o.arg = arg;
    o.vec[0] = v0;
    return o;
}

// C ABI 侧的统计快照 + 差值：ABI 作用在进程内唯一队列上，统计是累计值，
// 每条用例只能断言"这条用例造成的增量"。
struct StatsSnap {
    int pending;
    uint64_t pushed, coalesced, drained, evicted, rejected, throttled;
};

StatsSnap snap_stats() {
    StatsSnap s{};
    GUi_GetOpQueueStats(&s.pending, &s.pushed, &s.coalesced, &s.drained,
                        &s.evicted, &s.rejected, &s.throttled);
    return s;
}

// 每条 ABI 用例开头调：清空待处理、策略复位到"不限制 / 每帧可排"。
// 帧间隔状态跨用例保留（进程内唯一队列，没有公开的重置入口）——推进若干帧
// 让残留的间隔限制失效（本文件用例用到的最小间隔是 3，8 帧足够）。
void abi_reset() {
    GUi_ClearOps();
    GUi_SetOpPolicy(0, 0);
    for (int i = 0; i < 8; ++i) GUi_AdvanceOpFrame();
}

GUiOp c_op(uint32_t type, uint32_t key, int64_t arg) {
    GUiOp o;
    std::memset(&o, 0, sizeof(o));
    o.type = type;
    o.key = key;
    o.arg = arg;
    return o;
}

} // namespace

// ===========================================================================
// suite: queue —— OpQueue 本体
// ===========================================================================

TEST(queue, fifo_order) {
    OpQueue q;
    CHECK_TRUE(q.push(make_op(1, 0, 10)));
    CHECK_TRUE(q.push(make_op(2, 0, 20)));
    CHECK_TRUE(q.push(make_op(3, 0, 30)));
    CHECK_EQ(q.pending(), 3);

    Op out[4];
    CHECK_EQ(q.drain(out, 4), 3);
    CHECK_EQ(out[0].arg, 10);
    CHECK_EQ(out[1].arg, 20);
    CHECK_EQ(out[2].arg, 30);
    CHECK_EQ(q.pending(), 0);
    // seq 按入队先后递增，与 FIFO 顺序一致
    CHECK_EQ(out[0].seq, 1);
    CHECK_EQ(out[2].seq, 3);
}

TEST(queue, coalesce_keeps_position_and_seq) {
    OpQueue q;
    CHECK_TRUE(q.push(make_op(1, 0, 10)));   // 离散，占住队头
    CHECK_TRUE(q.push(make_op(2, 7, 20)));   // 可合并，第二
    CHECK_TRUE(q.push(make_op(2, 7, 21)));   // 合并进上一条
    CHECK_TRUE(q.push(make_op(2, 7, 22)));   // 再合并
    CHECK_EQ(q.pending(), 2);

    Op out[4];
    CHECK_EQ(q.drain(out, 4), 2);
    CHECK_EQ(out[0].arg, 10);
    CHECK_EQ(out[1].arg, 22);                 // 只剩最新值
    CHECK_EQ(out[1].seq, 2);                  // seq 保持首次入队的值
    const OpQueue::Stats s = q.stats();
    CHECK_EQ(s.pushed, 4);
    CHECK_EQ(s.coalesced, 2);
    CHECK_EQ(s.drained, 2);
}

TEST(queue, coalesce_matches_type_too) {
    OpQueue q;
    // 同 key 不同 type 是两条不同的操作，不合并
    CHECK_TRUE(q.push(make_op(1, 7, 1)));
    CHECK_TRUE(q.push(make_op(2, 7, 2)));
    CHECK_EQ(q.pending(), 2);
    Op out[4];
    CHECK_EQ(q.drain(out, 4), 2);
    CHECK_EQ(out[0].type, 1u);
    CHECK_EQ(out[1].type, 2u);
}

TEST(queue, discrete_ops_never_merge) {
    OpQueue q;
    for (int i = 0; i < 8; ++i) CHECK_TRUE(q.push(make_op(9, 0, i)));
    CHECK_EQ(q.pending(), 8);
}

TEST(queue, evict_oldest_coalescing_when_full) {
    OpQueue q;
    q.set_policy(OpQueue::Policy{0, 0});   // 本用例要一次排空，关掉预算与间隔
    // 先填一条可合并项，再用离散项把队列填满
    CHECK_TRUE(q.push(make_op(1, 5, 100)));
    for (int i = 0; i < OpQueue::kCapacity - 1; ++i) {
        CHECK_TRUE(q.push(make_op(2, 0, i)));
    }
    CHECK_EQ(q.pending(), OpQueue::kCapacity);

    // 再推可合并项：应逐出最旧的那条（key=5），新项入队
    CHECK_TRUE(q.push(make_op(3, 6, 200)));
    CHECK_EQ(q.pending(), OpQueue::kCapacity);
    CHECK_EQ(q.stats().evicted, 1u);

    Op out[OpQueue::kCapacity];
    CHECK_EQ(q.drain(out, OpQueue::kCapacity), OpQueue::kCapacity);
    // 队头是被逐出的那条的继任者（离散项 0），key=5 的旧值已不在
    CHECK_EQ(out[0].arg, 0);
    bool found_key5 = false;
    for (int i = 0; i < OpQueue::kCapacity; ++i) {
        if (out[i].key == 5) found_key5 = true;
    }
    CHECK_FALSE(found_key5);
    // 新推的可合并项在队尾
    CHECK_EQ(out[OpQueue::kCapacity - 1].arg, 200);
}

TEST(queue, reject_when_full_of_discrete) {
    OpQueue q;
    for (int i = 0; i < OpQueue::kCapacity; ++i) {
        CHECK_TRUE(q.push(make_op(4, 0, i)));
    }
    // 全是离散项：一条都不能逐，新入队被拒绝且计数
    CHECK_FALSE(q.push(make_op(4, 0, 9999)));
    CHECK_EQ(q.stats().rejected, 1u);
    CHECK_EQ(q.pending(), OpQueue::kCapacity);
}

TEST(queue, drain_budget) {
    OpQueue q;
    q.set_policy(OpQueue::Policy{2, 0});
    for (int i = 0; i < 5; ++i) CHECK_TRUE(q.push(make_op(1, 0, i)));

    Op out[8];
    CHECK_EQ(q.drain(out, 8), 2);   // 预算 2 压过请求的 8
    CHECK_EQ(out[0].arg, 0);
    CHECK_EQ(out[1].arg, 1);
    CHECK_EQ(q.drain(out, 8), 2);
    CHECK_EQ(q.drain(out, 8), 1);
    CHECK_EQ(q.drain(out, 8), 0);   // 空
}

TEST(queue, drain_interval_first_allowed) {
    OpQueue q;
    q.set_policy(OpQueue::Policy{0, 2});
    CHECK_TRUE(q.push(make_op(1, 0, 1)));

    Op out[4];
    // 首次排出不受间隔限制
    CHECK_EQ(q.drain(out, 4), 1);

    CHECK_TRUE(q.push(make_op(1, 0, 2)));
    CHECK_EQ(q.drain(out, 4), 0);           // 同一帧内第二次：被节流
    CHECK_EQ(q.stats().throttled, 1u);
    q.advance_frame();
    CHECK_EQ(q.drain(out, 4), 0);           // 只过了 1 帧，还差 1 帧
    q.advance_frame();
    CHECK_EQ(q.drain(out, 4), 1);           // 间隔够了
}

TEST(queue, empty_drain_keeps_interval) {
    OpQueue q;
    q.set_policy(OpQueue::Policy{0, 3});
    Op out[4];
    CHECK_EQ(q.drain(out, 4), 0);           // 空队列直接返回，不消耗间隔
    CHECK_EQ(q.stats().throttled, 0u);

    CHECK_TRUE(q.push(make_op(1, 0, 1)));
    CHECK_EQ(q.drain(out, 4), 1);           // 仍然允许（空排没有占位）
}

TEST(queue, clear_keeps_cumulative_stats) {
    OpQueue q;
    CHECK_TRUE(q.push(make_op(1, 0, 1)));
    q.clear();
    CHECK_EQ(q.pending(), 0);
    // clear 只清待处理，累计统计留着（排查"消费方没在排"要靠它们）
    CHECK_EQ(q.stats().pushed, 1u);

    // clear 之后还能正常入队（环形头复位，不会绕回去）
    CHECK_TRUE(q.push(make_op(1, 0, 2)));
    Op out[2];
    CHECK_EQ(q.drain(out, 2), 1);
    CHECK_EQ(out[0].arg, 2);
}

TEST(queue, same_key_after_drain_is_new_op) {
    OpQueue q;
    q.set_policy(OpQueue::Policy{0, 0});   // 本用例同一帧排两次，关掉默认间隔
    CHECK_TRUE(q.push(make_op(1, 7, 1)));
    Op out[2];
    CHECK_EQ(q.drain(out, 2), 1);
    // 排出后再推同键：是全新的一条（seq 继续递增），不是复活旧项
    CHECK_TRUE(q.push(make_op(1, 7, 2)));
    CHECK_EQ(q.pending(), 1);
    CHECK_EQ(q.drain(out, 2), 1);
    CHECK_EQ(out[0].arg, 2);
    CHECK_EQ(out[0].seq, 2);
}

TEST(queue, bad_drain_args) {
    OpQueue q;
    CHECK_TRUE(q.push(make_op(1, 0, 1)));
    CHECK_EQ(q.drain(nullptr, 4), 0);
    Op out[2];
    CHECK_EQ(q.drain(out, 0), 0);
    CHECK_EQ(q.drain(out, -1), 0);
    CHECK_EQ(q.pending(), 1);   // 坏参数不动队列
}

// ===========================================================================
// suite: abi —— GUi_* C ABI（进程内唯一队列）
// ===========================================================================

TEST(abi, push_drain_roundtrip) {
    abi_reset();
    const StatsSnap before = snap_stats();

    GUiOp op = c_op(7, 0, 42);
    op.vec[0] = 1.5f;
    op.vec[3] = -2.25f;
    CHECK_EQ(GUi_PushOp(&op), 1);

    GUiOp out[8];
    const int n = GUi_DrainOps(out, 8);
    CHECK_EQ(n, 1);
    CHECK_EQ(out[0].type, 7u);
    CHECK_EQ(out[0].key, 0u);
    CHECK_EQ(out[0].arg, 42);
    CHECK_NEAR(out[0].vec[0], 1.5, 1e-6);
    CHECK_NEAR(out[0].vec[3], -2.25, 1e-6);
    CHECK_TRUE(out[0].seq > 0);              // seq 由引擎填写

    const StatsSnap after = snap_stats();
    CHECK_EQ(after.pushed - before.pushed, 1u);
    CHECK_EQ(after.drained - before.drained, 1u);
}

TEST(abi, coalesce_and_discrete) {
    abi_reset();
    const StatsSnap before = snap_stats();

    GUiOp a = c_op(1, 5, 1);
    GUiOp b = c_op(1, 5, 2);
    GUiOp c = c_op(1, 5, 3);
    GUiOp d = c_op(2, 0, 4);
    CHECK_EQ(GUi_PushOp(&a), 1);
    CHECK_EQ(GUi_PushOp(&b), 1);
    CHECK_EQ(GUi_PushOp(&c), 1);
    CHECK_EQ(GUi_PushOp(&d), 1);

    GUiOp out[8];
    CHECK_EQ(GUi_DrainOps(out, 8), 2);       // 三条同键塌缩成一条 + 一条离散
    CHECK_EQ(out[0].arg, 3);
    CHECK_EQ(out[1].arg, 4);

    const StatsSnap after = snap_stats();
    CHECK_EQ(after.pushed - before.pushed, 4u);
    CHECK_EQ(after.coalesced - before.coalesced, 2u);
}

TEST(abi, policy_budget_and_get) {
    abi_reset();
    GUi_SetOpPolicy(2, 0);
    int mx = -1, mn = -1;
    GUi_GetOpPolicy(&mx, &mn);
    CHECK_EQ(mx, 2);
    CHECK_EQ(mn, 0);

    for (int i = 0; i < 5; ++i) {
        GUiOp o = c_op(1, 0, i);
        CHECK_EQ(GUi_PushOp(&o), 1);
    }
    GUiOp out[8];
    CHECK_EQ(GUi_DrainOps(out, 8), 2);
    CHECK_EQ(GUi_DrainOps(out, 8), 2);
    CHECK_EQ(GUi_DrainOps(out, 8), 1);
    CHECK_EQ(GUi_DrainOps(out, 8), 0);
}

TEST(abi, policy_interval) {
    abi_reset();
    GUi_SetOpPolicy(0, 2);
    GUiOp out[4];

    GUiOp a = c_op(1, 0, 1);
    CHECK_EQ(GUi_PushOp(&a), 1);
    CHECK_EQ(GUi_DrainOps(out, 4), 1);       // 首次不受限

    GUiOp b = c_op(1, 0, 2);
    CHECK_EQ(GUi_PushOp(&b), 1);
    CHECK_EQ(GUi_DrainOps(out, 4), 0);       // 未到间隔
    GUi_AdvanceOpFrame();
    CHECK_EQ(GUi_DrainOps(out, 4), 0);
    GUi_AdvanceOpFrame();
    CHECK_EQ(GUi_DrainOps(out, 4), 1);
}

TEST(abi, clear_and_stats_tolerate_null) {
    abi_reset();
    // 统计查询容忍任意 nullptr 组合
    GUi_GetOpQueueStats(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    GUiOp o = c_op(3, 0, 9);
    CHECK_EQ(GUi_PushOp(&o), 1);
    int pending = -1;
    GUi_GetOpQueueStats(&pending, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK_EQ(pending, 1);

    GUi_ClearOps();
    GUi_GetOpQueueStats(&pending, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    CHECK_EQ(pending, 0);
}

TEST(abi, bad_args_rejected) {
    abi_reset();
    CHECK_EQ(GUi_PushOp(nullptr), 0);
    CHECK_EQ(GUi_DrainOps(nullptr, 4), 0);
    GUiOp out[2];
    CHECK_EQ(GUi_DrainOps(out, 0), 0);
    CHECK_EQ(GUi_DrainOps(out, -3), 0);
}

TEST(abi, drain_caps_at_capacity) {
    abi_reset();
    // 请求超过容量时按容量封顶，不会写越界（策略 0 = 不限制）
    GUi_SetOpPolicy(0, 0);
    for (int i = 0; i < 8; ++i) {
        GUiOp o = c_op(1, 0, i);
        CHECK_EQ(GUi_PushOp(&o), 1);
    }
    GUiOp out[OpQueue::kCapacity];
    CHECK_EQ(GUi_DrainOps(out, OpQueue::kCapacity * 4), 8);
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    std::vector<std::string> wanted;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            std::vector<std::string> suites;
            for (const Case& c : registry()) {
                bool seen = false;
                for (const std::string& s : suites) seen = seen || (s == c.suite);
                if (!seen) suites.push_back(c.suite);
            }
            for (const std::string& s : suites) std::printf("%s\n", s.c_str());
            return 0;
        }
        if (arg == "--suite" && i + 1 < argc) {
            wanted.push_back(argv[++i]);
            continue;
        }
        if (!arg.empty() && arg[0] != '-') wanted.push_back(arg);
    }

    std::vector<std::string> suites;
    for (const Case& c : registry()) {
        bool seen = false;
        for (const std::string& s : suites) seen = seen || (s == c.suite);
        if (!seen) suites.push_back(c.suite);
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
            g_case_checks = 0;
            g_case_failed = 0;
            c.fn();
            ++cases_total;
            if (g_case_failed > 0) {
                std::printf("  [FAIL] %s (%d/%d checks failed)\n",
                            c.name.c_str(), g_case_failed, g_case_checks);
            } else {
                std::printf("  [ OK  ] %s (%d checks)\n", c.name.c_str(), g_case_checks);
            }
        }
    }

    std::printf("\n--------------------------------------------------------------\n");
    std::printf("checks: %d total, %d passed, %d failed\n",
                g_checks, g_checks - g_checks_failed, g_checks_failed);
    std::printf("cases : %d total, %d passed, %d failed\n",
                cases_total, cases_total - cases_failed, cases_failed);
    if (cases_failed == 0 && g_checks_failed == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL\n");
    return 1;
}
