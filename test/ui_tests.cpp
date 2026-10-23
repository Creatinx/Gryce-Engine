// ===========================================================================
// test/ui_tests.cpp
// UI 系统的单元测试：操作队列（OpQueue 本体 + GUi_* C ABI 两层）与模型层
// （跨帧状态 / 布局栈 / 组件树 / 主题 / 输入 / 视图模型）。
//
// 与 math / script / asset 测试同构：被测源码（api/ui_op_queue.cpp +
// api/ui_api.cpp + ui/*.cpp）直接编译进本可执行文件，不链接
// GryceRenderer.dll，不碰 GPU / 窗口 —— CI 上毫秒级完成。
//
// ui_api.cpp 与 ui/*.cpp 在**不定义 GRYCE_HAS_IMGUI** 的情况下编译：这同时
// 验证了"关掉 ImGui 的构建里 UI 模型层照常工作"（无窗口宿主也走这条路）。
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
#include "ui/ui_component.h"
#include "ui/ui_id.h"
#include "ui/ui_input.h"
#include "ui/ui_layout.h"
#include "ui/ui_snapshot.h"
#include "ui/ui_state.h"
#include "ui/ui_theme.h"

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
// suite: state —— 跨帧控件状态存储
// ===========================================================================

TEST(state, persist_across_frames) {
    gryce_ui::StateStore s;
    s.begin_frame(1);
    const uint64_t id = gryce_ui::hash_label("Opacity", 42);
    s.set_float(id, 0.35f);
    s.set_bool(id, true);
    s.set_int(id, 7);

    // 推进到另一帧：值必须还在（这就是"拖一下弹回去"的根治点）
    s.begin_frame(2);
    s.begin_frame(3);
    CHECK_NEAR(s.get_float(id, -1.0f), 0.35f, 1e-6f);
    CHECK_TRUE(s.get_bool(id, false));
    CHECK_EQ(s.get_int(id, -1), 7);
    CHECK_EQ(s.size(), 1u);
}

TEST(state, new_entry_defaults) {
    gryce_ui::StateStore s;
    s.begin_frame(1);
    const uint64_t id = gryce_ui::hash_label("fresh", 0);
    // 不存在的 id：读返回默认值，不碰 out
    float v = 3.0f;
    CHECK_FALSE(GUi_StateGetFloat(id, &v));
    CHECK_NEAR(v, 3.0f, 1e-6f);
    CHECK_NEAR(s.get_float(id, -5.0f), -5.0f, 1e-6f);

    // 写一次即创建：默认清零
    s.set_float(id, 1.0f);
    const gryce_ui::StateEntry* e = s.find(id);
    CHECK_TRUE(e != nullptr);
    if (e) {
        CHECK_EQ(e->i, 0);
        CHECK_EQ(e->b, 0);
        CHECK_EQ(e->text[0], '\0');
    }
}

TEST(state, distinct_ids_isolated) {
    gryce_ui::StateStore s;
    s.begin_frame(1);
    const uint64_t a = gryce_ui::hash_label("Position", 1);
    const uint64_t b = gryce_ui::hash_label("Position", 2);
    CHECK_TRUE(a != b);   // 同标签不同 seed 必须是不同 id
    s.set_float(a, 1.0f);
    s.set_float(b, 2.0f);
    CHECK_NEAR(s.get_float(a, 0.0f), 1.0f, 1e-6f);
    CHECK_NEAR(s.get_float(b, 0.0f), 2.0f, 1e-6f);
    CHECK_EQ(s.size(), 2u);
}

TEST(state, text_roundtrip_and_truncate) {
    gryce_ui::StateStore s;
    s.begin_frame(1);
    const uint64_t id = gryce_ui::hash_label("name", 0);
    s.set_text(id, "Cube");
    char buf[gryce_ui::StateEntry::kTextCap + 8];
    CHECK_EQ(s.get_text(id, buf, sizeof(buf)), 4u);
    CHECK_EQ(std::strcmp(buf, "Cube"), 0);

    // 超长截断到 kTextCap-1 并补 '\0'，不写越界
    std::string long_text(gryce_ui::StateEntry::kTextCap + 100, 'x');
    s.set_text(id, long_text.c_str());
    const size_t n = s.get_text(id, buf, sizeof(buf));
    CHECK_EQ(n, gryce_ui::StateEntry::kTextCap - 1);
    CHECK_EQ(buf[n], '\0');

    // 缺省路径：nullptr 文本等价于清空
    s.set_text(id, nullptr);
    CHECK_EQ(s.get_text(id, buf, sizeof(buf)), 0u);
    CHECK_EQ(buf[0], '\0');
}

TEST(state, vec4_roundtrip) {
    gryce_ui::StateStore s;
    s.begin_frame(1);
    const uint64_t id = gryce_ui::hash_label("color", 9);
    const float in[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    s.set_vec4(id, in);
    float out[4] = {0, 0, 0, 0};
    s.get_vec4(id, out);
    for (int i = 0; i < 4; ++i) CHECK_NEAR(out[i], in[i], 1e-6f);
    // 不存在的 id 读成全零而不是保留旧值
    const uint64_t missing = gryce_ui::hash_label("nope", 0);
    s.get_vec4(missing, out);
    CHECK_NEAR(out[0], 0.0f, 1e-6f);
    CHECK_NEAR(out[3], 0.0f, 1e-6f);
}

TEST(state, prune_drops_stale_keeps_boundary) {
    gryce_ui::StateStore s;
    s.begin_frame(100);
    // 填到 3/4 负载以上，之后每次 begin_frame 都会触发压缩
    for (int i = 0; i < 1700; ++i) {
        s.set_float(gryce_ui::hash_label("k", static_cast<uint64_t>(i)),
                    static_cast<float>(i));
    }
    CHECK_EQ(s.size(), 1700u);

    // 龄期恰好等于上限：保留（控件只要可见就会被 touch， aging 判据必须
    // 给足余量，节流 UI（--ui-interval N）下也不会误杀）
    s.begin_frame(100 + static_cast<uint32_t>(gryce_ui::StateStore::kMaxAgeFrames));
    CHECK_EQ(s.size(), 1700u);

    // 超过一帧：全部丢弃
    s.begin_frame(101 + static_cast<uint32_t>(gryce_ui::StateStore::kMaxAgeFrames));
    CHECK_EQ(s.size(), 0u);
}

TEST(state, clear_and_count_abi) {
    // C ABI 层：进程内唯一存储，用例之间用 Clear 隔离
    GUi_StateClear();
    CHECK_EQ(GUi_StateCount(), 0);
    const uint64_t id = GUi_MakeId("slider", 12345);
    GUi_StateSetFloat(id, 0.75f);
    GUi_StateSetBool(id, 1);
    float v = 0.0f;
    int b = 0;
    CHECK_EQ(GUi_StateGetFloat(id, &v), 1);
    CHECK_EQ(GUi_StateGetBool(id, &b), 1);
    CHECK_NEAR(v, 0.75f, 1e-6f);
    CHECK_EQ(b, 1);
    CHECK_EQ(GUi_StateCount(), 1);

    GUi_StateSetText(id, "hello");
    char buf[16] = {};
    CHECK_EQ(GUi_StateGetText(id, buf, sizeof(buf)), 1);
    CHECK_EQ(std::strcmp(buf, "hello"), 0);

    GUi_StateClear();
    CHECK_EQ(GUi_StateCount(), 0);
    CHECK_EQ(GUi_StateGetFloat(id, &v), 0);
}

TEST(state, make_id_stable_and_nonzero) {
    const uint64_t a = GUi_MakeId("Position", 7);
    const uint64_t b = GUi_MakeId("Position", 7);
    const uint64_t c = GUi_MakeId("Position", 8);
    const uint64_t d = GUi_MakeId("Rotation", 7);
    CHECK_TRUE(a != 0);            // 0 是状态存储的空槽标记，绝不允许
    CHECK_EQ(a, b);
    CHECK_TRUE(a != c);
    CHECK_TRUE(a != d);
    // 空标签也必须有合法 id（全局控件可能没有文本）
    CHECK_TRUE(GUi_MakeId(nullptr, 0) != 0);
    CHECK_EQ(GUi_MakeId(nullptr, 0), GUi_MakeId("", 0));
}

// ===========================================================================
// suite: layout —— 确定性布局栈
// ===========================================================================

namespace {

// 两个布局实例跑同一串调用，矩形必须逐个相等——确定性是本套件的主题。
bool same_rect(const gryce_ui::Rect& a, const gryce_ui::Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

gryce_ui::Rect rc(float x, float y, float w, float h) {
    return gryce_ui::Rect{x, y, w, h};
}

} // namespace

TEST(layout, panel_content_insets_padding) {
    gryce_ui::Layout l;
    l.reset(rc(0, 0, 800, 600));
    l.begin_panel(rc(10, 10, 300, 200), 6);
    const gryce_ui::Rect c = l.content();
    CHECK_NEAR(c.x, 16.0f, 1e-4f);
    CHECK_NEAR(c.y, 16.0f, 1e-4f);
    CHECK_NEAR(c.w, 288.0f, 1e-4f);
    CHECK_NEAR(c.h, 188.0f, 1e-4f);
    CHECK_EQ(l.depth(), 2);
    l.end_panel();
    CHECK_EQ(l.depth(), 1);
    CHECK_NEAR(l.content().w, 800.0f, 1e-4f);
}

TEST(layout, rows_and_cells_flow) {
    gryce_ui::Layout l;
    l.reset(rc(0, 0, 400, 300));
    l.set_spacing(0.0f);
    l.begin_panel(rc(0, 0, 400, 300), 0);

    l.begin_row(20);
    const gryce_ui::Rect r1 = l.cell(100, 0);   // h<=0 用行高
    const gryce_ui::Rect r2 = l.cell(0, 0);     // 填满剩余
    l.end_row();
    CHECK_TRUE(same_rect(r1, rc(0, 0, 100, 20)));
    CHECK_TRUE(same_rect(r2, rc(100, 0, 300, 20)));

    l.begin_row(20);
    const gryce_ui::Rect r3 = l.cell(0, 30);    // 显式行高
    l.end_row();
    CHECK_TRUE(same_rect(r3, rc(0, 20, 400, 30)));

    // 默认间距下行距 = 行高 + spacing
    l.set_spacing(4.0f);
    l.begin_row(10);
    l.cell(10, 0);
    l.end_row();
    l.begin_row(10);
    const gryce_ui::Rect r4 = l.cell(10, 0);
    l.end_row();
    // 前三行把光标推到 50（20+0、20+30+0），本行行高 10 + 间距 4
    CHECK_NEAR(r4.y, 64.0f, 1e-4f);
}

TEST(layout, begin_row_auto_ends_previous) {
    gryce_ui::Layout l;
    l.reset(rc(0, 0, 200, 200));
    l.set_spacing(2.0f);
    l.begin_panel(rc(0, 0, 200, 200), 0);
    l.begin_row(10);
    l.cell(5, 0);
    // 忘记 end_row：下一个 begin_row 必须先收尾，否则两行会叠在一起
    l.begin_row(10);
    const gryce_ui::Rect r = l.cell(5, 0);
    CHECK_NEAR(r.y, 12.0f, 1e-4f);   // 10 + spacing 2
}

TEST(layout, indent_applies_to_next_row) {
    gryce_ui::Layout l;
    l.reset(rc(0, 0, 200, 200));
    l.set_spacing(0.0f);
    l.begin_panel(rc(0, 0, 200, 200), 0);
    l.indent(18.0f);
    l.begin_row(10);
    const gryce_ui::Rect r = l.cell(50, 0);
    l.end_row();
    CHECK_NEAR(r.x, 18.0f, 1e-4f);
    l.unindent(18.0f);
    l.begin_row(10);
    const gryce_ui::Rect r2 = l.cell(50, 0);
    l.end_row();
    CHECK_NEAR(r2.x, 0.0f, 1e-4f);
    // 缩进不会缩成负数
    l.unindent(100.0f);
    l.begin_row(10);
    CHECK_NEAR(l.cell(10, 0).x, 0.0f, 1e-4f);
}

TEST(layout, space_and_avail_height) {
    gryce_ui::Layout l;
    l.reset(rc(0, 0, 200, 100));
    l.set_spacing(0.0f);
    l.begin_panel(rc(0, 0, 200, 100), 0);
    CHECK_NEAR(l.avail_height(), 100.0f, 1e-4f);
    l.begin_row(30);
    l.cell(10, 0);
    l.end_row();
    CHECK_NEAR(l.avail_height(), 70.0f, 1e-4f);
    l.space(20.0f);   // space 先收尾行（这里行已收），再下移
    CHECK_NEAR(l.avail_height(), 50.0f, 1e-4f);
    CHECK_NEAR(l.cursor_y(), 50.0f, 1e-4f);
}

TEST(layout, nested_panels_and_depth_cap) {
    gryce_ui::Layout l;
    l.reset(rc(0, 0, 100, 100));
    l.begin_panel(rc(10, 10, 50, 50), 5);   // depth 2
    l.begin_panel(rc(12, 12, 20, 20), 2);   // depth 3
    CHECK_EQ(l.depth(), 3);
    CHECK_NEAR(l.content().w, 16.0f, 1e-4f);
    l.end_panel();
    CHECK_NEAR(l.content().w, 40.0f, 1e-4f);   // 回到外层
    l.end_panel();
    CHECK_EQ(l.depth(), 1);

    // 超过最大层数的 begin_panel 被忽略，不越界
    for (int i = 0; i < 32; ++i) l.begin_panel(rc(0, 0, 10, 10), 1);
    CHECK_EQ(l.depth(), gryce_ui::Layout::kMaxDepth);
    for (int i = 0; i < 32; ++i) l.end_panel();
    CHECK_EQ(l.depth(), 1);
}

TEST(layout, degenerate_rects_clamped) {
    gryce_ui::Layout l;
    l.reset(rc(0, 0, 100, 100));
    l.begin_panel(rc(0, 0, 3, 3), 10);   // pad 比面板还大
    CHECK_NEAR(l.content().w, 0.0f, 1e-4f);
    CHECK_NEAR(l.content().h, 0.0f, 1e-4f);
    l.begin_row(20);
    const gryce_ui::Rect r = l.cell(0, 0);   // 剩余宽度为负 → 归零
    CHECK_NEAR(r.w, 0.0f, 1e-4f);
    l.end_row();
    l.end_panel();
}

TEST(layout, deterministic_across_instances) {
    auto run = [] {
        gryce_ui::Layout l;
        l.reset(rc(0, 0, 640, 480));
        l.set_spacing(3.0f);
        l.begin_panel(rc(8, 8, 300, 400), 6);
        l.begin_row(22);
        l.cell(80, 0);
        l.cell(0, 0);
        l.end_row();
        l.space(6);
        l.indent(18);
        l.begin_row(22);
        l.cell(0, 0);
        l.end_row();
        l.begin_panel(rc(20, 100, 100, 100), 4);
        l.begin_row(18);
        l.cell(40, 0);
        l.end_row();
        l.end_panel();
        return l.content();
    };
    // 同一序列在两个实例上给出同一个内容矩形（纯函数的定义）
    const gryce_ui::Rect a = run();
    const gryce_ui::Rect b = run();
    CHECK_TRUE(same_rect(a, b));
}

TEST(layout, abi_matches_model) {
    GRUiRect vp{0, 0, 800, 600};
    GUi_LayoutReset(&vp);
    GRUiRect area{10, 10, 300, 200};
    GUi_LayoutBeginPanel(&area, 6.0f);
    GRUiRect c{};
    GUi_LayoutContentRect(&c);
    CHECK_NEAR(c.x, 16.0f, 1e-4f);
    CHECK_NEAR(c.w, 288.0f, 1e-4f);
    CHECK_EQ(GUi_LayoutDepth(), 2);

    GUi_LayoutBeginRow(20.0f);
    GRUiRect cell{};
    CHECK_EQ(GUi_LayoutCell(100.0f, 0.0f, &cell), 1);
    CHECK_NEAR(cell.w, 100.0f, 1e-4f);
    CHECK_EQ(GUi_LayoutCell(-1.0f, -1.0f, &cell), 1);   // 负参数同 <=0 语义
    CHECK_NEAR(cell.w, 184.0f, 1e-4f);                  // 288 - 100 - 4(默认间距)
    GUi_LayoutEndRow();
    GUi_LayoutSpace(4.0f);
    GUi_LayoutIndent(18.0f);
    GUi_LayoutBeginRow(20.0f);
    GUi_LayoutCell(0.0f, 0.0f, &cell);
    CHECK_NEAR(cell.x, 34.0f, 1e-4f);                   // 内容区左 16 + 缩进 18
    GUi_LayoutEndRow();
    GUi_LayoutEndPanel();
    CHECK_EQ(GUi_LayoutDepth(), 1);
}

// ===========================================================================
// suite: comp —— 组件树布局（模型层）
//
// 核心是那条定位规则：父级是布局组件 -> 子节点由布局摆位；父级不是布局
// 组件（Leaf 或无父级）-> 子节点保留调用方写的 pos。
// ===========================================================================

using gryce_ui::Component;
using gryce_ui::ComponentTree;
using gryce_ui::CompAlign;
using gryce_ui::CompKind;

Component make_comp(uint64_t parent, CompKind kind, float x, float y, float w, float h) {
    Component c{};
    c.parent = parent;
    c.kind = kind;
    c.rect = gryce_ui::Rect{x, y, w, h};
    return c;
}

TEST(comp, add_find_remove) {
    ComponentTree t;
    const uint64_t a = t.add(make_comp(0, CompKind::Leaf, 10, 20, 100, 50));
    CHECK_TRUE(a != 0);
    const Component* ca = t.find(a);
    CHECK_TRUE(ca != nullptr);
    CHECK_EQ(ca->parent, 0u);
    CHECK_NEAR(ca->rect.x, 10.0f, 1e-4f);
    CHECK_EQ(t.count(), 1);

    const uint64_t b = t.add(make_comp(a, CompKind::Leaf, 0, 0, 10, 10));
    CHECK_TRUE(b != a);
    CHECK_EQ(t.find(b)->parent, a);
    CHECK_EQ(t.child_count(a), 1);
    CHECK_EQ(t.child_at(a, 0), b);

    // 移除带子树
    CHECK_TRUE(t.remove(a));
    CHECK_TRUE(t.find(a) == nullptr);
    CHECK_TRUE(t.find(b) == nullptr);
    CHECK_EQ(t.count(), 0);

    // id 单调递增、移除后不复用（野引用不会落到新组件上）
    const uint64_t c = t.add(make_comp(0, CompKind::Leaf, 0, 0, 1, 1));
    CHECK_TRUE(c != a);
    CHECK_TRUE(c != b);
}

TEST(comp, parent_must_exist) {
    ComponentTree t;
    const uint64_t a = t.add(make_comp(0, CompKind::Leaf, 0, 0, 10, 10));
    CHECK_EQ(t.add(make_comp(999, CompKind::Leaf, 0, 0, 1, 1)), 0u);
    CHECK_FALSE(t.set(a, make_comp(999, CompKind::Leaf, 0, 0, 1, 1)));
    CHECK_EQ(t.count(), 1);
    CHECK_EQ(t.child_count(999), -1);
    CHECK_TRUE(t.rect_of(999) == nullptr);
    CHECK_EQ(t.child_at(a, 0), 0u);
    CHECK_EQ(t.child_at(a, -1), 0u);
    CHECK_EQ(t.parent_of(999), 0u);
    CHECK_FALSE(t.remove(999));
}

TEST(comp, capacity) {
    ComponentTree t;
    std::vector<uint64_t> ids;
    ids.reserve(ComponentTree::kCapacity);
    for (int i = 0; i < ComponentTree::kCapacity; ++i) {
        const uint64_t id = t.add(make_comp(0, CompKind::Leaf, 0, 0, 1, 1));
        CHECK_TRUE(id != 0);
        ids.push_back(id);
    }
    CHECK_EQ(t.count(), ComponentTree::kCapacity);
    CHECK_EQ(t.add(make_comp(0, CompKind::Leaf, 0, 0, 1, 1)), 0u);   // 满
    CHECK_TRUE(t.remove(ids[0]));
    CHECK_TRUE(t.add(make_comp(0, CompKind::Leaf, 0, 0, 1, 1)) != 0);
}

TEST(comp, child_order_is_insertion_order) {
    ComponentTree t;
    const uint64_t p = t.add(make_comp(0, CompKind::Column, 0, 0, 100, 100));
    const uint64_t a = t.add(make_comp(p, CompKind::Leaf, 0, 0, 10, 10));
    const uint64_t b = t.add(make_comp(p, CompKind::Leaf, 0, 0, 10, 10));
    const uint64_t c = t.add(make_comp(p, CompKind::Leaf, 0, 0, 10, 10));
    CHECK_EQ(t.child_at(p, 0), a);
    CHECK_EQ(t.child_at(p, 1), b);
    CHECK_EQ(t.child_at(p, 2), c);
    CHECK_EQ(t.child_at(p, 3), 0u);

    // 删掉中间的，剩下的顺序保持（布局顺序 = 插入顺序，不重排）
    CHECK_TRUE(t.remove(b));
    CHECK_EQ(t.child_count(p), 2);
    CHECK_EQ(t.child_at(p, 0), a);
    CHECK_EQ(t.child_at(p, 1), c);
}

TEST(comp, set_rejects_cycles) {
    ComponentTree t;
    const uint64_t root = t.add(make_comp(0, CompKind::Column, 0, 0, 100, 100));
    const uint64_t mid = t.add(make_comp(root, CompKind::Column, 0, 0, 100, 50));
    const uint64_t leaf = t.add(make_comp(mid, CompKind::Leaf, 0, 0, 10, 10));

    Component bad = *t.find(root);
    bad.parent = leaf;      // 挂到自己的后代下
    CHECK_FALSE(t.set(root, bad));
    bad.parent = mid;
    CHECK_FALSE(t.set(root, bad));
    bad.parent = root;      // 自挂
    CHECK_FALSE(t.set(root, bad));
    CHECK_EQ(t.count(), 3);   // 树没被破坏
    CHECK_EQ(t.child_count(root), 1);

    // 合法重挂
    Component ok = *t.find(leaf);
    ok.parent = root;
    CHECK_TRUE(t.set(leaf, ok));
    CHECK_EQ(t.parent_of(leaf), root);
    CHECK_EQ(t.child_count(mid), 0);
    CHECK_EQ(t.child_count(root), 2);
}

TEST(comp, non_layout_parent_keeps_pos) {
    ComponentTree t;
    // 叶子当父级：子节点"直接写 pos"，resolve 不碰它
    const uint64_t anchor = t.add(make_comp(0, CompKind::Leaf, 100, 100, 200, 80));
    const uint64_t c1 = t.add(make_comp(anchor, CompKind::Leaf, 110, 120, 60, 20));
    const uint64_t c2 = t.add(make_comp(anchor, CompKind::Leaf, 180, 150, 60, 20));
    t.resolve();
    CHECK_NEAR(t.rect_of(c1)->x, 110.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(c1)->y, 120.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(c2)->x, 180.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(c2)->y, 150.0f, 1e-4f);

    // 无父级的根同样直接写 pos
    const uint64_t root = t.add(make_comp(0, CompKind::Leaf, 5, 6, 7, 8));
    t.resolve();
    CHECK_NEAR(t.rect_of(root)->x, 5.0f, 1e-4f);

    // 把 anchor 换成 Column：同一个子节点立刻改由布局摆位（规则翻转）
    Component up = *t.find(anchor);
    up.kind = CompKind::Column;
    up.gap = 10.0f;
    for (int i = 0; i < 4; ++i) up.padding[i] = 4.0f;
    CHECK_TRUE(t.set(anchor, up));
    t.resolve();
    // 内容区 (104,104,192,72)：c1 在 (104,104,60,20)，c2 在 (104,134,60,20)
    const gryce_ui::Rect* n1 = t.rect_of(c1);
    CHECK_NEAR(n1->x, 104.0f, 1e-4f);
    CHECK_NEAR(n1->y, 104.0f, 1e-4f);
    CHECK_NEAR(n1->w, 60.0f, 1e-4f);   // 默认 Start：用请求宽
    CHECK_NEAR(t.rect_of(c2)->y, 134.0f, 1e-4f);
}

TEST(comp, column_stack_arranges_children) {
    ComponentTree t;
    Component col = make_comp(0, CompKind::Column, 0, 0, 300, 200);
    col.gap = 8.0f;
    const uint64_t cid = t.add(col);
    const uint64_t a = t.add(make_comp(cid, CompKind::Leaf, 999, 999, 50, 30));
    Component b = make_comp(cid, CompKind::Leaf, 999, 999, 70, 40);
    b.margin[0] = 5.0f;   // 上
    b.margin[2] = 3.0f;   // 下
    const uint64_t bid = t.add(b);
    t.resolve();

    const gryce_ui::Rect* ra = t.rect_of(a);
    CHECK_NEAR(ra->x, 0.0f, 1e-4f);
    CHECK_NEAR(ra->y, 0.0f, 1e-4f);
    CHECK_NEAR(ra->w, 50.0f, 1e-4f);
    CHECK_NEAR(ra->h, 30.0f, 1e-4f);
    // b：y = 30(a) + 8(gap) + 5(margin上) = 43
    const gryce_ui::Rect* rb = t.rect_of(bid);
    CHECK_NEAR(rb->y, 43.0f, 1e-4f);
    CHECK_NEAR(rb->h, 40.0f, 1e-4f);
    CHECK_NEAR(rb->w, 70.0f, 1e-4f);
    // 调用方写的垃圾 pos 被布局覆盖（这正是"自动布局"的含义）
    CHECK_FALSE(ra->x == 999.0f);
}

TEST(comp, row_cross_align) {
    ComponentTree t;
    Component row = make_comp(0, CompKind::Row, 0, 0, 300, 100);
    const uint64_t rid = t.add(row);
    const uint64_t s = t.add(make_comp(rid, CompKind::Leaf, 0, 0, 40, 30));
    Component c = make_comp(rid, CompKind::Leaf, 0, 0, 40, 30);
    c.align = CompAlign::Center;
    const uint64_t cid = t.add(c);
    Component e = make_comp(rid, CompKind::Leaf, 0, 0, 40, 30);
    e.align = CompAlign::End;
    const uint64_t eid = t.add(e);
    Component st = make_comp(rid, CompKind::Leaf, 0, 0, 0, 0);
    st.align = CompAlign::Stretch;
    st.flex = 1.0f;              // 交叉轴吃满 + 主轴拿剩余：两个轴都填
    const uint64_t stid = t.add(st);
    t.resolve();

    // 交叉轴（竖直）对齐
    CHECK_NEAR(t.rect_of(s)->y, 0.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(cid)->y, 35.0f, 1e-4f);    // (100 - 30) / 2
    CHECK_NEAR(t.rect_of(eid)->y, 70.0f, 1e-4f);    // 100 - 30
    const gryce_ui::Rect* rst = t.rect_of(stid);
    CHECK_NEAR(rst->y, 0.0f, 1e-4f);
    CHECK_NEAR(rst->h, 100.0f, 1e-4f);              // Stretch 吃满交叉区
    // 主轴（水平）依次排：40 + 40 + 40 + 剩余
    CHECK_NEAR(t.rect_of(s)->x, 0.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(cid)->x, 40.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(eid)->x, 80.0f, 1e-4f);
    CHECK_NEAR(rst->x, 120.0f, 1e-4f);
    CHECK_NEAR(rst->w, 180.0f, 1e-4f);
}

TEST(comp, flex_distributes_remaining) {
    ComponentTree t;
    Component col = make_comp(0, CompKind::Column, 0, 0, 200, 300);
    const uint64_t cid = t.add(col);
    const uint64_t fixed = t.add(make_comp(cid, CompKind::Leaf, 0, 0, 100, 50));
    Component f1 = make_comp(cid, CompKind::Leaf, 0, 0, 100, 0);
    f1.flex = 1.0f;
    const uint64_t i1 = t.add(f1);
    Component f3 = make_comp(cid, CompKind::Leaf, 0, 0, 100, 0);
    f3.flex = 3.0f;
    const uint64_t i3 = t.add(f3);
    t.resolve();

    // 固定 50；剩余 250 按 1:3 分 -> 62.5 / 187.5
    CHECK_NEAR(t.rect_of(fixed)->h, 50.0f, 1e-3f);
    CHECK_NEAR(t.rect_of(i1)->h, 62.5f, 1e-3f);
    CHECK_NEAR(t.rect_of(i3)->h, 187.5f, 1e-3f);
    // 交叉轴默认 Start：宽度用请求值 100
    CHECK_NEAR(t.rect_of(i1)->w, 100.0f, 1e-3f);
    CHECK_NEAR(t.rect_of(i3)->y, 112.5f, 1e-3f);   // 50 + 62.5
}

TEST(comp, overflow_clamps_to_zero) {
    ComponentTree t;
    // 容器只有 40 高，固定子节点就要 50：放不下
    Component col = make_comp(0, CompKind::Column, 0, 0, 100, 40);
    const uint64_t cid = t.add(col);
    const uint64_t a = t.add(make_comp(cid, CompKind::Leaf, 0, 0, 100, 50));
    Component b = make_comp(cid, CompKind::Leaf, 0, 0, 100, 50);
    b.flex = 1.0f;
    const uint64_t bid = t.add(b);
    t.resolve();

    // 固定项按请求高落位（溢出就溢出）；弹性项剩余钳到 0，不产生负尺寸
    CHECK_NEAR(t.rect_of(a)->h, 50.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(bid)->h, 0.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(bid)->y, 50.0f, 1e-4f);
}

TEST(comp, grid_columns_and_row_height) {
    ComponentTree t;
    Component g = make_comp(0, CompKind::Grid, 0, 0, 320, 200);
    g.cols = 3;
    g.gap = 4.0f;
    const uint64_t gid = t.add(g);
    uint64_t ids[7] = {};
    for (int i = 0; i < 7; ++i) {
        ids[i] = t.add(make_comp(gid, CompKind::Leaf, 0, 0, 0, (i % 3 == 0) ? 40.0f : 20.0f));
    }
    t.resolve();

    // 列宽 = (320 - 4*2) / 3 = 104；行高 = 行内请求高的最大值
    CHECK_NEAR(t.rect_of(ids[0])->x, 0.0f, 1e-3f);
    CHECK_NEAR(t.rect_of(ids[0])->w, 104.0f, 1e-3f);
    CHECK_NEAR(t.rect_of(ids[0])->h, 40.0f, 1e-3f);
    CHECK_NEAR(t.rect_of(ids[1])->x, 108.0f, 1e-3f);
    CHECK_NEAR(t.rect_of(ids[1])->h, 40.0f, 1e-3f);   // 填满行高
    CHECK_NEAR(t.rect_of(ids[2])->x, 216.0f, 1e-3f);
    // 行 1：y = 40 + 4
    CHECK_NEAR(t.rect_of(ids[3])->y, 44.0f, 1e-3f);
    // 7 个节点 3 列 -> 行 2 只有 1 个：y = 44 + 40 + 4
    CHECK_NEAR(t.rect_of(ids[6])->y, 88.0f, 1e-3f);
    // 幂等：再解析一次结果相同
    t.resolve();
    CHECK_NEAR(t.rect_of(ids[3])->y, 44.0f, 1e-3f);
    CHECK_NEAR(t.rect_of(ids[0])->w, 104.0f, 1e-3f);
}

TEST(comp, padding_shrinks_content) {
    ComponentTree t;
    Component col = make_comp(0, CompKind::Column, 50, 60, 200, 100);
    col.padding[0] = 10.0f;   // 上
    col.padding[1] = 20.0f;   // 右
    col.padding[2] = 30.0f;   // 下
    col.padding[3] = 40.0f;   // 左
    const uint64_t cid = t.add(col);
    Component st = make_comp(cid, CompKind::Leaf, 0, 0, 0, 25);
    st.align = CompAlign::Stretch;
    const uint64_t sid = t.add(st);
    t.resolve();

    // 内容区 = (50+40, 60+10, 200-40-20, 100-10-30) = (90, 70, 140, 60)
    const gryce_ui::Rect* r = t.rect_of(sid);
    CHECK_NEAR(r->x, 90.0f, 1e-4f);
    CHECK_NEAR(r->y, 70.0f, 1e-4f);
    CHECK_NEAR(r->w, 140.0f, 1e-4f);   // Stretch 吃满内容宽
    CHECK_NEAR(r->h, 25.0f, 1e-4f);    // 主轴用请求高
}

TEST(comp, nested_trees) {
    ComponentTree t;
    // 根 Column -> Row（Stretch 吃满宽）-> 两个 Leaf
    Component col = make_comp(0, CompKind::Column, 0, 0, 400, 300);
    col.gap = 10.0f;
    const uint64_t cid = t.add(col);
    Component row = make_comp(cid, CompKind::Row, 0, 0, 0, 60);
    row.align = CompAlign::Stretch;   // 交叉轴（宽）吃满
    row.gap = 6.0f;
    const uint64_t rid = t.add(row);
    const uint64_t l1 = t.add(make_comp(rid, CompKind::Leaf, 0, 0, 80, 20));
    const uint64_t l2 = t.add(make_comp(rid, CompKind::Leaf, 0, 0, 80, 20));
    t.resolve();

    CHECK_NEAR(t.rect_of(rid)->y, 0.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(rid)->w, 400.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(rid)->h, 60.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(l1)->x, 0.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(l2)->x, 86.0f, 1e-4f);   // 80 + gap 6

    // 叶子下挂布局组件：叶子不是布局组件 -> 这个 Column 直接写 pos；
    // 但它自己是布局组件 -> 它的子节点照样由它摆位
    const uint64_t host = t.add(make_comp(0, CompKind::Leaf, 500, 400, 300, 200));
    Component inner = make_comp(host, CompKind::Column, 10, 20, 100, 150);
    inner.gap = 5.0f;
    const uint64_t iid = t.add(inner);
    const uint64_t leaf2 = t.add(make_comp(iid, CompKind::Leaf, 0, 0, 30, 30));
    t.resolve();

    const gryce_ui::Rect* ri = t.rect_of(iid);
    CHECK_NEAR(ri->x, 10.0f, 1e-4f);
    CHECK_NEAR(ri->y, 20.0f, 1e-4f);
    const gryce_ui::Rect* r2 = t.rect_of(leaf2);
    CHECK_NEAR(r2->x, 10.0f, 1e-4f);
    CHECK_NEAR(r2->y, 20.0f, 1e-4f);
    CHECK_NEAR(r2->w, 30.0f, 1e-4f);
}

TEST(comp, degenerate_container_keeps_children) {
    ComponentTree t;
    // 容器没有内容区（padding 吃掉全部）：子节点 rect 保持原值，不崩不写坏
    Component col = make_comp(0, CompKind::Column, 0, 0, 10, 10);
    for (int i = 0; i < 4; ++i) col.padding[i] = 8.0f;
    const uint64_t cid = t.add(col);
    const uint64_t a = t.add(make_comp(cid, CompKind::Leaf, 33, 44, 5, 5));
    t.resolve();
    CHECK_NEAR(t.rect_of(a)->x, 33.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(a)->y, 44.0f, 1e-4f);

    // 空容器：resolve 不碰任何东西
    ComponentTree t2;
    const uint64_t empty = t2.add(make_comp(0, CompKind::Grid, 0, 0, 100, 100));
    t2.resolve();
    CHECK_NEAR(t2.rect_of(empty)->w, 100.0f, 1e-4f);
    (void)empty;
}

// ===========================================================================
// suite: compabi —— 组件树的 C ABI
// ===========================================================================

GUiCompSpec c_spec(uint64_t parent, int kind, float x, float y, float w, float h) {
    GUiCompSpec s{};
    s.parent = parent;
    s.kind = kind;
    s.x = x; s.y = y; s.w = w; s.h = h;
    return s;
}

void comp_reset() {
    GUi_CompClear();
}

TEST(compabi, add_get_roundtrip) {
    comp_reset();
    CHECK_EQ(GUi_CompCount(), 0);

    GUiCompSpec s = c_spec(0, GUI_COMP_KIND_COLUMN, 10, 20, 300, 200);
    s.gap = 4.0f;
    s.padding[0] = 2.0f; s.padding[1] = 3.0f; s.padding[2] = 4.0f; s.padding[3] = 5.0f;
    s.cols = 2;
    const uint64_t id = GUi_CompAdd(&s);
    CHECK_TRUE(id != 0);
    CHECK_EQ(GUi_CompCount(), 1);
    CHECK_EQ(GUi_CompKind(id), GUI_COMP_KIND_COLUMN);
    CHECK_EQ(GUi_CompParent(id), 0u);

    GUiCompSpec back{};
    CHECK_EQ(GUi_CompGet(id, &back), 1);
    CHECK_EQ(back.kind, GUI_COMP_KIND_COLUMN);
    CHECK_NEAR(back.x, 10.0f, 1e-4f);
    CHECK_NEAR(back.gap, 4.0f, 1e-4f);
    CHECK_NEAR(back.padding[3], 5.0f, 1e-4f);
    CHECK_EQ(back.cols, 2);

    // 不存在的 id
    CHECK_EQ(GUi_CompKind(9999), -1);
    CHECK_EQ(GUi_CompGet(9999, &back), 0);
    CHECK_EQ(GUi_CompRect(9999, nullptr), 0);
    CHECK_EQ(GUi_CompParent(9999), 0u);

    // 坏参数：越界枚举直接拒绝，不静默夹值
    CHECK_EQ(GUi_CompAdd(nullptr), 0u);
    GUiCompSpec bad = c_spec(0, 99, 0, 0, 1, 1);
    CHECK_EQ(GUi_CompAdd(&bad), 0u);
    bad = c_spec(0, GUI_COMP_KIND_LEAF, 0, 0, 1, 1);
    bad.align = 99;
    CHECK_EQ(GUi_CompAdd(&bad), 0u);
    CHECK_EQ(GUi_CompSet(id, nullptr), 0);
    CHECK_EQ(GUi_CompCount(), 1);
}

TEST(compabi, resolve_writes_rects) {
    comp_reset();
    GUiCompSpec col = c_spec(0, GUI_COMP_KIND_COLUMN, 0, 0, 200, 100);
    col.gap = 10.0f;
    const uint64_t cid = GUi_CompAdd(&col);
    GUiCompSpec sa = c_spec(cid, GUI_COMP_KIND_LEAF, 7, 7, 50, 30);
    const uint64_t aid = GUi_CompAdd(&sa);
    GUiCompSpec sb = c_spec(cid, GUI_COMP_KIND_LEAF, 8, 8, 60, 20);
    const uint64_t bid = GUi_CompAdd(&sb);

    CHECK_EQ(GUi_CompResolve(), 3);   // 返回解析到的组件数
    GRUiRect ra{}, rb{};
    CHECK_EQ(GUi_CompRect(aid, &ra), 1);
    CHECK_EQ(GUi_CompRect(bid, &rb), 1);
    CHECK_NEAR(ra.y, 0.0f, 1e-4f);
    CHECK_NEAR(rb.y, 40.0f, 1e-4f);   // 30 + gap 10
    CHECK_NEAR(ra.w, 50.0f, 1e-4f);

    // 枚举
    CHECK_EQ(GUi_CompChildCount(cid), 2);
    CHECK_EQ(GUi_CompChildAt(cid, 0), aid);
    CHECK_EQ(GUi_CompChildAt(cid, 1), bid);
    CHECK_EQ(GUi_CompChildAt(cid, 2), 0u);
    CHECK_EQ(GUi_CompChildCount(9999), -1);
}

TEST(compabi, non_layout_parent_keeps_pos) {
    comp_reset();
    GUiCompSpec host = c_spec(0, GUI_COMP_KIND_LEAF, 100, 100, 200, 80);
    const uint64_t hid = GUi_CompAdd(&host);
    GUiCompSpec kid = c_spec(hid, GUI_COMP_KIND_LEAF, 111, 222, 33, 44);
    const uint64_t cid = GUi_CompAdd(&kid);
    CHECK_EQ(GUi_CompParent(cid), hid);

    GUi_CompResolve();
    GRUiRect r{};
    CHECK_EQ(GUi_CompRect(cid, &r), 1);
    CHECK_NEAR(r.x, 111.0f, 1e-4f);   // 父级不是布局组件：pos 原样保留
    CHECK_NEAR(r.y, 222.0f, 1e-4f);

    // 重挂到布局组件下：立刻改由布局摆位
    GUiCompSpec col = c_spec(0, GUI_COMP_KIND_COLUMN, 0, 0, 200, 100);
    const uint64_t colid = GUi_CompAdd(&col);
    GUiCompSpec mv{};
    CHECK_EQ(GUi_CompGet(cid, &mv), 1);
    mv.parent = colid;
    CHECK_EQ(GUi_CompSet(cid, &mv), 1);
    CHECK_EQ(GUi_CompParent(cid), colid);
    GUi_CompResolve();
    CHECK_EQ(GUi_CompRect(cid, &r), 1);
    CHECK_NEAR(r.x, 0.0f, 1e-4f);
    CHECK_NEAR(r.y, 0.0f, 1e-4f);

    // set 到不存在的父级被拒
    GUiCompSpec bad = c_spec(9999, GUI_COMP_KIND_LEAF, 0, 0, 1, 1);
    CHECK_EQ(GUi_CompSet(cid, &bad), 0);
    CHECK_EQ(GUi_CompParent(cid), colid);
}

TEST(compabi, remove_takes_subtree) {
    comp_reset();
    GUiCompSpec col = c_spec(0, GUI_COMP_KIND_COLUMN, 0, 0, 100, 100);
    const uint64_t cid = GUi_CompAdd(&col);
    GUiCompSpec sb = c_spec(cid, GUI_COMP_KIND_LEAF, 0, 0, 10, 10);
    const uint64_t b = GUi_CompAdd(&sb);
    GUiCompSpec sd = c_spec(b, GUI_COMP_KIND_LEAF, 1, 2, 3, 4);
    const uint64_t d = GUi_CompAdd(&sd);
    CHECK_EQ(GUi_CompCount(), 3);

    CHECK_EQ(GUi_CompRemove(b), 1);
    CHECK_EQ(GUi_CompCount(), 1);          // b 与 d 一起没
    GRUiRect r{};
    CHECK_EQ(GUi_CompRect(d, &r), 0);
    CHECK_EQ(GUi_CompRemove(b), 0);        // 已不存在
    CHECK_EQ(GUi_CompChildCount(cid), 0);
}

TEST(compabi, set_rejects_cycle_through_abi) {
    comp_reset();
    GUiCompSpec root = c_spec(0, GUI_COMP_KIND_COLUMN, 0, 0, 100, 100);
    const uint64_t rid = GUi_CompAdd(&root);
    GUiCompSpec leaf = c_spec(rid, GUI_COMP_KIND_LEAF, 0, 0, 10, 10);
    const uint64_t lid = GUi_CompAdd(&leaf);

    GUiCompSpec s{};
    CHECK_EQ(GUi_CompGet(rid, &s), 1);
    s.parent = lid;                        // 挂到自己的后代下
    CHECK_EQ(GUi_CompSet(rid, &s), 0);
    CHECK_EQ(GUi_CompParent(rid), 0u);     // 未被破坏
    CHECK_EQ(GUi_CompCount(), 2);
}

// ===========================================================================
// suite: theme —— 主题 token
// ===========================================================================

TEST(theme, default_dark_preset) {
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_TEXT), 0xE6E6EAFFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0x1F1F24FFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0x4C8DFFFFu);
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_ROW_H), 22.0f, 1e-4f);
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_INDENT), 18.0f, 1e-4f);
}

TEST(theme, light_preset_swaps_and_back) {
    GUi_ThemeSetPreset(GUI_THEME_PRESET_LIGHT);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0xF2F2F4FFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_TEXT), 0x222228FFu);
    // 度量与预设无关
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_ROW_H), 22.0f, 1e-4f);
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0x1F1F24FFu);
}

TEST(theme, per_token_override_and_reset) {
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    GUi_ThemeSetColor(GUI_THEME_COLOR_ACCENT, 0xFF0000FFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0xFF0000FFu);
    // 换预设整体重置（个别覆盖不跨预设存活）
    GUi_ThemeSetPreset(GUI_THEME_PRESET_LIGHT);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0x2E6BE6FFu);

    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    GUi_ThemeSetColor(GUI_THEME_COLOR_TEXT, 0x123456FFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_TEXT), 0x123456FFu);
}

TEST(theme, bad_tokens_tolerated) {
    // 从干净预设开始：主题存储是进程内唯一的，上一条用例的覆盖会残留
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    // 越界 token：读返回 0、写忽略，不崩不写坏内存
    CHECK_EQ(GUi_ThemeColor(-1), 0u);
    CHECK_EQ(GUi_ThemeColor(9999), 0u);
    CHECK_EQ(GUi_ThemeMetric(-1), 0.0f);
    GUi_ThemeSetColor(-1, 0xFFFFFFFFu);
    GUi_ThemeSetColor(GUI_THEME_COLOR_COUNT + 5, 0xFFFFFFFFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_TEXT), 0xE6E6EAFFu);   // 未被误改

    // 未知 preset 回落深色（调用方传错值时行为仍确定）
    GUi_ThemeSetPreset(42);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0x1F1F24FFu);
}

TEST(theme, apply_is_safe_without_imgui) {
    // 本测试可执行不定义 GRYCE_HAS_IMGUI：apply() 退化为 no-op，token 不变
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    const uint32_t before = GUi_ThemeColor(GUI_THEME_COLOR_TEXT);
    GUi_ThemeApply();
    GUi_ThemeApply();
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_TEXT), before);
}

// ===========================================================================
// suite: input —— 输入门控与快捷键表
// ===========================================================================

TEST(input, shortcut_add_find_clear) {
    gryce_ui::ShortcutTable t;
    CHECK_EQ(t.size(), 0);
    CHECK_EQ(t.add(294, 0, 5), 1);              // F5，无修饰
    CHECK_EQ(t.add(294, gryce_ui::UI_MOD_CTRL, 6), 1);   // Ctrl+F5 是另一个键
    CHECK_EQ(t.find(294, 0), 5);
    CHECK_EQ(t.find(294, gryce_ui::UI_MOD_CTRL), 6);
    CHECK_EQ(t.find(294, gryce_ui::UI_MOD_SHIFT), -1);   // 修饰键必须精确匹配
    CHECK_EQ(t.find(999, 0), -1);
    CHECK_EQ(t.size(), 2);
    t.clear();
    CHECK_EQ(t.size(), 0);
    CHECK_EQ(t.find(294, 0), -1);
}

TEST(input, shortcut_conflict_rejected) {
    gryce_ui::ShortcutTable t;
    CHECK_EQ(t.add(65, gryce_ui::UI_MOD_CTRL, 1), 1);
    CHECK_TRUE(t.has_conflict(65, gryce_ui::UI_MOD_CTRL));
    // 同一个 (key,mods) 第二次注册失败，且不覆盖第一条
    CHECK_EQ(t.add(65, gryce_ui::UI_MOD_CTRL, 2), 0);
    CHECK_EQ(t.find(65, gryce_ui::UI_MOD_CTRL), 1);
    CHECK_EQ(t.size(), 1);
}

TEST(input, shortcut_capacity) {
    gryce_ui::ShortcutTable t;
    for (int i = 0; i < gryce_ui::ShortcutTable::kCapacity; ++i) {
        CHECK_EQ(t.add(static_cast<uint32_t>(i + 1), 0, i), 1);
    }
    CHECK_EQ(t.add(9999, 0, -1), 0);   // 满：拒绝而不是挤掉别人
    CHECK_EQ(t.size(), gryce_ui::ShortcutTable::kCapacity);
}

TEST(input, dispatch_gate_without_imgui) {
    // 无 ImGui 构建：没有 UI 在跑，也就没有 UI 在吃键盘 —— 门控恒放行。
    // 这条同时锁死"关掉 ImGui 时快捷键全部失效"的回归。
    CHECK_EQ(GUi_ShouldDispatchShortcut(), 1);
    CHECK_FALSE(gryce_ui::wants_capture_keyboard());
    CHECK_FALSE(gryce_ui::wants_capture_mouse());
}

TEST(input, abi_shortcut_table) {
    GUi_ShortcutClear();
    CHECK_EQ(GUi_ShortcutAdd(294, GUI_MOD_CTRL, 11), 1);
    CHECK_EQ(GUi_ShortcutFind(294, GUI_MOD_CTRL), 11);
    CHECK_EQ(GUi_ShortcutFind(294, 0), -1);
    CHECK_EQ(GUi_ShortcutAdd(294, GUI_MOD_CTRL, 12), 0);   // 冲突
    CHECK_EQ(GUi_ShortcutFind(294, GUI_MOD_CTRL), 11);     // 未被覆盖
    GUi_ShortcutClear();
    CHECK_EQ(GUi_ShortcutFind(294, GUI_MOD_CTRL), -1);
}

// ===========================================================================
// suite: snapshot —— 面板视图模型与 JSON 往返
// ===========================================================================

TEST(snapshot, json_roundtrip_all_kinds) {
    using gryce_ui::Panel;
    using gryce_ui::Row;
    using gryce_ui::RowKind;

    Panel p;
    std::strcpy(p.title, "Inspector");
    Row r{};
    r.kind = RowKind::Slider;
    r.id = 0x1234567890ABCDEFULL;
    std::strcpy(r.label, "Opacity");
    r.value = 0.5f; r.min_v = 0.0f; r.max_v = 1.0f;
    CHECK_TRUE(p.add(r));
    r = Row{};
    r.kind = RowKind::Checkbox;
    r.id = 7;
    std::strcpy(r.label, "Shadows");
    r.flag = 1;
    CHECK_TRUE(p.add(r));
    r = Row{};
    r.kind = RowKind::Text;
    r.id = 8;
    std::strcpy(r.label, "Name");
    std::strcpy(r.text, "Cube");
    CHECK_TRUE(p.add(r));
    r = Row{};
    r.kind = RowKind::Separator;
    CHECK_TRUE(p.add(r));
    CHECK_EQ(p.row_count, 4u);

    const nlohmann::json j = gryce_ui::to_json(p);
    Panel back;
    CHECK_TRUE(gryce_ui::from_json(j, back));
    CHECK_EQ(std::strcmp(back.title, "Inspector"), 0);
    CHECK_EQ(back.row_count, 4u);
    CHECK_EQ(back.rows[0].id, 0x1234567890ABCDEFULL);   // 64 位原样回来
    CHECK_EQ(std::strcmp(back.rows[0].label, "Opacity"), 0);
    CHECK_NEAR(back.rows[0].value, 0.5f, 1e-6f);
    CHECK_EQ(back.rows[1].flag, 1);
    CHECK_EQ(std::strcmp(back.rows[2].text, "Cube"), 0);
    CHECK_EQ(static_cast<uint32_t>(back.rows[3].kind),
             static_cast<uint32_t>(RowKind::Separator));
}

TEST(snapshot, id_serializes_as_decimal_string) {
    using gryce_ui::Panel;
    using gryce_ui::Row;
    Panel p;
    Row r{};
    r.id = 9007199254740993ULL;   // 2^53 + 1：JS 数字精度丢得掉的最低一位
    std::strcpy(r.label, "edge");
    CHECK_TRUE(p.add(r));
    const nlohmann::json j = gryce_ui::to_json(p);
    CHECK_TRUE(j["rows"][0]["id"].is_string());
    CHECK_EQ(std::strcmp(j["rows"][0]["id"].get<std::string>().c_str(),
                         "9007199254740993"),
             0);
    Panel back;
    CHECK_TRUE(gryce_ui::from_json(j, back));
    CHECK_EQ(back.rows[0].id, 9007199254740993ULL);
}

TEST(snapshot, from_json_tolerant) {
    using gryce_ui::Panel;
    // 非对象：明确失败
    Panel dummy;
    CHECK_FALSE(gryce_ui::from_json(nlohmann::json::array(), dummy));
    // 缺 rows：视为空面板而不是错误
    Panel p;
    CHECK_TRUE(gryce_ui::from_json(nlohmann::json::object(), p));
    CHECK_EQ(p.row_count, 0u);
    // 坏行（未知 kind / 非对象）跳过，好行保留
    const nlohmann::json j = nlohmann::json::parse(R"({
        "title": "T",
        "rows": [
            {"kind": "wat", "id": "1", "label": "bad"},
            {"kind": "label", "id": "2", "label": "good"},
            42
        ]
    })");
    CHECK_TRUE(gryce_ui::from_json(j, p));
    CHECK_EQ(p.row_count, 1u);
    CHECK_EQ(std::strcmp(p.rows[0].label, "good"), 0);
    CHECK_EQ(p.rows[0].id, 2u);
}

TEST(snapshot, row_kind_names_roundtrip) {
    for (uint32_t i = 0; i <= static_cast<uint32_t>(gryce_ui::RowKind::Custom); ++i) {
        const auto k = static_cast<gryce_ui::RowKind>(i);
        gryce_ui::RowKind back{};
        CHECK_TRUE(gryce_ui::row_kind_from_name(gryce_ui::row_kind_name(k), back));
        CHECK_EQ(static_cast<uint32_t>(back), i);
    }
    gryce_ui::RowKind out{};
    CHECK_FALSE(gryce_ui::row_kind_from_name("nonsense", out));
    CHECK_FALSE(gryce_ui::row_kind_from_name(nullptr, out));
}

TEST(snapshot, capacity_and_truncation) {
    using gryce_ui::Panel;
    using gryce_ui::Row;
    Panel p;
    for (size_t i = 0; i < Panel::kMaxRows; ++i) CHECK_TRUE(p.add(Row{}));
    CHECK_FALSE(p.add(Row{}));   // 满：拒绝而不是越界
    CHECK_EQ(p.row_count, Panel::kMaxRows);

    p.clear();
    CHECK_EQ(p.row_count, 0u);
    Row r{};
    const std::string long_label(Panel::kTitleCap + 100, 'L');
    // 源头就用带截断的拷贝：strcpy 不截断，会把 48 字节的数组写爆
    std::strncpy(r.label, long_label.c_str(), sizeof(r.label) - 1);
    CHECK_TRUE(p.add(r));
    CHECK_EQ(std::strlen(p.rows[0].label), Row::kLabelCap - 1);

    // 超长 title 经 JSON 往返后截断
    p.clear();
    std::strncpy(p.title, long_label.c_str(), sizeof(p.title) - 1);
    const nlohmann::json j = gryce_ui::to_json(p);
    Panel back;
    CHECK_TRUE(gryce_ui::from_json(j, back));
    CHECK_EQ(std::strlen(back.title), Panel::kTitleCap - 1);
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
                ++cases_failed;
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
