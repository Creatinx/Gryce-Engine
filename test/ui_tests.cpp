// ===========================================================================
// test/ui_tests.cpp
// UI 系统的单元测试：操作队列（OpQueue 本体 + GUi_* C ABI 两层）与模型层
// （跨帧状态 / 布局栈 / 布局树 / 主题 / 输入 / 视图模型 / 动画系统）。
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
#include "ui/ui_anim.h"
#include "ui/ui_layout_tree.h"
#include "ui/ui_drawlist.h"
#include "ui/ui_id.h"
#include "ui/ui_input.h"
#include "ui/ui_layout.h"
#include "ui/ui_render_bridge.h"
#include "ui/ui_snapshot.h"
#include "ui/ui_state.h"
#include "ui/ui_texture_v_flip.h"
#include "ui/ui_theme.h"
#include "ui/ui_widget.h"
#include "ui/ui_builtin.h"
#include "ui/ui_layout_tree_json.h"

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

// 字符串相等（CHECK_EQ 只接数值：mock 渲染器记录的是 std::string）
#define CHECK_STR(actual, expected)                                                  \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        const std::string a_ = (actual);                                             \
        const std::string b_ = (expected);                                           \
        if (a_ != b_) ::fail_at(__FILE__, __LINE__,                                  \
                               std::string(#actual) + " = \"" + a_ +                 \
                               "\", expected \"" + b_ + "\"");                       \
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
// suite: comp —— 布局树（模型层）
//
// 核心是那条定位规则：父级是布局组件 -> 子节点由布局摆位；父级不是布局
// 组件（Leaf 或无父级）-> 子节点保留调用方写的 pos。
// ===========================================================================

using gryce_ui::UiLayoutNode;
using gryce_ui::UiLayoutTree;
using gryce_ui::UiLayoutAlign;
using gryce_ui::UiLayoutKind;

UiLayoutNode make_comp(uint64_t parent, UiLayoutKind kind, float x, float y, float w, float h) {
    UiLayoutNode c{};
    c.parent = parent;
    c.kind = kind;
    c.rect = gryce_ui::Rect{x, y, w, h};
    return c;
}

TEST(comp, add_find_remove) {
    UiLayoutTree t;
    const uint64_t a = t.add(make_comp(0, UiLayoutKind::Leaf, 10, 20, 100, 50));
    CHECK_TRUE(a != 0);
    const UiLayoutNode* ca = t.find(a);
    CHECK_TRUE(ca != nullptr);
    CHECK_EQ(ca->parent, 0u);
    CHECK_NEAR(ca->rect.x, 10.0f, 1e-4f);
    CHECK_EQ(t.count(), 1);

    const uint64_t b = t.add(make_comp(a, UiLayoutKind::Leaf, 0, 0, 10, 10));
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
    const uint64_t c = t.add(make_comp(0, UiLayoutKind::Leaf, 0, 0, 1, 1));
    CHECK_TRUE(c != a);
    CHECK_TRUE(c != b);
}

TEST(comp, parent_must_exist) {
    UiLayoutTree t;
    const uint64_t a = t.add(make_comp(0, UiLayoutKind::Leaf, 0, 0, 10, 10));
    CHECK_EQ(t.add(make_comp(999, UiLayoutKind::Leaf, 0, 0, 1, 1)), 0u);
    CHECK_FALSE(t.set(a, make_comp(999, UiLayoutKind::Leaf, 0, 0, 1, 1)));
    CHECK_EQ(t.count(), 1);
    CHECK_EQ(t.child_count(999), -1);
    CHECK_TRUE(t.rect_of(999) == nullptr);
    CHECK_EQ(t.child_at(a, 0), 0u);
    CHECK_EQ(t.child_at(a, -1), 0u);
    CHECK_EQ(t.parent_of(999), 0u);
    CHECK_FALSE(t.remove(999));
}

TEST(comp, capacity) {
    UiLayoutTree t;
    std::vector<uint64_t> ids;
    ids.reserve(UiLayoutTree::kCapacity);
    for (int i = 0; i < UiLayoutTree::kCapacity; ++i) {
        const uint64_t id = t.add(make_comp(0, UiLayoutKind::Leaf, 0, 0, 1, 1));
        CHECK_TRUE(id != 0);
        ids.push_back(id);
    }
    CHECK_EQ(t.count(), UiLayoutTree::kCapacity);
    CHECK_EQ(t.add(make_comp(0, UiLayoutKind::Leaf, 0, 0, 1, 1)), 0u);   // 满
    CHECK_TRUE(t.remove(ids[0]));
    CHECK_TRUE(t.add(make_comp(0, UiLayoutKind::Leaf, 0, 0, 1, 1)) != 0);
}

TEST(comp, child_order_is_insertion_order) {
    UiLayoutTree t;
    const uint64_t p = t.add(make_comp(0, UiLayoutKind::Column, 0, 0, 100, 100));
    const uint64_t a = t.add(make_comp(p, UiLayoutKind::Leaf, 0, 0, 10, 10));
    const uint64_t b = t.add(make_comp(p, UiLayoutKind::Leaf, 0, 0, 10, 10));
    const uint64_t c = t.add(make_comp(p, UiLayoutKind::Leaf, 0, 0, 10, 10));
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
    UiLayoutTree t;
    const uint64_t root = t.add(make_comp(0, UiLayoutKind::Column, 0, 0, 100, 100));
    const uint64_t mid = t.add(make_comp(root, UiLayoutKind::Column, 0, 0, 100, 50));
    const uint64_t leaf = t.add(make_comp(mid, UiLayoutKind::Leaf, 0, 0, 10, 10));

    UiLayoutNode bad = *t.find(root);
    bad.parent = leaf;      // 挂到自己的后代下
    CHECK_FALSE(t.set(root, bad));
    bad.parent = mid;
    CHECK_FALSE(t.set(root, bad));
    bad.parent = root;      // 自挂
    CHECK_FALSE(t.set(root, bad));
    CHECK_EQ(t.count(), 3);   // 树没被破坏
    CHECK_EQ(t.child_count(root), 1);

    // 合法重挂
    UiLayoutNode ok = *t.find(leaf);
    ok.parent = root;
    CHECK_TRUE(t.set(leaf, ok));
    CHECK_EQ(t.parent_of(leaf), root);
    CHECK_EQ(t.child_count(mid), 0);
    CHECK_EQ(t.child_count(root), 2);
}

TEST(comp, non_layout_parent_keeps_pos) {
    UiLayoutTree t;
    // 叶子当父级：子节点"直接写 pos"，resolve 不碰它
    const uint64_t anchor = t.add(make_comp(0, UiLayoutKind::Leaf, 100, 100, 200, 80));
    const uint64_t c1 = t.add(make_comp(anchor, UiLayoutKind::Leaf, 110, 120, 60, 20));
    const uint64_t c2 = t.add(make_comp(anchor, UiLayoutKind::Leaf, 180, 150, 60, 20));
    t.resolve();
    CHECK_NEAR(t.rect_of(c1)->x, 110.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(c1)->y, 120.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(c2)->x, 180.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(c2)->y, 150.0f, 1e-4f);

    // 无父级的根同样直接写 pos
    const uint64_t root = t.add(make_comp(0, UiLayoutKind::Leaf, 5, 6, 7, 8));
    t.resolve();
    CHECK_NEAR(t.rect_of(root)->x, 5.0f, 1e-4f);

    // 把 anchor 换成 Column：同一个子节点立刻改由布局摆位（规则翻转）
    UiLayoutNode up = *t.find(anchor);
    up.kind = UiLayoutKind::Column;
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
    UiLayoutTree t;
    UiLayoutNode col = make_comp(0, UiLayoutKind::Column, 0, 0, 300, 200);
    col.gap = 8.0f;
    const uint64_t cid = t.add(col);
    const uint64_t a = t.add(make_comp(cid, UiLayoutKind::Leaf, 999, 999, 50, 30));
    UiLayoutNode b = make_comp(cid, UiLayoutKind::Leaf, 999, 999, 70, 40);
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
    UiLayoutTree t;
    UiLayoutNode row = make_comp(0, UiLayoutKind::Row, 0, 0, 300, 100);
    const uint64_t rid = t.add(row);
    const uint64_t s = t.add(make_comp(rid, UiLayoutKind::Leaf, 0, 0, 40, 30));
    UiLayoutNode c = make_comp(rid, UiLayoutKind::Leaf, 0, 0, 40, 30);
    c.align = UiLayoutAlign::Center;
    const uint64_t cid = t.add(c);
    UiLayoutNode e = make_comp(rid, UiLayoutKind::Leaf, 0, 0, 40, 30);
    e.align = UiLayoutAlign::End;
    const uint64_t eid = t.add(e);
    UiLayoutNode st = make_comp(rid, UiLayoutKind::Leaf, 0, 0, 0, 0);
    st.align = UiLayoutAlign::Stretch;
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
    UiLayoutTree t;
    UiLayoutNode col = make_comp(0, UiLayoutKind::Column, 0, 0, 200, 300);
    const uint64_t cid = t.add(col);
    const uint64_t fixed = t.add(make_comp(cid, UiLayoutKind::Leaf, 0, 0, 100, 50));
    UiLayoutNode f1 = make_comp(cid, UiLayoutKind::Leaf, 0, 0, 100, 0);
    f1.flex = 1.0f;
    const uint64_t i1 = t.add(f1);
    UiLayoutNode f3 = make_comp(cid, UiLayoutKind::Leaf, 0, 0, 100, 0);
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
    UiLayoutTree t;
    // 容器只有 40 高，固定子节点就要 50：放不下
    UiLayoutNode col = make_comp(0, UiLayoutKind::Column, 0, 0, 100, 40);
    const uint64_t cid = t.add(col);
    const uint64_t a = t.add(make_comp(cid, UiLayoutKind::Leaf, 0, 0, 100, 50));
    UiLayoutNode b = make_comp(cid, UiLayoutKind::Leaf, 0, 0, 100, 50);
    b.flex = 1.0f;
    const uint64_t bid = t.add(b);
    t.resolve();

    // 固定项按请求高落位（溢出就溢出）；弹性项剩余钳到 0，不产生负尺寸
    CHECK_NEAR(t.rect_of(a)->h, 50.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(bid)->h, 0.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(bid)->y, 50.0f, 1e-4f);
}

TEST(comp, grid_columns_and_row_height) {
    UiLayoutTree t;
    UiLayoutNode g = make_comp(0, UiLayoutKind::Grid, 0, 0, 320, 200);
    g.cols = 3;
    g.gap = 4.0f;
    const uint64_t gid = t.add(g);
    uint64_t ids[7] = {};
    for (int i = 0; i < 7; ++i) {
        ids[i] = t.add(make_comp(gid, UiLayoutKind::Leaf, 0, 0, 0, (i % 3 == 0) ? 40.0f : 20.0f));
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
    UiLayoutTree t;
    UiLayoutNode col = make_comp(0, UiLayoutKind::Column, 50, 60, 200, 100);
    col.padding[0] = 10.0f;   // 上
    col.padding[1] = 20.0f;   // 右
    col.padding[2] = 30.0f;   // 下
    col.padding[3] = 40.0f;   // 左
    const uint64_t cid = t.add(col);
    UiLayoutNode st = make_comp(cid, UiLayoutKind::Leaf, 0, 0, 0, 25);
    st.align = UiLayoutAlign::Stretch;
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
    UiLayoutTree t;
    // 根 Column -> Row（Stretch 吃满宽）-> 两个 Leaf
    UiLayoutNode col = make_comp(0, UiLayoutKind::Column, 0, 0, 400, 300);
    col.gap = 10.0f;
    const uint64_t cid = t.add(col);
    UiLayoutNode row = make_comp(cid, UiLayoutKind::Row, 0, 0, 0, 60);
    row.align = UiLayoutAlign::Stretch;   // 交叉轴（宽）吃满
    row.gap = 6.0f;
    const uint64_t rid = t.add(row);
    const uint64_t l1 = t.add(make_comp(rid, UiLayoutKind::Leaf, 0, 0, 80, 20));
    const uint64_t l2 = t.add(make_comp(rid, UiLayoutKind::Leaf, 0, 0, 80, 20));
    t.resolve();

    CHECK_NEAR(t.rect_of(rid)->y, 0.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(rid)->w, 400.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(rid)->h, 60.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(l1)->x, 0.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(l2)->x, 86.0f, 1e-4f);   // 80 + gap 6

    // 叶子下挂布局组件：叶子不是布局组件 -> 这个 Column 直接写 pos；
    // 但它自己是布局组件 -> 它的子节点照样由它摆位
    const uint64_t host = t.add(make_comp(0, UiLayoutKind::Leaf, 500, 400, 300, 200));
    UiLayoutNode inner = make_comp(host, UiLayoutKind::Column, 10, 20, 100, 150);
    inner.gap = 5.0f;
    const uint64_t iid = t.add(inner);
    const uint64_t leaf2 = t.add(make_comp(iid, UiLayoutKind::Leaf, 0, 0, 30, 30));
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
    UiLayoutTree t;
    // 容器没有内容区（padding 吃掉全部）：子节点 rect 保持原值，不崩不写坏
    UiLayoutNode col = make_comp(0, UiLayoutKind::Column, 0, 0, 10, 10);
    for (int i = 0; i < 4; ++i) col.padding[i] = 8.0f;
    const uint64_t cid = t.add(col);
    const uint64_t a = t.add(make_comp(cid, UiLayoutKind::Leaf, 33, 44, 5, 5));
    t.resolve();
    CHECK_NEAR(t.rect_of(a)->x, 33.0f, 1e-4f);
    CHECK_NEAR(t.rect_of(a)->y, 44.0f, 1e-4f);

    // 空容器：resolve 不碰任何东西
    UiLayoutTree t2;
    const uint64_t empty = t2.add(make_comp(0, UiLayoutKind::Grid, 0, 0, 100, 100));
    t2.resolve();
    CHECK_NEAR(t2.rect_of(empty)->w, 100.0f, 1e-4f);
    (void)empty;
}

// ===========================================================================
// suite: compabi —— 布局树的 C ABI
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
// suite: layoutjson —— 布局树的 JSON 往返
// ===========================================================================

TEST(layoutjson, kind_and_align_names_roundtrip) {
    UiLayoutKind k = UiLayoutKind::Leaf;
    CHECK_TRUE(layout_kind_from_name("leaf", k));
    CHECK_EQ(static_cast<int>(k), static_cast<int>(UiLayoutKind::Leaf));
    CHECK_TRUE(layout_kind_from_name("column", k));
    CHECK_EQ(static_cast<int>(k), static_cast<int>(UiLayoutKind::Column));
    CHECK_TRUE(layout_kind_from_name("row", k));
    CHECK_EQ(static_cast<int>(k), static_cast<int>(UiLayoutKind::Row));
    CHECK_TRUE(layout_kind_from_name("grid", k));
    CHECK_EQ(static_cast<int>(k), static_cast<int>(UiLayoutKind::Grid));

    UiLayoutKind keep = UiLayoutKind::Row;
    CHECK_FALSE(layout_kind_from_name("nonsense", keep));   // 查不到不动 out
    CHECK_EQ(static_cast<int>(keep), static_cast<int>(UiLayoutKind::Row));

    CHECK_STR(layout_kind_name(UiLayoutKind::Leaf), "leaf");
    CHECK_STR(layout_kind_name(UiLayoutKind::Column), "column");
    CHECK_STR(layout_align_name(UiLayoutAlign::Stretch), "stretch");

    UiLayoutAlign a = UiLayoutAlign::Start;
    CHECK_TRUE(layout_align_from_name("stretch", a));
    CHECK_EQ(static_cast<int>(a), static_cast<int>(UiLayoutAlign::Stretch));
    CHECK_FALSE(layout_align_from_name("", a));
    CHECK_EQ(static_cast<int>(a), static_cast<int>(UiLayoutAlign::Stretch));
}

TEST(layoutjson, roundtrip_preserves_structure_and_geometry) {
    UiLayoutTree t;
    const uint64_t col = t.add(make_comp(0, UiLayoutKind::Column, 10, 20, 300, 200));
    const uint64_t row = t.add(make_comp(col, UiLayoutKind::Row, 0, 0, 100, 40));
    const uint64_t a   = t.add(make_comp(row, UiLayoutKind::Leaf, 0, 0, 50, 30));
    const uint64_t b   = t.add(make_comp(row, UiLayoutKind::Leaf, 0, 0, 60, 20));
    CHECK_EQ(t.count(), 4);

    // 给容器侧 / 子侧参数都填上非默认值：往返要一个不丢
    UiLayoutNode* n = t.find(row);
    CHECK_TRUE(n != nullptr);
    n->gap = 6.0f;
    n->padding[0] = 1.0f; n->padding[1] = 2.0f; n->padding[2] = 3.0f; n->padding[3] = 4.0f;
    n->cols = 3;
    UiLayoutNode* la = t.find(a);
    CHECK_TRUE(la != nullptr);
    la->flex = 2.0f;
    la->align = UiLayoutAlign::Stretch;
    la->margin[0] = 5.0f; la->margin[1] = 6.0f; la->margin[2] = 7.0f; la->margin[3] = 8.0f;

    const nlohmann::json j = layout_tree_to_json(t);
    CHECK_EQ(j.at("version").get<int>(), 1);
    CHECK_EQ(j.at("nodes").size(), 4u);
    // id 走十进制字符串（JS 只有 53 位整数精度）
    CHECK_TRUE(j.at("nodes").at(0).at("id").is_string());

    UiLayoutTree back;
    CHECK_TRUE(layout_tree_from_json(j, back));
    CHECK_EQ(back.count(), 4);

    // 结构一致：父子关系与插入顺序
    CHECK_EQ(back.child_count(back.node_at(0)->id), 1);
    CHECK_EQ(back.parent_of(back.node_at(1)->id), back.node_at(0)->id);
    CHECK_EQ(back.parent_of(back.node_at(2)->id), back.node_at(1)->id);
    CHECK_EQ(back.parent_of(back.node_at(3)->id), back.node_at(1)->id);

    // 几何与参数一致
    for (int i = 0; i < t.count(); ++i) {
        const UiLayoutNode* src = t.node_at(i);
        const UiLayoutNode* dst = back.node_at(i);
        CHECK_TRUE(src != nullptr && dst != nullptr);
        CHECK_EQ(static_cast<int>(dst->kind), static_cast<int>(src->kind));
        CHECK_EQ(static_cast<int>(dst->align), static_cast<int>(src->align));
        CHECK_EQ(dst->parent != 0, src->parent != 0);
        CHECK_NEAR(dst->rect.x, src->rect.x, 1e-5f);
        CHECK_NEAR(dst->rect.y, src->rect.y, 1e-5f);
        CHECK_NEAR(dst->rect.w, src->rect.w, 1e-5f);
        CHECK_NEAR(dst->rect.h, src->rect.h, 1e-5f);
        CHECK_NEAR(dst->flex, src->flex, 1e-5f);
        CHECK_NEAR(dst->gap, src->gap, 1e-5f);
        CHECK_EQ(dst->cols, src->cols);
        for (int m = 0; m < 4; ++m) {
            CHECK_NEAR(dst->margin[m], src->margin[m], 1e-5f);
            CHECK_NEAR(dst->padding[m], src->padding[m], 1e-5f);
        }
    }

    // 解析结果也一致（resolve 是幂等的，同结构同输入 → 同矩形）
    t.resolve();
    back.resolve();
    for (int i = 0; i < t.count(); ++i) {
        const gryce_ui::Rect* rs = t.rect_of(t.node_at(i)->id);
        const gryce_ui::Rect* rd = back.rect_of(back.node_at(i)->id);
        CHECK_TRUE(rs != nullptr && rd != nullptr);
        CHECK_NEAR(rd->x, rs->x, 1e-4f);
        CHECK_NEAR(rd->y, rs->y, 1e-4f);
        CHECK_NEAR(rd->w, rs->w, 1e-4f);
        CHECK_NEAR(rd->h, rs->h, 1e-4f);
    }
}

TEST(layoutjson, dump_is_a_valid_document_and_ids_are_strings) {
    UiLayoutTree t;
    t.add(make_comp(0, UiLayoutKind::Column, 0, 0, 100, 100));
    t.add(make_comp(1, UiLayoutKind::Leaf, 0, 0, 10, 10));   // parent = 上面那个 id

    const std::string text = layout_tree_to_json(t).dump();
    CHECK_TRUE(text.find("\"version\"") != std::string::npos);
    CHECK_TRUE(text.find("\"nodes\"") != std::string::npos);
    // id / parent 必须是 JSON **字符串**（不是数字）：JS 的 JSON.parse 只有 53
    // 位整数精度，数字形式过一轮就丢低位。这里不依赖 nlohmann 的具体间距
    // （dump 在冒号后带一个空格），只看 key 之后第一个非空白字符是不是引号。
    auto value_is_string = [](const std::string& s, const char* key) {
        const std::size_t p = s.find(key);
        if (p == std::string::npos) return false;
        const std::size_t c = s.find(':', p);
        if (c == std::string::npos) return false;
        std::size_t q = c + 1;
        while (q < s.size() && (s[q] == ' ' || s[q] == '\t' || s[q] == '\n' || s[q] == '\r')) ++q;
        return q < s.size() && s[q] == '"';
    };
    CHECK_TRUE(value_is_string(text, "\"id\""));
    CHECK_TRUE(value_is_string(text, "\"parent\""));

    // 文本再解析回来，一个节点不少
    UiLayoutTree back;
    CHECK_TRUE(layout_tree_from_json(nlohmann::json::parse(text), back));
    CHECK_EQ(back.count(), 2);
}

TEST(layoutjson, from_json_is_lenient_but_rejects_garbage) {
    // 顶层不是对象 / 没有 nodes 数组：false，且不动目标树
    UiLayoutTree t;
    CHECK_FALSE(layout_tree_from_json(nlohmann::json::array(), t));
    CHECK_FALSE(layout_tree_from_json(nlohmann::json::object(), t));
    CHECK_FALSE(layout_tree_from_json(nlohmann::json::parse("{\"nodes\": 5}"), t));
    CHECK_EQ(t.count(), 0);

    // 缺字段用默认值；未知 kind / align 名按 Leaf / Start；坏条目跳过
    const nlohmann::json j = nlohmann::json::parse(R"({
        "nodes": [
            { "parent": "0", "kind": "column", "rect": [0, 0, 100, 100] },
            { "parent": "0" },
            { "parent": "0", "kind": "wat", "align": "sideways" },
            "not an object",
            42
        ]
    })");
    CHECK_TRUE(layout_tree_from_json(j, t));
    CHECK_EQ(t.count(), 3);                       // 后两条坏条目被跳过
    CHECK_EQ(static_cast<int>(t.node_at(0)->kind), static_cast<int>(UiLayoutKind::Column));
    CHECK_EQ(static_cast<int>(t.node_at(1)->kind), static_cast<int>(UiLayoutKind::Leaf));
    CHECK_EQ(static_cast<int>(t.node_at(2)->kind), static_cast<int>(UiLayoutKind::Leaf));
    CHECK_EQ(static_cast<int>(t.node_at(2)->align), static_cast<int>(UiLayoutAlign::Start));

    // 读回会先 clear：旧内容不残留
    CHECK_TRUE(layout_tree_from_json(nlohmann::json::parse("{\"nodes\": []}"), t));
    CHECK_EQ(t.count(), 0);
}

TEST(layoutjson, node_at_is_topological_and_bounded) {
    UiLayoutTree t;
    const uint64_t root = t.add(make_comp(0, UiLayoutKind::Column, 0, 0, 10, 10));
    t.add(make_comp(root, UiLayoutKind::Leaf, 0, 0, 1, 1));
    t.add(make_comp(root, UiLayoutKind::Leaf, 0, 0, 2, 2));

    CHECK_TRUE(t.node_at(-1) == nullptr);
    CHECK_TRUE(t.node_at(t.count()) == nullptr);
    // 父先于子：第 0 个是根，它的 children 都在后面
    CHECK_EQ(t.node_at(0)->id, root);
    CHECK_EQ(t.node_at(0)->parent, 0u);
    CHECK_EQ(t.node_at(1)->parent, root);
    CHECK_EQ(t.node_at(2)->parent, root);

    // 移除中间一层：node_at 只数活节点
    t.remove(root);
    CHECK_EQ(t.count(), 0);
    CHECK_TRUE(t.node_at(0) == nullptr);
}

// ===========================================================================
// suite: drawlist —— UI 自绘命令缓冲（内置 UI 渲染路径的模型层）
// ===========================================================================

TEST(drawlist, empty_and_reset) {
    gryce_ui::UiDrawList dl;
    CHECK_EQ(dl.command_count(), 0);
    CHECK_EQ(dl.clip_depth(), 0);
    CHECK_EQ(dl.dropped(), 0u);
    CHECK_EQ(dl.culled(), 0u);

    dl.add_rect(0, 0, 10, 10, 0xFF0000FFu);
    dl.push_clip(0, 0, 5, 5);
    dl.add_text(1, 1, "hi", 12.0f, 0xFFFFFFFFu);
    CHECK_EQ(dl.command_count(), 3);
    CHECK_EQ(dl.clip_depth(), 1);

    dl.reset();
    CHECK_EQ(dl.command_count(), 0);
    CHECK_EQ(dl.clip_depth(), 0);
    CHECK_EQ(dl.dropped(), 0u);
    CHECK_EQ(dl.culled(), 0u);
}

TEST(drawlist, command_order_and_fields) {
    gryce_ui::UiDrawList dl;
    dl.add_rect(10, 20, 30, 40, 0x11223344u);
    dl.add_text(1, 2, "ok", 16.0f, 0xAABBCCDDu);
    dl.add_sprite(5, 6, 7, 8, 42u, 0.1f, 0.2f, 0.3f, 0.4f, 0x01020304u);

    CHECK_EQ(dl.command_count(), 3);
    CHECK_EQ(dl.command(0).cmd, gryce_ui::UiCmd::Rect);
    CHECK_EQ(dl.command(0).x, 10.0f);
    CHECK_EQ(dl.command(0).w, 30.0f);
    CHECK_EQ(dl.command(0).color, 0x11223344u);

    CHECK_EQ(dl.command(1).cmd, gryce_ui::UiCmd::Text);
    CHECK_EQ(dl.command(1).font_size, 16.0f);
    CHECK_EQ(dl.command(1).color, 0xAABBCCDDu);
    CHECK_TRUE(std::strcmp(dl.text_at(dl.command(1).text_off), "ok") == 0);
    CHECK_EQ(dl.command(1).text_len, 2u);

    CHECK_EQ(dl.command(2).cmd, gryce_ui::UiCmd::Sprite);
    CHECK_EQ(dl.command(2).texture, 42u);
    CHECK_NEAR(dl.command(2).u0, 0.1f, 1e-6f);
    CHECK_NEAR(dl.command(2).v1, 0.4f, 1e-6f);
}

TEST(drawlist, text_pool_roundtrip_multiple) {
    gryce_ui::UiDrawList dl;
    dl.add_text(0, 0, "alpha", 12.0f, 0xFFFFFFFFu);
    dl.add_text(0, 20, "beta", 12.0f, 0xFFFFFFFFu);
    dl.add_text(0, 40, "", 12.0f, 0xFFFFFFFFu);   // 空串不产生命令
    dl.add_text(0, 60, nullptr, 12.0f, 0xFFFFFFFFu);
    CHECK_EQ(dl.command_count(), 2);
    CHECK_TRUE(std::strcmp(dl.text_at(dl.command(0).text_off), "alpha") == 0);
    CHECK_TRUE(std::strcmp(dl.text_at(dl.command(1).text_off), "beta") == 0);
    // 两条文本在池中互不重叠（偏移不同）
    CHECK_TRUE(dl.command(0).text_off != dl.command(1).text_off);
}

TEST(drawlist, push_clip_carries_effective_rect) {
    // PushClip 命令自带与父裁剪相交后的生效矩形：命令流因此是自描述的，
    // 渲染桥读命令时不必回查构建期的裁剪栈（见 ui_drawlist.h 的 UiCommand 注释）
    gryce_ui::UiDrawList dl;
    dl.push_clip(0, 0, 100, 100);
    dl.push_clip(50, 50, 100, 100);               // 交集 (50,50,50,50)
    const gryce_ui::UiCommand& c = dl.command(1);
    CHECK_EQ(c.cmd, gryce_ui::UiCmd::PushClip);
    CHECK_EQ(c.clip, 2u);                         // 记压入后的深度
    CHECK_NEAR(c.x, 50.0f, 1e-6f);
    CHECK_NEAR(c.y, 50.0f, 1e-6f);
    CHECK_NEAR(c.w, 50.0f, 1e-6f);
    CHECK_NEAR(c.h, 50.0f, 1e-6f);

    dl.push_clip(500, 500, 10, 10);               // 不相交 -> 空矩形也照记
    const gryce_ui::UiCommand& e = dl.command(2);
    CHECK_EQ(e.cmd, gryce_ui::UiCmd::PushClip);
    CHECK_TRUE(e.w <= 0.0f);                      // 空裁剪：后续绘制被剔除
    CHECK_TRUE(e.h <= 0.0f);
}

TEST(drawlist, clip_depth_tagged_on_commands) {
    gryce_ui::UiDrawList dl;
    dl.add_rect(0, 0, 10, 10, 0xFFFFFFFFu);       // clip = 0
    dl.push_clip(0, 0, 100, 100);
    dl.add_rect(0, 0, 10, 10, 0xFFFFFFFFu);       // clip = 1
    dl.push_clip(0, 0, 50, 50);
    dl.add_rect(0, 0, 10, 10, 0xFFFFFFFFu);       // clip = 2
    CHECK_EQ(dl.command(0).clip, 0u);
    CHECK_EQ(dl.command(1).clip, 1u);             // PushClip 记"执行后"深度
    CHECK_EQ(dl.command(2).clip, 1u);
    CHECK_EQ(dl.command(3).clip, 2u);
    CHECK_EQ(dl.command(4).clip, 2u);
}

TEST(drawlist, clip_intersects_parent) {
    gryce_ui::UiDrawList dl;
    dl.push_clip(0, 0, 100, 100);
    dl.push_clip(50, 50, 100, 100);               // 与父相交 -> (50,50,50,50)
    const gryce_ui::UiClipRect& c = dl.clip_at(2);
    CHECK_NEAR(c.x, 50.0f, 1e-6f);
    CHECK_NEAR(c.y, 50.0f, 1e-6f);
    CHECK_NEAR(c.w, 50.0f, 1e-6f);
    CHECK_NEAR(c.h, 50.0f, 1e-6f);

    dl.push_clip(200, 200, 10, 10);               // 与父不相交 -> 空
    const gryce_ui::UiClipRect& e = dl.clip_at(3);
    CHECK_TRUE(e.w <= 0.0f);
    CHECK_TRUE(e.h <= 0.0f);
}

TEST(drawlist, empty_clip_culls_draw_commands) {
    gryce_ui::UiDrawList dl;
    dl.push_clip(0, 0, 100, 100);
    dl.push_clip(500, 500, 10, 10);               // 不相交 -> 剔除态
    dl.add_rect(0, 0, 10, 10, 0xFFFFFFFFu);       // 被剔除
    dl.add_text(1, 1, "x", 12.0f, 0xFFFFFFFFu);   // 被剔除
    dl.add_sprite(1, 1, 4, 4, 7u, 0, 0, 1, 1, 0xFFFFFFFFu);
    CHECK_EQ(dl.culled(), 3u);
    CHECK_EQ(dl.command_count(), 2);              // 只剩两条 PushClip

    dl.pop_clip();                                // 回到父裁剪（非空）
    dl.add_rect(0, 0, 10, 10, 0xFFFFFFFFu);       // 恢复可见
    CHECK_EQ(dl.command_count(), 4);
    CHECK_EQ(dl.culled(), 3u);
    CHECK_EQ(dl.command(2).cmd, gryce_ui::UiCmd::PopClip);
    CHECK_EQ(dl.command(2).clip, 1u);             // pop 先减深度再记录
    CHECK_EQ(dl.command(3).cmd, gryce_ui::UiCmd::Rect);
    CHECK_EQ(dl.command(3).clip, 1u);             // 恢复后命令带父深度
}

TEST(drawlist, pop_clip_restores_and_records) {
    gryce_ui::UiDrawList dl;
    dl.push_clip(0, 0, 100, 100);                 // depth 1
    dl.push_clip(0, 0, 50, 50);                   // depth 2
    dl.pop_clip();                                // depth 1
    CHECK_EQ(dl.clip_depth(), 1);
    const gryce_ui::UiClipRect& c = dl.clip_at(1);
    CHECK_NEAR(c.w, 100.0f, 1e-6f);               // 父裁剪完好
    dl.add_rect(0, 0, 10, 10, 0xFFFFFFFFu);
    CHECK_EQ(dl.command_count(), 4);
    CHECK_EQ(dl.command(2).cmd, gryce_ui::UiCmd::PopClip);
    CHECK_EQ(dl.command(2).clip, 1u);             // pop 后 depth=1
    CHECK_EQ(dl.command(3).clip, 1u);             // 后续命令带 depth 1
}

TEST(drawlist, pop_on_empty_stack_is_noop) {
    gryce_ui::UiDrawList dl;
    dl.pop_clip();
    dl.pop_clip();
    CHECK_EQ(dl.clip_depth(), 0);
    CHECK_EQ(dl.command_count(), 0);
    CHECK_EQ(dl.dropped(), 0u);
    dl.add_rect(0, 0, 1, 1, 0xFFFFFFFFu);         // 仍然可用
    CHECK_EQ(dl.command_count(), 1);
}

TEST(drawlist, nested_clip_three_levels) {
    gryce_ui::UiDrawList dl;
    dl.push_clip(0, 0, 300, 300);
    dl.push_clip(100, 100, 300, 300);             // -> (100,100,200,200)
    dl.push_clip(150, 150, 300, 300);             // -> (150,150,150,150)
    const gryce_ui::UiClipRect& c = dl.clip_at(3);
    CHECK_NEAR(c.x, 150.0f, 1e-6f);
    CHECK_NEAR(c.y, 150.0f, 1e-6f);
    CHECK_NEAR(c.w, 150.0f, 1e-6f);
    CHECK_NEAR(c.h, 150.0f, 1e-6f);
    dl.pop_clip();
    const gryce_ui::UiClipRect& p = dl.clip_at(2);
    CHECK_NEAR(p.x, 100.0f, 1e-6f);
    CHECK_NEAR(p.w, 200.0f, 1e-6f);
}

TEST(drawlist, clip_stack_overflow_rejected) {
    gryce_ui::UiDrawList dl;
    for (int i = 0; i < gryce_ui::UiDrawList::kMaxClips; ++i) {
        dl.push_clip(0, 0, 100, 100);
    }
    CHECK_EQ(dl.clip_depth(), gryce_ui::UiDrawList::kMaxClips);
    dl.push_clip(0, 0, 10, 10);                   // 栈满：拒绝
    CHECK_EQ(dl.clip_depth(), gryce_ui::UiDrawList::kMaxClips);
    CHECK_TRUE(dl.dropped() >= 1u);
    // pop 仍然按 depth 弹，不会失衡
    dl.pop_clip();
    CHECK_EQ(dl.clip_depth(), gryce_ui::UiDrawList::kMaxClips - 1);
    dl.add_rect(0, 0, 5, 5, 0xFFFFFFFFu);
    CHECK_EQ(dl.command_count(), gryce_ui::UiDrawList::kMaxClips + 1 + 1);
}

TEST(drawlist, command_capacity_drops_and_counts) {
    gryce_ui::UiDrawList dl;
    for (int i = 0; i < gryce_ui::UiDrawList::kMaxCommands + 8; ++i) {
        dl.add_rect(0, 0, 1, 1, 0xFFFFFFFFu);
    }
    CHECK_EQ(dl.command_count(), gryce_ui::UiDrawList::kMaxCommands);
    CHECK_EQ(dl.dropped(), 8u);
    // 丢弃不影响已记录命令
    CHECK_EQ(dl.command(0).cmd, gryce_ui::UiCmd::Rect);
}

TEST(drawlist, text_pool_overflow_drops_whole_entry) {
    gryce_ui::UiDrawList dl;
    // 第一条吃掉几乎整个池子（余量 < 10 字节）
    const std::string big(static_cast<size_t>(gryce_ui::UiDrawList::kTextPoolSize) - 4, 'a');
    dl.add_text(0, 0, big.c_str(), 12.0f, 0xFFFFFFFFu);
    CHECK_EQ(dl.command_count(), 1);
    // 第二条放不下：整条丢弃（不截断）
    dl.add_text(0, 0, "overflow", 12.0f, 0xFFFFFFFFu);
    CHECK_EQ(dl.command_count(), 1);
    CHECK_TRUE(dl.dropped() >= 1u);
}

TEST(drawlist, reset_clears_clip_stack) {
    gryce_ui::UiDrawList dl;
    dl.push_clip(0, 0, 100, 100);
    dl.push_clip(500, 500, 10, 10);               // 空裁剪
    dl.reset();
    CHECK_EQ(dl.clip_depth(), 0);
    dl.add_rect(0, 0, 10, 10, 0xFFFFFFFFu);       // 不再被剔除
    CHECK_EQ(dl.command_count(), 1);
    CHECK_EQ(dl.culled(), 0u);
}

TEST(drawlist, negative_geometry_kept_verbatim) {
    gryce_ui::UiDrawList dl;
    dl.add_rect(-5, -5, -10, -10, 0xFFFFFFFFu);   // 布局层的锅，drawlist 不规范化
    CHECK_EQ(dl.command_count(), 1);
    CHECK_EQ(dl.command(0).x, -5.0f);
    CHECK_EQ(dl.command(0).w, -10.0f);
}

TEST(drawlist, clip_commands_survive_cull_state) {
    gryce_ui::UiDrawList dl;
    dl.push_clip(0, 0, 10, 10);
    dl.push_clip(500, 0, 10, 10);                 // 不相交
    CHECK_TRUE(dl.clip_at(2).w <= 0.0f);
    dl.push_clip(0, 0, 100, 100);                 // 与空裁剪相交仍为空
    CHECK_TRUE(dl.clip_at(3).w <= 0.0f);
    dl.pop_clip();
    dl.pop_clip();
    CHECK_EQ(dl.clip_depth(), 1);
    const gryce_ui::UiClipRect& c = dl.clip_at(1);
    CHECK_NEAR(c.w, 10.0f, 1e-6f);                // 恢复的是父级，不是被pop的
    dl.add_rect(0, 0, 5, 5, 0xFFFFFFFFu);
    CHECK_EQ(dl.culled(), 0u);                    // 父级非空，恢复可见
}

// ===========================================================================
// suite: bridge —— 渲染桥（drawlist → UiRenderer）
//
// mock 渲染器记录调用序列：不碰 GPU、不建窗口，因此"桥的行为"（顺序、裁剪
// 切换时机、剔除、颜色/文本原样透传）可以在 CI 里毫秒级断言。真正的
// IRenderer2D 适配器（ui_render_bridge_rd.cpp）不编进本 exe——它只做方言
// 翻译，翻译规则由这里的契约钉死。
// ===========================================================================

namespace {

struct MockCall {
    enum Kind { Rect, Text, Sprite, Scissor, ResetScissor,
                Round, RoundGradient, RoundBorder, Shadow };
    Kind        kind = Rect;
    float       x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    uint32_t    color = 0;
    uint32_t    color2 = 0;                 // 渐变下端色
    uint64_t    texture = 0;
    float       u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    float       font_size = 0.0f;
    float       radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float       border_w = 0.0f;            // 描边线宽 / 阴影扩散半径
    std::string text;
};

class MockRenderer final : public gryce_ui::UiRenderer {
public:
    std::vector<MockCall> calls;

    int count_of(MockCall::Kind k) const {
        int n = 0;
        for (const MockCall& c : calls) if (c.kind == k) ++n;
        return n;
    }

    void ui_draw_rect(float x, float y, float w, float h, uint32_t rgba) override {
        MockCall c; c.kind = MockCall::Rect; c.x = x; c.y = y; c.w = w; c.h = h; c.color = rgba;
        calls.push_back(c);
    }
    void ui_draw_text(float x, float y, const char* text, float font_size,
                      uint32_t rgba) override {
        MockCall c; c.kind = MockCall::Text; c.x = x; c.y = y; c.font_size = font_size;
        c.color = rgba; c.text = text ? text : "";
        calls.push_back(c);
    }
    void ui_draw_sprite(float x, float y, float w, float h, uint64_t texture,
                        float u0, float v0, float u1, float v1, uint32_t rgba) override {
        MockCall c; c.kind = MockCall::Sprite; c.x = x; c.y = y; c.w = w; c.h = h;
        c.texture = texture; c.u0 = u0; c.v0 = v0; c.u1 = u1; c.v1 = v1; c.color = rgba;
        calls.push_back(c);
    }
    void ui_draw_round_rect(float x, float y, float w, float h,
                            const float radii[4], uint32_t rgba) override {
        MockCall c; c.kind = MockCall::Round; c.x = x; c.y = y; c.w = w; c.h = h;
        c.color = rgba;
        for (int i = 0; i < 4; ++i) c.radii[i] = radii[i];
        calls.push_back(c);
    }
    void ui_draw_round_gradient(float x, float y, float w, float h,
                                const float radii[4], uint32_t top,
                                uint32_t bottom) override {
        MockCall c; c.kind = MockCall::RoundGradient; c.x = x; c.y = y; c.w = w; c.h = h;
        c.color = top; c.color2 = bottom;
        for (int i = 0; i < 4; ++i) c.radii[i] = radii[i];
        calls.push_back(c);
    }
    void ui_draw_round_border(float x, float y, float w, float h,
                              const float radii[4], float thickness,
                              uint32_t rgba) override {
        MockCall c; c.kind = MockCall::RoundBorder; c.x = x; c.y = y; c.w = w; c.h = h;
        c.color = rgba; c.border_w = thickness;
        for (int i = 0; i < 4; ++i) c.radii[i] = radii[i];
        calls.push_back(c);
    }
    void ui_draw_shadow(float x, float y, float w, float h, const float radii[4],
                        float spread, uint32_t rgba) override {
        MockCall c; c.kind = MockCall::Shadow; c.x = x; c.y = y; c.w = w; c.h = h;
        c.color = rgba; c.border_w = spread;
        for (int i = 0; i < 4; ++i) c.radii[i] = radii[i];
        calls.push_back(c);
    }
    void ui_set_scissor(float x, float y, float w, float h) override {
        MockCall c; c.kind = MockCall::Scissor; c.x = x; c.y = y; c.w = w; c.h = h;
        calls.push_back(c);
    }
    void ui_reset_scissor() override {
        MockCall c; c.kind = MockCall::ResetScissor;
        calls.push_back(c);
    }
};

} // namespace

TEST(bridge, empty_list_no_calls) {
    gryce_ui::UiDrawList dl;
    MockRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);
    CHECK_TRUE(r.calls.empty());
    CHECK_EQ(bridge.draw_commands(), 0);
    CHECK_EQ(bridge.scissor_switches(), 0);
}

TEST(bridge, commands_pass_through_in_order) {
    gryce_ui::UiDrawList dl;
    dl.add_rect(10, 20, 30, 40, 0x11223344u);
    dl.add_text(1, 2, "hello", 13.0f, 0xAABBCCDDu);
    dl.add_sprite(5, 6, 7, 8, 42u, 0.1f, 0.2f, 0.3f, 0.4f, 0x01020304u);

    MockRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);
    CHECK_EQ(r.calls.size(), 4u);   // 首条命令前的强制同步 + 三条绘制
    CHECK_EQ(bridge.draw_commands(), 3);
    CHECK_EQ(bridge.scissor_switches(), 1);   // 首条命令强制同步一次（reset）

    CHECK_TRUE(r.calls[0].kind == MockCall::ResetScissor);  // clip=0 → 复位
    CHECK_TRUE(r.calls[1].kind == MockCall::Rect);
    CHECK_NEAR(r.calls[1].x, 10.0f, 1e-6f);
    CHECK_NEAR(r.calls[1].w, 30.0f, 1e-6f);
    CHECK_EQ(r.calls[1].color, 0x11223344u);

    CHECK_TRUE(r.calls[2].kind == MockCall::Text);
    CHECK_STR(r.calls[2].text, "hello");
    CHECK_NEAR(r.calls[2].font_size, 13.0f, 1e-6f);
    CHECK_EQ(r.calls[2].color, 0xAABBCCDDu);
}

TEST(bridge, sprite_fields_forwarded) {
    gryce_ui::UiDrawList dl;
    dl.add_sprite(5, 6, 7, 8, 0xDEADBEEFu, 0.25f, 0.5f, 0.75f, 1.0f, 0xFF00FF00u);
    MockRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);
    CHECK_EQ(r.count_of(MockCall::Sprite), 1);
    const MockCall& s = r.calls.back();
    CHECK_EQ(s.texture, 0xDEADBEEFull);
    CHECK_NEAR(s.u0, 0.25f, 1e-6f);
    CHECK_NEAR(s.v1, 1.0f, 1e-6f);
    CHECK_EQ(s.color, 0xFF00FF00u);
}

TEST(bridge, scissor_switches_only_on_depth_change) {
    gryce_ui::UiDrawList dl;
    dl.add_rect(0, 0, 4, 4, 0xFFFFFFFFu);        // clip=0
    dl.push_clip(10, 10, 50, 60);               // clip=1
    dl.add_rect(0, 0, 4, 4, 0xFFFFFFFFu);        // clip=1：不切
    dl.add_rect(1, 1, 4, 4, 0xFFFFFFFFu);        // clip=1：不切
    dl.pop_clip();                              // clip=0
    dl.add_rect(2, 2, 4, 4, 0xFFFFFFFFu);        // clip=0：reset

    MockRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);
    // 序列：reset(首条) rect set(1) rect rect reset rect
    CHECK_EQ(r.count_of(MockCall::Scissor), 1);
    CHECK_EQ(r.count_of(MockCall::ResetScissor), 2);
    CHECK_EQ(r.count_of(MockCall::Rect), 4);
    CHECK_EQ(bridge.scissor_switches(), 3);
    // 序列：reset(首条同步) rect set(1) rect rect reset rect
    const MockCall& sc = r.calls[2];
    CHECK_TRUE(sc.kind == MockCall::Scissor);
    CHECK_NEAR(sc.x, 10.0f, 1e-6f);
    CHECK_NEAR(sc.h, 60.0f, 1e-6f);
}

TEST(bridge, nested_clip_uses_intersected_rects) {
    gryce_ui::UiDrawList dl;
    dl.push_clip(0, 0, 100, 100);               // 1: (0,0,100,100)
    dl.push_clip(50, 50, 100, 100);             // 2: 交集 (50,50,50,50)
    dl.add_rect(0, 0, 1, 1, 0xFFFFFFFFu);        // clip=2
    dl.pop_clip();                              // 回 1
    dl.add_rect(0, 0, 1, 1, 0xFFFFFFFFu);        // clip=1

    MockRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);
    // set(100x100) set(50x50) rect set(100x100) rect
    CHECK_EQ(r.count_of(MockCall::Scissor), 3);
    CHECK_EQ(r.count_of(MockCall::Rect), 2);
    CHECK_TRUE(r.calls[0].kind == MockCall::Scissor);
    CHECK_NEAR(r.calls[0].w, 100.0f, 1e-6f);
    CHECK_TRUE(r.calls[1].kind == MockCall::Scissor);
    CHECK_NEAR(r.calls[1].x, 50.0f, 1e-6f);     // 交集，不是请求的 50,50,100,100
    CHECK_NEAR(r.calls[1].w, 50.0f, 1e-6f);
    CHECK_TRUE(r.calls[2].kind == MockCall::Rect);
    CHECK_TRUE(r.calls[3].kind == MockCall::Scissor);
    CHECK_NEAR(r.calls[3].w, 100.0f, 1e-6f);    // pop 后恢复父级
    CHECK_TRUE(r.calls[4].kind == MockCall::Rect);
}

TEST(bridge, pop_restores_parent_before_next_draw) {
    gryce_ui::UiDrawList dl;
    dl.push_clip(0, 0, 100, 100);               // 1
    dl.push_clip(50, 50, 100, 100);             // 2: 交集 (50,50,50,50)
    dl.add_rect(0, 0, 1, 1, 0xFFFFFFFFu);        // clip=2
    dl.pop_clip();                              // 命令记 clip=1
    dl.add_rect(0, 0, 1, 1, 0xFFFFFFFFu);        // clip=1：先恢复到父级再画

    MockRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);
    // set(1) set(2) rect set(1) rect：pop 之后、下一条绘制之前必须恢复父级
    CHECK_EQ(r.count_of(MockCall::Scissor), 3);
    CHECK_TRUE(r.calls[2].kind == MockCall::Rect);
    CHECK_TRUE(r.calls[3].kind == MockCall::Scissor);
    CHECK_NEAR(r.calls[3].w, 100.0f, 1e-6f);    // 父级矩形，不是被 pop 的 50x50
    CHECK_TRUE(r.calls[4].kind == MockCall::Rect);
}

TEST(bridge, culled_commands_never_reach_renderer) {
    gryce_ui::UiDrawList dl;
    dl.push_clip(0, 0, 10, 10);
    dl.push_clip(500, 0, 10, 10);               // 不相交 → 剔除态
    dl.add_rect(0, 0, 5, 5, 0xFFFFFFFFu);        // 被裁掉，不进命令表
    dl.add_text(0, 0, "gone", 12.0f, 0xFFFFFFFFu);
    dl.pop_clip();
    dl.add_rect(0, 0, 5, 5, 0xFFFFFFFFu);        // 恢复可见

    MockRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);
    CHECK_EQ(r.count_of(MockCall::Rect), 1);
    CHECK_EQ(r.count_of(MockCall::Text), 0);
    CHECK_EQ(dl.culled(), 2u);                  // 丢弃发生在 drawlist 侧
    CHECK_EQ(bridge.draw_commands(), 1);
}

TEST(bridge, rerender_resyncs_scissor_state) {
    // 调用方可能在两次 render 之间动过渲染器的 scissor（比如 begin_frame
    // 复位），桥不能假设上一轮的结论还有效：每轮开头强制同步一次。
    gryce_ui::UiDrawList dl;
    dl.push_clip(0, 0, 40, 40);
    dl.add_rect(0, 0, 1, 1, 0xFFFFFFFFu);
    dl.pop_clip();

    MockRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);
    const int first = r.count_of(MockCall::Scissor);
    r.calls.clear();
    bridge.render(dl, r);
    CHECK_EQ(r.count_of(MockCall::Scissor), first);   // 第二轮同样设了裁剪
    CHECK_EQ(r.count_of(MockCall::ResetScissor), 1);
}

TEST(bridge, full_clip_stack_still_draws_at_max_depth) {
    // 裁剪栈满时 push 被拒绝、命令不进表；后续绘制命令带 clip = 容量深度，
    // clips_[kMaxClips] 由最后一次成功的 push 写好——桥必须按它设裁剪，
    // 不能因为"超过当前深度"就复位（render 时栈已弹回 0）。
    // 全部推同一个矩形：交集仍是它自己，每一层都非空。
    gryce_ui::UiDrawList dl;
    for (int i = 0; i < gryce_ui::UiDrawList::kMaxClips; ++i) {
        dl.push_clip(0, 0, 100, 100);
    }
    CHECK_EQ(dl.clip_depth(), gryce_ui::UiDrawList::kMaxClips);
    dl.push_clip(0, 0, 5, 5);                   // 拒绝：栈满
    CHECK_EQ(dl.clip_depth(), gryce_ui::UiDrawList::kMaxClips);
    dl.add_rect(0, 0, 2, 2, 0xFFFFFFFFu);        // clip = kMaxClips，非空裁剪
    CHECK_EQ(dl.culled(), 0u);
    dl.pop_clip();                              // 弹回容量-1

    MockRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);
    CHECK_EQ(r.count_of(MockCall::Rect), 1);
    CHECK_TRUE(r.count_of(MockCall::Scissor) >= 1);
    const MockCall& sc = r.calls.front();
    CHECK_TRUE(sc.kind == MockCall::Scissor);
    CHECK_NEAR(sc.x, 0.0f, 1e-6f);
    CHECK_NEAR(sc.w, 100.0f, 1e-6f);
    CHECK_EQ(bridge.draw_commands(), 1);
}

TEST(bridge, text_pool_string_forwarded_verbatim) {
    gryce_ui::UiDrawList dl;
    dl.add_text(0, 0, "first", 10.0f, 0xFFFFFFFFu);
    dl.add_text(0, 20, "second", 10.0f, 0xFFFFFFFFu);
    dl.add_text(0, 40, "third", 10.0f, 0xFFFFFFFFu);
    MockRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);
    CHECK_EQ(r.count_of(MockCall::Text), 3);
    CHECK_STR(r.calls[1].text, "first");     // calls[0] 是首条命令前的同步
    CHECK_STR(r.calls[2].text, "second");
    CHECK_STR(r.calls[3].text, "third");
    CHECK_NEAR(r.calls[3].y, 40.0f, 1e-6f);
}

// 逻辑字号 → 物理字号必须取整：字形图集按整数像素烘一档，绘制尺寸与那一档
// 完全相等才是 1:1 采样（渲染器这时还会把字形原点对齐到像素格）。13×1.5=19.5
// 直接画就是"半像素落点 + 半像素缩放"，笔画立刻发虚。
TEST(bridge, physical_text_size_rounds_to_whole_pixels) {
    CHECK_NEAR(gryce_ui::ui_text_physical_size(13.0f, 1.0f), 13.0f, 1e-6f);
    CHECK_NEAR(gryce_ui::ui_text_physical_size(13.0f, 1.5f), 20.0f, 1e-6f);
    CHECK_NEAR(gryce_ui::ui_text_physical_size(13.0f, 2.0f), 26.0f, 1e-6f);
    CHECK_NEAR(gryce_ui::ui_text_physical_size(13.0f, 1.25f), 16.0f, 1e-6f);
    // scale 非法（0/负数）按 1 处理：不能让整屏文字塌成 0 号
    CHECK_NEAR(gryce_ui::ui_text_physical_size(13.0f, 0.0f), 13.0f, 1e-6f);
    // 结果恒为整数（这就是"能 1:1"的判据）
    for (float s : {1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 3.0f}) {
        const float px = gryce_ui::ui_text_physical_size(13.0f, s);
        CHECK_NEAR(px, std::floor(px), 1e-6f);
    }
}

// ===========================================================================
// suite: widget —— retained 控件层（命中测试 / 事件 / 值 / 绘制）
//
// 全部用例走同一条流水线：begin_frame(输入快照) → touch(控件) → end_frame。
// 输入快照是精确的（不读时钟、不读窗口），因此"第几帧按下、第几帧松开、
// 鼠标在哪"都能逐帧断言——这正是把交互从 ImGui 里剥出来单测的意义。
// ===========================================================================

namespace {

using gryce_ui::Rect;
using gryce_ui::UiInputFrame;
using gryce_ui::Widget;
using gryce_ui::WidgetClass;
using gryce_ui::WidgetKind;
using gryce_ui::WidgetTree;
using gryce_ui::BuiltinUi;

// 默认度量实现（估算）：单测里宽度可预测，不需要字体
struct ApproxMetrics final : gryce_ui::UiTextMetrics {};

UiInputFrame mouse_at(float x, float y, bool down, bool clicked, bool released) {
    UiInputFrame in;
    in.mouse_x = x;
    in.mouse_y = y;
    in.mouse_down[0] = down;
    in.mouse_clicked[0] = clicked;
    in.mouse_released[0] = released;
    return in;
}

UiInputFrame typing(const char* chars, bool backspace = false) {
    UiInputFrame in;
    in.mouse_x = 50.0f;
    in.mouse_y = 20.0f;
    for (const char* p = chars; p && *p && in.char_count < UiInputFrame::kMaxChars; ++p) {
        in.chars[in.char_count++] = static_cast<uint32_t>(static_cast<unsigned char>(*p));
    }
    in.backspace = backspace;
    return in;
}

// 与 ui_widget.cpp 的 shade(c, +0.04f) 同公式：按钮渐变填充命令的顶端色。
// 断言"填充命令颜色"时必须用它换算——命令里装的是渐变端点，不是 token 原值。
uint32_t shade_top_4(uint32_t c) {
    auto adj = [](uint32_t v) {
        const float t = static_cast<float>(v) / 255.0f;
        return static_cast<uint32_t>((t + (1.0f - t) * 0.04f) * 255.0f + 0.5f);
    };
    return (adj((c >> 24) & 0xFFu) << 24) | (adj((c >> 16) & 0xFFu) << 16) |
           (adj((c >> 8) & 0xFFu) << 8) | (c & 0xFFu);
}

// 统计 drawlist 里某类命令的条数
int count_cmd(const gryce_ui::UiDrawList& dl, gryce_ui::UiCmd kind) {    int n = 0;
    for (int i = 0; i < dl.command_count(); ++i) {
        if (dl.command(i).cmd == kind) ++n;
    }
    return n;
}

// 第一条指定类型的命令（nullptr = 没有）。颜色断言用它，不必依赖"第 0 条是填充"。
const gryce_ui::UiCommand* first_cmd(const gryce_ui::UiDrawList& dl, gryce_ui::UiCmd kind) {
    for (int i = 0; i < dl.command_count(); ++i) {
        if (dl.command(i).cmd == kind) return &dl.command(i);
    }
    return nullptr;
}

} // namespace

TEST(widget, button_click_needs_press_and_release_inside) {
    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 100.0f, 30.0f};

    tree.begin_frame(mouse_at(50.0f, 20.0f, true, true, false));   // 第 1 帧：按下
    Widget* w = tree.touch(1, WidgetKind::Button, r, "OK");
    CHECK_TRUE(w != nullptr);
    CHECK_TRUE(w->pressed);
    CHECK_FALSE(w->clicked);
    tree.end_frame();

    tree.begin_frame(mouse_at(52.0f, 22.0f, true, false, false));  // 第 2 帧：仍按住
    w = tree.touch(1, WidgetKind::Button, r, "OK");
    CHECK_TRUE(w->pressed);
    tree.end_frame();

    tree.begin_frame(mouse_at(52.0f, 22.0f, false, false, true));  // 第 3 帧：松开
    w = tree.touch(1, WidgetKind::Button, r, "OK");
    CHECK_FALSE(w->pressed);
    CHECK_TRUE(w->clicked);
    tree.end_frame();

    tree.begin_frame(mouse_at(0.0f, 0.0f, false, false, false));   // 第 4 帧：无事
    w = tree.touch(1, WidgetKind::Button, r, "OK");
    CHECK_FALSE(w->clicked);
    tree.end_frame();
}

TEST(widget, release_outside_cancels_click) {
    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 100.0f, 30.0f};

    tree.begin_frame(mouse_at(50.0f, 20.0f, true, true, false));
    tree.touch(1, WidgetKind::Button, r, "OK");
    tree.end_frame();

    // 拖到外面松开：按下归属没变，但松开点不在控件内 → 不产生点击
    tree.begin_frame(mouse_at(400.0f, 400.0f, false, false, true));
    Widget* w = tree.touch(1, WidgetKind::Button, r, "OK");
    CHECK_FALSE(w->clicked);
    CHECK_FALSE(w->hovered);
    tree.end_frame();
}

TEST(widget, press_outside_then_release_inside_is_not_a_click) {
    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 100.0f, 30.0f};

    // 在空白处按下（没有任何控件认领）
    tree.begin_frame(mouse_at(400.0f, 400.0f, true, true, false));
    tree.end_frame();
    // 移到控件内松开
    tree.begin_frame(mouse_at(50.0f, 20.0f, false, false, true));
    Widget* w = tree.touch(1, WidgetKind::Button, r, "OK");
    CHECK_FALSE(w->clicked);
    CHECK_TRUE(w->hovered);
    tree.end_frame();
}

TEST(widget, hovered_follows_mouse_geometry) {
    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 100.0f, 30.0f};

    tree.begin_frame(mouse_at(50.0f, 20.0f, false, false, false));
    CHECK_TRUE(tree.touch(1, WidgetKind::Button, r, "OK")->hovered);
    tree.end_frame();

    tree.begin_frame(mouse_at(9.0f, 20.0f, false, false, false));   // 左边界外
    CHECK_FALSE(tree.touch(1, WidgetKind::Button, r, "OK")->hovered);
    tree.end_frame();

    tree.begin_frame(mouse_at(110.0f, 40.0f, false, false, false)); // 右下角（含边界）
    CHECK_TRUE(tree.touch(1, WidgetKind::Button, r, "OK")->hovered);
    tree.end_frame();
}

TEST(widget, checkbox_toggles_on_click) {
    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 120.0f, 20.0f};

    tree.begin_frame(mouse_at(50.0f, 20.0f, false, false, false));
    Widget* w = tree.touch(1, WidgetKind::Checkbox, r, "Shadows");
    CHECK_FALSE(w->checked);                       // 新建控件默认不勾
    tree.end_frame();

    for (int i = 0; i < 3; ++i) {
        tree.begin_frame(mouse_at(50.0f, 20.0f, true, true, false));   // 每轮都是新的按下
        w = tree.touch(1, WidgetKind::Checkbox, r, "Shadows");
        tree.end_frame();
        tree.begin_frame(mouse_at(50.0f, 20.0f, false, false, true));
        w = tree.touch(1, WidgetKind::Checkbox, r, "Shadows");
        tree.end_frame();
        CHECK_EQ(w->checked, (i % 2) == 0);        // 点一次翻一次
    }
}

TEST(widget, slider_drag_maps_mouse_to_value) {
    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 100.0f, 20.0f};

    // 按下时光标在轨道 40% 处 → 值直接跳过去（不用先拖）
    tree.begin_frame(mouse_at(50.0f, 20.0f, true, true, false));
    Widget* w = tree.touch(1, WidgetKind::Slider, r, "Opacity");
    w->min_value = 0.0f;
    w->max_value = 1.0f;
    CHECK_NEAR(w->value, 0.4f, 1e-6f);
    tree.end_frame();

    // 继续拖：按住期间每帧按鼠标位置更新（含拖出框外，钳住）
    tree.begin_frame(mouse_at(90.0f, 20.0f, true, false, false));
    w = tree.touch(1, WidgetKind::Slider, r, "Opacity");
    CHECK_NEAR(w->value, 0.8f, 1e-6f);
    tree.end_frame();

    tree.begin_frame(mouse_at(-50.0f, 20.0f, true, false, false));
    w = tree.touch(1, WidgetKind::Slider, r, "Opacity");
    CHECK_NEAR(w->value, 0.0f, 1e-6f);
    tree.end_frame();

    tree.begin_frame(mouse_at(1000.0f, 20.0f, true, false, false));
    w = tree.touch(1, WidgetKind::Slider, r, "Opacity");
    CHECK_NEAR(w->value, 1.0f, 1e-6f);
    tree.end_frame();

    // 换范围：min/max 由调用方写进控件，映射随之改变
    w->min_value = 2.0f;
    w->max_value = 6.0f;
    tree.begin_frame(mouse_at(60.0f, 20.0f, true, false, false));
    w = tree.touch(1, WidgetKind::Slider, r, "Opacity");
    CHECK_NEAR(w->value, 4.0f, 1e-6f);   // 2 + 0.5*(6-2)
    tree.end_frame();
}

TEST(widget, slider_ignores_mouse_without_press) {
    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 100.0f, 20.0f};

    tree.begin_frame(mouse_at(50.0f, 20.0f, false, false, false));
    Widget* w = tree.touch(1, WidgetKind::Slider, r, "Opacity");
    w->min_value = 0.0f;
    w->max_value = 1.0f;
    w->value = 0.25f;
    CHECK_NEAR(w->value, 0.25f, 1e-6f);   // 悬停不改值（只有按住拖才改）
    CHECK_TRUE(w->hovered);
    tree.end_frame();
}

TEST(widget, textinput_types_backspaces_and_respects_capacity) {
    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 200.0f, 22.0f};

    // 点一下聚焦
    tree.begin_frame(mouse_at(50.0f, 20.0f, true, true, false));
    tree.touch(1, WidgetKind::TextInput, r, "Name");
    tree.end_frame();
    CHECK_EQ(tree.focus_id(), 1u);

    // 打字
    tree.begin_frame(typing("Cube"));
    Widget* w = tree.touch(1, WidgetKind::TextInput, r, "Name");
    CHECK_STR(w->text, "Cube");
    tree.end_frame();

    // 退格
    tree.begin_frame(typing("", true));
    w = tree.touch(1, WidgetKind::TextInput, r, "Name");
    CHECK_STR(w->text, "Cub");
    tree.end_frame();

    // 多字节码点整体进退（UTF-8 序列不被撕坏）
    tree.begin_frame(typing(""));
    w = tree.touch(1, WidgetKind::TextInput, r, "Name");
    CHECK_STR(w->text, "Cub");
    tree.end_frame();

    // 失焦期间键入不进去
    tree.clear_focus();
    tree.begin_frame(typing("XYZ"));
    w = tree.touch(1, WidgetKind::TextInput, r, "Name");
    CHECK_STR(w->text, "Cub");
    tree.end_frame();

    // 容量：灌满到 kWidgetTextCap-1 为止，不多写一个字节
    tree.begin_frame(mouse_at(50.0f, 20.0f, true, true, false));
    w = tree.touch(1, WidgetKind::TextInput, r, "Name");
    tree.end_frame();
    for (int i = 0; i < 400; ++i) {
        tree.begin_frame(typing("A"));
        w = tree.touch(1, WidgetKind::TextInput, r, "Name");
        tree.end_frame();
    }
    CHECK_EQ(std::strlen(w->text), static_cast<size_t>(gryce_ui::kWidgetTextCap - 1));
}

TEST(widget, textinput_multibyte_roundtrip) {
    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 200.0f, 22.0f};
    tree.begin_frame(mouse_at(50.0f, 20.0f, true, true, false));
    tree.touch(1, WidgetKind::TextInput, r, "Name");
    tree.end_frame();

    UiInputFrame in = typing("");
    in.chars[in.char_count++] = 0x4E2Du;   // 中
    in.chars[in.char_count++] = 0x6587u;   // 文
    tree.begin_frame(in);
    Widget* w = tree.touch(1, WidgetKind::TextInput, r, "Name");
    CHECK_STR(w->text, "\xE4\xB8\xAD\xE6\x96\x87");   // "中文" 的 UTF-8
    tree.end_frame();

    // 退格一次删掉整个"文"（3 字节），不是撕半个序列
    tree.begin_frame(typing("", true));
    w = tree.touch(1, WidgetKind::TextInput, r, "Name");
    CHECK_STR(w->text, "\xE4\xB8\xAD");
    tree.end_frame();
}

TEST(widget, focus_moves_between_inputs_and_clears_on_blank) {
    WidgetTree tree;
    const Rect a{10.0f, 10.0f, 200.0f, 22.0f};
    const Rect b{10.0f, 40.0f, 200.0f, 22.0f};

    tree.begin_frame(mouse_at(50.0f, 20.0f, true, true, false));
    tree.touch(1, WidgetKind::TextInput, a, "A");
    tree.touch(2, WidgetKind::TextInput, b, "B");
    tree.end_frame();
    CHECK_EQ(tree.focus_id(), 1u);

    tree.begin_frame(typing("aa"));
    tree.touch(1, WidgetKind::TextInput, a, "A");
    tree.touch(2, WidgetKind::TextInput, b, "B");
    tree.end_frame();
    CHECK_STR(tree.find(1)->text, "aa");
    CHECK_STR(tree.find(2)->text, "");

    // 点 B：焦点转移，之后键入进 B
    tree.begin_frame(mouse_at(50.0f, 50.0f, true, true, false));
    tree.touch(1, WidgetKind::TextInput, a, "A");
    tree.touch(2, WidgetKind::TextInput, b, "B");
    tree.end_frame();
    CHECK_EQ(tree.focus_id(), 2u);

    tree.begin_frame(typing("bb"));
    tree.touch(1, WidgetKind::TextInput, a, "A");
    tree.touch(2, WidgetKind::TextInput, b, "B");
    tree.end_frame();
    CHECK_STR(tree.find(1)->text, "aa");   // A 不再长字
    CHECK_STR(tree.find(2)->text, "bb");

    // 点空白处：焦点清掉
    tree.begin_frame(mouse_at(400.0f, 400.0f, true, true, false));
    tree.touch(1, WidgetKind::TextInput, a, "A");
    tree.touch(2, WidgetKind::TextInput, b, "B");
    tree.end_frame();
    CHECK_EQ(tree.focus_id(), 0u);

    tree.begin_frame(typing("cc"));
    tree.touch(1, WidgetKind::TextInput, a, "A");
    tree.touch(2, WidgetKind::TextInput, b, "B");
    tree.end_frame();
    CHECK_STR(tree.find(2)->text, "bb");   // 没有焦点，键入被丢弃
}

TEST(widget, every_kind_has_an_explicit_class_and_only_interactive_is_hit_tested) {
    // 分类表是命中测试的唯一真源（touch() 里那串 if 已经没了）。这张表逐 kind
    // 列期望值：新增 kind 时 Count 变，ui_widget.h 的 static_assert 先炸；改错
    // 分类时这里炸。两道一起保证"新 kind 不可能静默拿到错的命中策略"。
    struct Entry { WidgetKind kind; WidgetClass cls; const char* name; };
    static const Entry table[] = {
        {WidgetKind::Button,     WidgetClass::Interactive, "Button"},
        {WidgetKind::Label,      WidgetClass::Text,        "Label"},
        {WidgetKind::Slider,     WidgetClass::Interactive, "Slider"},
        {WidgetKind::Checkbox,   WidgetClass::Interactive, "Checkbox"},
        {WidgetKind::TextInput,  WidgetClass::Interactive, "TextInput"},
        {WidgetKind::Panel,      WidgetClass::Container,   "Panel"},
        {WidgetKind::Progress,   WidgetClass::Decoration,  "Progress"},
        {WidgetKind::Separator,  WidgetClass::Decoration,  "Separator"},
        {WidgetKind::DragFloat,  WidgetClass::Interactive, "DragFloat"},
        {WidgetKind::TreeNode,   WidgetClass::Interactive, "TreeNode"},
        {WidgetKind::Header,     WidgetClass::Interactive, "Header"},
        {WidgetKind::MenuItem,   WidgetClass::Interactive, "MenuItem"},
        {WidgetKind::ScrollBar,  WidgetClass::Interactive, "ScrollBar"},
        {WidgetKind::Tooltip,    WidgetClass::Decoration,  "Tooltip"},
        {WidgetKind::ClipBegin,  WidgetClass::Container,   "ClipBegin"},
        {WidgetKind::ClipEnd,    WidgetClass::Container,   "ClipEnd"},
        {WidgetKind::Bar,        WidgetClass::Decoration,  "Bar"},
        {WidgetKind::TitleBar,   WidgetClass::Interactive, "TitleBar"},
        {WidgetKind::Frame,      WidgetClass::Container,   "Frame"},
        {WidgetKind::Selectable, WidgetClass::Interactive, "Selectable"},
        {WidgetKind::Image,      WidgetClass::Interactive, "Image"},
    };
    const int n = static_cast<int>(sizeof(table) / sizeof(table[0]));
    CHECK_EQ(n, static_cast<int>(WidgetKind::Count));   // 一条不漏、一条不多

    // 逐 kind 校表 + 实际行为（Interactive 的才该被命中）
    for (int i = 0; i < n; ++i) {
        const Entry& e = table[i];
        CHECK_EQ(static_cast<int>(e.kind), i);          // 表序 == 枚举序
        CHECK_EQ(static_cast<int>(widget_class_of(e.kind)), static_cast<int>(e.cls));

        WidgetTree tree;
        const Rect r{0.0f, 0.0f, 400.0f, 300.0f};
        tree.begin_frame(mouse_at(100.0f, 100.0f, true, true, false));
        Widget* w = tree.touch(1, e.kind, r, e.name);
        CHECK_TRUE(w != nullptr);
        const bool should_hit = (e.cls == WidgetClass::Interactive);
        CHECK_EQ(w->hovered, should_hit);
        CHECK_EQ(tree.active_id() != 0u, should_hit);
        tree.end_frame();
    }
}

TEST(widget, label_is_not_hit_testable) {
    WidgetTree tree;
    const Rect r{0.0f, 0.0f, 400.0f, 300.0f};   // 大背景一样的标签

    tree.begin_frame(mouse_at(100.0f, 100.0f, true, true, false));
    Widget* w = tree.touch(1, WidgetKind::Label, r, "background");
    CHECK_FALSE(w->hovered);
    CHECK_FALSE(w->pressed);
    CHECK_FALSE(w->clicked);
    CHECK_EQ(tree.active_id(), 0u);             // 没有控件认领这次按下
    tree.end_frame();
}

TEST(widget, disabled_widget_ignores_input) {
    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 100.0f, 30.0f};

    tree.begin_frame(mouse_at(50.0f, 20.0f, true, true, false));
    Widget* w = tree.touch(1, WidgetKind::Button, r, "OK");
    w->enabled = false;                          // 本帧起禁用
    tree.end_frame();
    tree.begin_frame(mouse_at(50.0f, 20.0f, false, false, true));
    w = tree.touch(1, WidgetKind::Button, r, "OK");
    CHECK_FALSE(w->clicked);                     // 禁用控件不响应
    CHECK_FALSE(w->hovered);
    tree.end_frame();
}

TEST(widget, later_submission_wins_press_on_overlap) {
    // 提交序 = 上层序：两个矩形重叠时，后提交（画在上层）的拿到按下与点击
    WidgetTree tree;
    const Rect under{10.0f, 10.0f, 100.0f, 60.0f};
    const Rect over{10.0f, 10.0f, 100.0f, 60.0f};

    tree.begin_frame(mouse_at(50.0f, 30.0f, true, true, false));
    tree.touch(1, WidgetKind::Button, under, "under");
    tree.touch(2, WidgetKind::Button, over, "over");
    tree.end_frame();
    tree.begin_frame(mouse_at(50.0f, 30.0f, false, false, true));
    Widget* w1 = tree.touch(1, WidgetKind::Button, under, "under");
    Widget* w2 = tree.touch(2, WidgetKind::Button, over, "over");
    CHECK_FALSE(w1->clicked);
    CHECK_TRUE(w2->clicked);
    tree.end_frame();
}

TEST(widget, sweep_recycles_untouched_keeps_touched) {
    WidgetTree tree;
    const Rect r{0.0f, 0.0f, 80.0f, 24.0f};

    tree.begin_frame(mouse_at(0.0f, 0.0f, false, false, false));
    tree.touch(1, WidgetKind::Button, r, "a");
    tree.touch(2, WidgetKind::Button, r, "b");
    tree.touch(3, WidgetKind::Button, r, "c");
    CHECK_EQ(tree.count(), 3);
    tree.end_frame();

    // 第 2 帧只提交 1 和 3：2 应该被回收，且它的值不残留到新控件上
    tree.begin_frame(mouse_at(0.0f, 0.0f, false, false, false));
    tree.touch(1, WidgetKind::Button, r, "a");
    tree.touch(3, WidgetKind::Button, r, "c");
    tree.sweep();
    CHECK_EQ(tree.count(), 2);
    CHECK_TRUE(tree.find(2) == nullptr);
    tree.end_frame();

    // 回收的槽位可复用：新 id 2 拿到的是干净控件
    tree.begin_frame(mouse_at(0.0f, 0.0f, false, false, false));
    Widget* w = tree.touch(2, WidgetKind::Checkbox, r, "new");
    CHECK_TRUE(w != nullptr);
    CHECK_FALSE(w->checked);      // 不是上一个 id=2 的残留状态
    CHECK_EQ(tree.count(), 3);
    tree.end_frame();
}

TEST(widget, touch_rejects_zero_id_and_full_table) {
    WidgetTree tree;
    tree.begin_frame(mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_TRUE(tree.touch(0, WidgetKind::Button, Rect{0, 0, 10, 10}, "zero") == nullptr);

    const Rect r{0.0f, 0.0f, 10.0f, 10.0f};
    for (int i = 1; i <= WidgetTree::kCapacity; ++i) {
        CHECK_TRUE(tree.touch(static_cast<uint64_t>(i), WidgetKind::Label, r, "x") != nullptr);
    }
    // 表满：拒绝而不是越界（调用方能看见失败）
    CHECK_TRUE(tree.touch(99999, WidgetKind::Label, r, "overflow") == nullptr);
    tree.end_frame();
}

TEST(widget, update_all_drives_retained_flow) {
    // retained 用法：树自己维护几何，update_all + draw，不 sweep
    WidgetTree tree;
    tree.begin_frame(mouse_at(0.0f, 0.0f, false, false, false));
    tree.touch(1, WidgetKind::Slider, Rect{10.0f, 10.0f, 100.0f, 20.0f}, "v");
    tree.touch(2, WidgetKind::Checkbox, Rect{10.0f, 40.0f, 120.0f, 20.0f}, "c");
    tree.end_frame();

    // 按住滑块中段拖动（值已经在 touch 里落地）
    tree.begin_frame(mouse_at(85.0f, 20.0f, true, true, false));
    tree.update_all();
    CHECK_NEAR(tree.find(1)->value, 0.75f, 1e-6f);
    tree.end_frame();

    tree.begin_frame(mouse_at(85.0f, 20.0f, false, false, true));
    tree.update_all();
    CHECK_NEAR(tree.find(1)->value, 0.75f, 1e-6f);
    tree.end_frame();

    tree.begin_frame(mouse_at(30.0f, 50.0f, true, true, false));
    tree.update_all();
    tree.end_frame();
    tree.begin_frame(mouse_at(30.0f, 50.0f, false, false, true));
    tree.update_all();
    CHECK_TRUE(tree.find(2)->checked);
    tree.end_frame();

    tree.clear();
    CHECK_EQ(tree.count(), 0);
}

TEST(widget, draw_emits_commands_per_kind) {
    WidgetTree tree;
    tree.begin_frame(mouse_at(0.0f, 0.0f, false, false, false));
    tree.touch(1, WidgetKind::Button, Rect{0.0f, 0.0f, 80.0f, 24.0f}, "OK");
    tree.touch(2, WidgetKind::Label, Rect{0.0f, 30.0f, 80.0f, 16.0f}, "text");
    tree.touch(3, WidgetKind::Checkbox, Rect{0.0f, 50.0f, 120.0f, 20.0f}, "on");
    tree.touch(4, WidgetKind::Slider, Rect{0.0f, 80.0f, 200.0f, 20.0f}, "v")->value = 0.5f;
    Widget* ti = tree.touch(5, WidgetKind::TextInput, Rect{0.0f, 110.0f, 200.0f, 22.0f}, "name");
    std::strcpy(ti->text, "hi");                    // 空文本不产生命令：给点内容
    tree.end_frame();

    ApproxMetrics metrics;
    gryce_ui::UiDrawList dl;
    tree.draw(dl, gryce_ui::theme(), metrics);

    // 美化后的形态：填充与描边各是一条**圆角**命令（不再是"底 + 四条边"）。
    // Button: 渐变底 + 圆角描边 + 标签 = 3；Label: 1 文本；
    // Checkbox: 底 + 描边 + 标签 = 3；Slider: 标签 + 轨道 + 填充 + 手柄(底+边) = 5；
    // TextInput: 底 + 描边 + push/pop 裁剪 + 文本 = 5
    CHECK_EQ(dl.command_count(), 3 + 1 + 3 + 5 + 5);
    // 直角矩形一个都不该有：边框全走圆角描边命令
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::Rect), 0);
    // 圆角填充：Button 1 + Checkbox 1 + Slider 轨道 1 + Slider 填充 1 + Slider 手柄 1
    //           + TextInput 1 = 6
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::RoundRect), 6);
    // 圆角描边：Button 1 + Checkbox 1 + Slider 手柄 1 + TextInput 1 = 4
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::RoundRectBorder), 4);
    // Text：每个控件各一条
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::Text), 5);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::PushClip), 1);   // 只有 TextInput 裁剪
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::PopClip), 1);
    CHECK_EQ(dl.clip_depth(), 0);                            // 绘制后裁剪栈必须归零
    CHECK_EQ(dl.dropped(), 0u);
    CHECK_EQ(dl.culled(), 0u);
}

TEST(widget, draw_follows_theme_preset) {
    WidgetTree tree;
    tree.begin_frame(mouse_at(400.0f, 400.0f, false, false, false));   // 鼠标在按钮外：非 hover
    tree.touch(1, WidgetKind::Button, Rect{0.0f, 0.0f, 80.0f, 24.0f}, "OK");
    tree.end_frame();

    ApproxMetrics metrics;
    gryce_ui::UiDrawList dl;
    tree.draw(dl, gryce_ui::theme(), metrics);
    // 按钮底现在是竖直渐变：一条 RoundRect + color2
    CHECK_EQ(dl.command(0).cmd, gryce_ui::UiCmd::RoundRect);
    CHECK_TRUE(dl.command(0).color2 != 0u);
    CHECK_TRUE(dl.command(0).color != dl.command(0).color2);
    const uint32_t dark_top = dl.command(0).color;
    // 描边取"控件描边"token（不参与渐变）——换成浅色后它必须换，且角半径不变。
    // 控件描边不是 BORDER：BORDER 是面板/弹层的强边界，控件日常只勾形状。
    const gryce_ui::UiCommand* dark_edge =
        first_cmd(dl, gryce_ui::UiCmd::RoundRectBorder);
    CHECK_TRUE(dark_edge != nullptr);
    CHECK_EQ(dark_edge->color, gryce_ui::theme().colors[gryce_ui::THEME_COLOR_CONTROL_BORDER]);
    CHECK_TRUE(dark_edge->radii[0] > 0.0f);

    // 换预设重画：颜色跟着 token 表走（调用方只引用 token）
    gryce_ui::set_preset(gryce_ui::THEME_PRESET_LIGHT);
    dl.reset();
    tree.draw(dl, gryce_ui::theme(), metrics);
    CHECK_TRUE(dl.command(0).color != dark_top);
    const gryce_ui::UiCommand* light_edge =
        first_cmd(dl, gryce_ui::UiCmd::RoundRectBorder);
    CHECK_TRUE(light_edge != nullptr);
    CHECK_EQ(light_edge->color, gryce_ui::theme().colors[gryce_ui::THEME_COLOR_CONTROL_BORDER]);
    CHECK_NEAR(light_edge->radii[0], dark_edge->radii[0], 1e-6f);   // 几何不随主题变
    gryce_ui::set_preset(gryce_ui::THEME_PRESET_DARK);   // 别影响后续用例
}

TEST(widget, draw_text_positions_are_deterministic) {
    WidgetTree tree;
    tree.begin_frame(mouse_at(0.0f, 0.0f, false, false, false));
    tree.touch(1, WidgetKind::Label, Rect{0.0f, 0.0f, 80.0f, 20.0f}, "abc");
    tree.end_frame();

    ApproxMetrics metrics;
    gryce_ui::UiDrawList dl;
    tree.draw(dl, gryce_ui::theme(), metrics);
    CHECK_EQ(dl.command_count(), 1);
    // 行盒在控件里垂直居中：y = (20 - 1.25*13)/2 ≈ 1.875
    CHECK_NEAR(dl.command(0).y, 1.875f, 1e-4f);
    CHECK_NEAR(dl.command(0).font_size, gryce_ui::kWidgetFontSize, 1e-6f);
}

TEST(widget, approx_text_width_counts_cjk_as_full_width) {
    CHECK_NEAR(gryce_ui::ui_approx_text_width("abc", 10.0f), 15.0f, 1e-6f);
    CHECK_NEAR(gryce_ui::ui_approx_text_width("", 10.0f), 0.0f, 1e-6f);
    CHECK_NEAR(gryce_ui::ui_approx_text_width(nullptr, 10.0f), 0.0f, 1e-6f);
    // "中文" = 2 个全宽码点
    CHECK_NEAR(gryce_ui::ui_approx_text_width("\xE4\xB8\xAD\xE6\x96\x87", 10.0f), 20.0f, 1e-6f);
    // 混合："a中" = 0.5 + 1.0
    CHECK_NEAR(gryce_ui::ui_approx_text_width("a\xE4\xB8\xAD", 10.0f), 15.0f, 1e-6f);
}

// ===========================================================================
// suite: builtin —— 内置即时模式层（窗口栈 / 光标流 / 控件 / 查询）
// ===========================================================================

namespace {

// 起一帧：内置层每帧都要喂输入 + 主题 + 度量
void bi_frame(BuiltinUi& ui, const UiInputFrame& in) {
    static ApproxMetrics metrics;
    ui.begin_frame(in, gryce_ui::theme(), metrics);
}

} // namespace

TEST(builtin, window_stack_depth_and_limits) {
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.window_depth(), 0);

    for (int i = 0; i < BuiltinUi::kMaxWindows; ++i) {
        char name[8];
        std::snprintf(name, sizeof(name), "W%d", i);
        CHECK_EQ(ui.begin_window(name, Rect{0.0f, 0.0f, 100.0f, 100.0f}), 1);
        CHECK_EQ(ui.window_depth(), i + 1);
    }
    // 栈满：第 9 个进不来，且不提交 chrome
    const int before = ui.tree().count();
    CHECK_EQ(ui.begin_window("overflow", Rect{0.0f, 0.0f, 100.0f, 100.0f}), 0);
    CHECK_EQ(ui.window_depth(), BuiltinUi::kMaxWindows);
    CHECK_EQ(ui.tree().count(), before);

    for (int i = 0; i < BuiltinUi::kMaxWindows; ++i) ui.end_window();
    CHECK_EQ(ui.window_depth(), 0);
    ui.end_frame();
    // 8 个 Panel chrome
    CHECK_EQ(ui.tree().count(), BuiltinUi::kMaxWindows);
}

TEST(builtin, degenerate_window_rejected) {
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("zero_w", Rect{0.0f, 0.0f, 0.0f, 100.0f}), 0);
    // 被拒窗口里提交内容 = no-op；Begin/End 仍要配对（退化态占一个栈位）
    ui.text("invisible");
    CHECK_EQ(ui.tree().count(), 0);
    ui.end_window();
    CHECK_EQ(ui.window_depth(), 0);

    CHECK_EQ(ui.begin_window("neg", Rect{0.0f, 0.0f, -5.0f, 10.0f}), 0);
    ui.end_window();
    CHECK_EQ(ui.begin_window("flat", Rect{0.0f, 0.0f, 100.0f, 0.0f}), 0);
    ui.end_window();
    CHECK_EQ(ui.window_depth(), 0);
    CHECK_EQ(ui.tree().count(), 0);
    ui.end_frame();
}

TEST(builtin, items_flow_downwards) {
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{10.0f, 10.0f, 200.0f, 200.0f}), 1);

    // PAD_X=8 / PAD_Y=5 / ITEM_SPACING=4 / 字号 13 / 行高 16.25 / 帧高 26.25
    ui.text("hello");                                   // y = 10+5 = 15，高 16.25
    CHECK_NEAR(ui.last_item_rect().y, 15.0f, 1e-5f);
    CHECK_NEAR(ui.last_item_rect().h, 16.25f, 1e-5f);

    ui.button("OK");                                    // y = 15+16.25+4 = 35.25
    CHECK_NEAR(ui.last_item_rect().y, 35.25f, 1e-5f);
    CHECK_NEAR(ui.last_item_rect().h, 26.25f, 1e-5f);
    CHECK_NEAR(ui.last_item_rect().w, 13.0f + 16.0f, 1e-5f);   // "OK"=1em + 2*PAD_X

    ui.separator();                                     // y = 35.25+26.25+4 = 65.5，高 1
    CHECK_NEAR(ui.last_item_rect().y, 65.5f, 1e-5f);
    CHECK_NEAR(ui.last_item_rect().h, 1.0f, 1e-5f);

    ui.text("world");                                   // y = 65.5+1+4 = 70.5
    CHECK_NEAR(ui.last_item_rect().y, 70.5f, 1e-5f);
    ui.end_window();
    ui.end_frame();
}

TEST(builtin, same_line_places_to_the_right) {
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{0.0f, 0.0f, 300.0f, 200.0f}), 1);

    ui.button_size("A", 40.0f, 20.0f);                  // (8,5,40,20)
    const Rect a = ui.last_item_rect();
    CHECK_NEAR(a.x, 8.0f, 1e-5f);
    CHECK_NEAR(a.y, 5.0f, 1e-5f);

    ui.same_line();
    ui.button_size("B", 40.0f, 20.0f);                  // 右边 4px，同一行
    const Rect b = ui.last_item_rect();
    CHECK_NEAR(b.x, a.right() + 4.0f, 1e-5f);
    CHECK_NEAR(b.y, a.y, 1e-5f);
    CHECK_NEAR(b.h, a.h, 1e-5f);

    ui.button_size("C", 40.0f, 20.0f);                  // 回到新行：y = 5+20+4
    CHECK_NEAR(ui.last_item_rect().x, 8.0f, 1e-5f);
    CHECK_NEAR(ui.last_item_rect().y, 29.0f, 1e-5f);

    // same_line 只对下一个 item 生效
    ui.same_line(10.0f);
    ui.button_size("D", 10.0f, 10.0f);
    ui.button_size("E", 10.0f, 10.0f);                  // 又一个新行
    CHECK_NEAR(ui.last_item_rect().x, 8.0f, 1e-5f);
    ui.end_window();
    ui.end_frame();
}

TEST(builtin, button_click_roundtrip) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};

    bi_frame(ui, mouse_at(20.0f, 20.0f, true, true, false));    // 按下（按钮宽 29：x∈[8,37]）
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.button("OK"), 0);                                // 按下帧不算点击
    CHECK_TRUE(ui.is_item_active());
    ui.end_window();
    ui.end_frame();

    bi_frame(ui, mouse_at(20.0f, 20.0f, false, false, true));    // 松开
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.button("OK"), 1);                                // 本帧完成点击
    ui.end_window();
    ui.end_frame();

    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));     // 无事
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.button("OK"), 0);                                // 不重复触发
    ui.end_window();
    ui.end_frame();
}

TEST(builtin, checkbox_roundtrips_caller_value) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    int v = 1;

    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.checkbox("Shadows", &v), 0);                     // 没交互就不报变化
    CHECK_EQ(v, 1);                                              // 调用方的值不动
    ui.end_window();
    ui.end_frame();

    // 点一下：勾态翻转并写回调用方
    bi_frame(ui, mouse_at(100.0f, 20.0f, true, true, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.checkbox("Shadows", &v), 0);
    ui.end_window();
    ui.end_frame();
    bi_frame(ui, mouse_at(100.0f, 20.0f, false, false, true));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.checkbox("Shadows", &v), 1);
    ui.end_window();
    ui.end_frame();
    CHECK_EQ(v, 0);

    // 外部改值（undo/重置）：不被控件里的旧值盖掉
    v = 1;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.checkbox("Shadows", &v), 0);
    ui.end_window();
    ui.end_frame();
    CHECK_EQ(v, 1);
}

TEST(builtin, slider_drag_and_external_sync) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};   // 内容宽 284
    float v = 0.5f;

    // 悬停不改值
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.slider_float("Opacity", &v, 0.0f, 1.0f), 0);
    CHECK_NEAR(v, 0.5f, 1e-6f);
    ui.end_window();
    ui.end_frame();

    // 外部改成 0.9（模拟撤销）：同步进控件，不拖就不动
    v = 0.9f;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.slider_float("Opacity", &v, 0.0f, 1.0f), 0);
    ui.end_window();
    ui.end_frame();
    CHECK_NEAR(v, 0.9f, 1e-6f);

    // 按在 30% 处：值直接跳过去并写回
    bi_frame(ui, mouse_at(8.0f + 284.0f * 0.3f, 20.0f, true, true, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.slider_float("Opacity", &v, 0.0f, 1.0f), 1);
    ui.end_window();
    ui.end_frame();
    CHECK_NEAR(v, 0.3f, 1e-5f);

    // 按住拖到 80%（拖出框外也钳住）
    bi_frame(ui, mouse_at(8.0f + 284.0f * 0.8f, 20.0f, true, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.slider_float("Opacity", &v, 0.0f, 1.0f), 1);
    ui.end_window();
    ui.end_frame();
    CHECK_NEAR(v, 0.8f, 1e-5f);
}

TEST(builtin, input_text_types_backspace_and_syncs) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    char buf[64] = "";
    const uint64_t input_id = gryce_ui::hash_label("Name", gryce_ui::hash_label("W"));

    // 点一下聚焦
    bi_frame(ui, mouse_at(100.0f, 20.0f, true, true, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.input_text("Name", buf, sizeof(buf)), 0);
    ui.end_window();
    ui.end_frame();
    CHECK_EQ(ui.tree().focus_id(), input_id);

    // 打字：进缓冲区并报一次变化
    bi_frame(ui, typing("Hi"));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.input_text("Name", buf, sizeof(buf)), 1);
    ui.end_window();
    ui.end_frame();
    CHECK_STR(buf, "Hi");

    // 退格
    bi_frame(ui, typing("", true));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.input_text("Name", buf, sizeof(buf)), 1);
    ui.end_window();
    ui.end_frame();
    CHECK_STR(buf, "H");

    // 外部清空（重置）：同步进控件
    std::strcpy(buf, "");
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.input_text("Name", buf, sizeof(buf)), 0);
    ui.end_window();
    ui.end_frame();
    const Widget* w = ui.tree().find(input_id);
    CHECK_TRUE(w != nullptr);
    CHECK_STR(w->text, "");
}

TEST(builtin, focus_drops_when_the_focused_widget_stops_being_submitted) {
    // 面板被关掉 / 条件 UI 收起来时，持有焦点的输入框不再被提交。
    // sweep 只回收槽位不清 focus_id_ 的话，那个 id 就成了野引用：
    // GUi_WantCaptureKeyboard()（内置轨 = tree_.focus_id() != 0）恒为真，
    // 宿主从此不再派发快捷键，直到用户随手点一下空白处才自愈。
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    char buf[64] = "";
    const uint64_t input_id = gryce_ui::hash_label("Name", gryce_ui::hash_label("W"));

    bi_frame(ui, mouse_at(100.0f, 20.0f, true, true, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.input_text("Name", buf, sizeof(buf));
    ui.end_window();
    ui.end_frame();
    CHECK_EQ(ui.tree().focus_id(), input_id);
    CHECK_TRUE(ui.wants_keyboard());

    // 下一帧：窗口还在，但输入框不再提交（宿主把那个面板收起来了）
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.text("panel body only");
    ui.end_window();
    ui.end_frame();

    CHECK_EQ(ui.tree().focus_id(), 0u);       // 焦点随控件一起消失
    CHECK_FALSE(ui.wants_keyboard());         // 快捷键通道恢复
    CHECK_TRUE(ui.tree().find(input_id) == nullptr);
}

TEST(builtin, focus_moves_between_inputs) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    char a[32] = "";
    char b[32] = "";

    // 聚焦第一个输入框（y = 15..41.25）
    bi_frame(ui, mouse_at(100.0f, 20.0f, true, true, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.input_text("A", a, sizeof(a));
    ui.end_window();
    ui.end_frame();
    CHECK_EQ(ui.tree().focus_id(), gryce_ui::hash_label("A", gryce_ui::hash_label("W")));

    // 点第二个输入框（y = 45.25..71.5）：焦点转移
    bi_frame(ui, mouse_at(100.0f, 50.0f, true, true, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.input_text("A", a, sizeof(a));
    ui.input_text("B", b, sizeof(b));
    ui.end_window();
    ui.end_frame();
    CHECK_EQ(ui.tree().focus_id(), gryce_ui::hash_label("B", gryce_ui::hash_label("W")));

    // 点按钮（y 75.5..101.75）：焦点清空
    bi_frame(ui, mouse_at(100.0f, 90.0f, true, true, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.input_text("A", a, sizeof(a));
    ui.input_text("B", b, sizeof(b));
    ui.button("OK");
    ui.end_window();
    ui.end_frame();
    CHECK_EQ(ui.tree().focus_id(), 0u);
}

TEST(builtin, two_separators_are_two_widgets) {
    // 同标签的无状态控件必须各占一个 slot（帧内序号进 id），否则只能画出一个
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{0.0f, 0.0f, 300.0f, 200.0f}), 1);
    ui.separator();
    ui.separator();
    ui.text("same");
    ui.text("same");
    ui.end_window();
    ui.end_frame();
    CHECK_EQ(ui.tree().count(), 5);               // Panel + 2 Separator + 2 Label

    ApproxMetrics metrics;
    gryce_ui::UiDrawList dl;
    ui.draw(dl);
    // chrome 现在是"圆角渐变底 + 圆角描边"2 条，分隔线仍是 2 条直角矩形
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::Rect), 2);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::RoundRect), 1);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::RoundRectBorder), 1);
}

TEST(builtin, progress_bar_emits_fill) {
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{0.0f, 0.0f, 300.0f, 200.0f}), 1);
    ui.progress_bar(0.5f, "50%");
    ui.end_window();
    ui.end_frame();

    ApproxMetrics metrics;
    gryce_ui::UiDrawList dl;
    ui.draw(dl);
    // chrome 2 条（渐变底 + 描边）+ 进度条 3 条（轨道 + 描边 + 圆角填充）+ 文字 1
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::RoundRect), 3);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::RoundRectBorder), 2);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::Rect), 0);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::Text), 1);

    // 0 进度不画填充
    ui.tree().clear();
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{0.0f, 0.0f, 300.0f, 200.0f}), 1);
    ui.progress_bar(0.0f, nullptr);
    ui.end_window();
    ui.end_frame();
    dl.reset();
    ui.draw(dl);
    // chrome 2 条 + 进度条 2 条（轨道 + 描边）；frac=0 不画填充
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::RoundRect), 2);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::Rect), 0);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::Text), 0);
}

TEST(builtin, label_text_composes_string) {
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{0.0f, 0.0f, 300.0f, 200.0f}), 1);
    ui.label_text("Position", "1.00");
    ui.end_window();
    ui.end_frame();

    bool found = false;
    ui.tree().for_each_alive([&](const Widget& w) {
        if (w.kind == WidgetKind::Label) {
            CHECK_STR(w.label, "Position: 1.00");
            found = true;
        }
    });
    CHECK_TRUE(found);
}

TEST(builtin, color_button_packs_rgba) {
    BuiltinUi ui;
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{0.0f, 0.0f, 300.0f, 200.0f}), 1);
    CHECK_EQ(ui.color_button("swatch", red), 0);
    ui.end_window();
    ui.end_frame();

    const Widget* w = ui.tree().find(gryce_ui::hash_label("swatch", gryce_ui::hash_label("W")));
    CHECK_TRUE(w != nullptr);
    CHECK_TRUE(w->bg_overridden);
    CHECK_EQ(w->bg_color, 0xFF0000FFu);
    CHECK_NEAR(w->rect.w, 26.25f, 1e-5f);      // 方形 = 帧高
    CHECK_NEAR(w->rect.h, 26.25f, 1e-5f);
}

TEST(builtin, hash_id_suffix_is_hidden_from_text) {
    // ImGui 惯例："Save##x" 的 id 是整串、显示只有 "Save"。两条都要对：
    // 显示不能带 ##，id 不能撞（同一窗口里两个 "Save##…" 是两个控件）。
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{0.0f, 0.0f, 300.0f, 200.0f}), 1);
    CHECK_EQ(ui.button("Save##toolbar"), 0);
    CHECK_EQ(ui.button("Save##inspector"), 0);
    ui.end_window();
    ui.end_frame();

    const uint64_t win = gryce_ui::hash_label("W");
    const Widget* a = ui.tree().find(gryce_ui::hash_label("Save##toolbar", win));
    const Widget* b = ui.tree().find(gryce_ui::hash_label("Save##inspector", win));
    CHECK_TRUE(a != nullptr);
    CHECK_TRUE(b != nullptr);
    CHECK_TRUE(a != b);
    CHECK_STR(a->label, "Save");                    // ## 之后不显示
    CHECK_NEAR(a->rect.w, 26.0f + 16.0f, 1e-4f);    // 宽度按 "Save" 算，不含 id
    CHECK_EQ(ui.tree().count(), 3);                 // chrome + 两个按钮
}

TEST(builtin, long_text_wraps_instead_of_clipping) {
    // 内容宽 200-16 = 184；ASCII 估算 6.5px/字符 -> 每行最多 28 字符。
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{0.0f, 0.0f, 200.0f, 300.0f}), 1);
    ui.text("aaaa bbbb cccc dddd eeee ffff");
    ui.end_window();
    ui.end_frame();

    int    labels = 0;
    float  ys[8]  = {};
    char   texts[8][gryce_ui::kWidgetLabelCap] = {};
    ui.tree().for_each_alive([&](const Widget& w) {
        if (w.kind != WidgetKind::Label || labels >= 8) return;
        ys[labels] = w.rect.y;
        std::snprintf(texts[labels], sizeof(texts[0]), "%s", w.label);
        ++labels;
    });
    CHECK_TRUE(labels >= 2);                       // 一行装不下 -> 折行
    CHECK_NEAR(ys[1] - ys[0], 16.25f + 4.0f, 1e-4f);   // 行高 + 行间距
    // 折行不丢字：每行的可见字符拼起来（去掉空格）等于原文（去掉空格）
    std::string joined;
    for (int i = 0; i < labels; ++i) {
        for (const char* p = texts[i]; *p; ++p) {
            if (*p != ' ') joined.push_back(*p);
        }
    }
    CHECK_STR(joined.c_str(), "aaaabbbbccccddddeeeeffff");
}

TEST(builtin, spacing_negative_advances_one_row) {
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{0.0f, 0.0f, 300.0f, 300.0f}), 1);
    ui.text("a");
    const float y0 = ui.last_item_rect().y;
    ui.spacing(0.0f);                    // 0 = 空操作
    ui.text("b");
    const float y1 = ui.last_item_rect().y;
    ui.spacing(-1.0f);                   // <0 = 用主题行间距（GUi_Spacing 的语义）
    ui.text("c");
    const float y2 = ui.last_item_rect().y;
    ui.end_window();
    ui.end_frame();

    CHECK_NEAR(y1 - y0, 16.25f + 4.0f, 1e-4f);
    CHECK_NEAR(y2 - y1, 16.25f + 4.0f + 4.0f, 1e-4f);
}

TEST(builtin, capture_flags_follow_mouse_and_focus) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    char buf[32] = {0};

    // 鼠标在面板里 -> wants_mouse；还没聚焦输入框 -> 不吃键盘
    bi_frame(ui, mouse_at(100.0f, 20.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.text("no inputs yet");
    ui.end_window();
    ui.end_frame();
    CHECK_TRUE(ui.wants_mouse());
    CHECK_EQ(ui.tree().focus_id(), 0u);
    CHECK_FALSE(ui.wants_keyboard());

    // 点一下输入框 -> 焦点落在它身上 -> wants_keyboard
    bi_frame(ui, mouse_at(100.0f, 20.0f, true, true, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.input_text("Name", buf, (int)sizeof(buf)), 0);
    ui.end_window();
    ui.end_frame();
    CHECK_TRUE(ui.tree().focus_id() != 0);
    CHECK_TRUE(ui.wants_keyboard());

    // 鼠标移出面板 -> wants_mouse 假；焦点还在，键盘仍被 UI 吃
    bi_frame(ui, mouse_at(500.0f, 500.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.input_text("Name", buf, (int)sizeof(buf));
    ui.end_window();
    ui.end_frame();
    CHECK_FALSE(ui.wants_mouse());
    CHECK_TRUE(ui.wants_keyboard());     // 焦点还在，键盘仍被 UI 吃
}

TEST(builtin, draw_orders_chrome_first_and_clips_content) {
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{10.0f, 10.0f, 200.0f, 100.0f}), 1);
    ui.text("a");
    ui.button("B");
    ui.end_window();
    ui.end_frame();

    ApproxMetrics metrics;
    gryce_ui::UiDrawList dl;
    ui.draw(dl);

    // chrome（圆角渐变底 + 圆角描边）在前，然后一个窗口裁剪，最后 pop
    CHECK_TRUE(dl.command(0).cmd == gryce_ui::UiCmd::RoundRect);
    CHECK_TRUE(dl.command(0).color2 != 0u);                       // 上亮下暗的渐变
    CHECK_EQ(dl.command(1).cmd, gryce_ui::UiCmd::RoundRectBorder); // 描边紧随填充
    CHECK_TRUE(dl.command(0).radii[0] > 0.0f);                    // 圆角生效
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::PushClip), 1);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::PopClip), 1);
    CHECK_TRUE(dl.command(dl.command_count() - 1).cmd == gryce_ui::UiCmd::PopClip);
    CHECK_EQ(dl.clip_depth(), 0);              // 绘制后裁剪栈归零
    // chrome 2 条 + Label 1 条文本 + Button（渐变底 + 描边 + 文本）3 条 + pop 1 条
    CHECK_EQ(dl.command_count(), 2 + 1 + 3 + 2);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::RoundRect), 2);        // chrome + button
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::RoundRectBorder), 2);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::Text), 2);
}

TEST(builtin, content_is_clipped_to_window) {
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{10.0f, 10.0f, 200.0f, 100.0f}), 1);
    ui.text("a");
    ui.text("b");
    ui.end_window();
    ui.end_frame();

    ApproxMetrics metrics;
    gryce_ui::UiDrawList dl;
    ui.draw(dl);
    // 内容裁剪域 = 窗口矩形内缩 PAD_X/PAD_Y（渲染桥据此设 scissor；
    // 构建期不按条裁剪，超出部分由 scissor 在渲染时解决）
    bool found_clip = false;
    for (int i = 0; i < dl.command_count(); ++i) {
        const gryce_ui::UiCommand& c = dl.command(i);
        if (c.cmd == gryce_ui::UiCmd::PushClip) {
            CHECK_NEAR(c.x, 18.0f, 1e-5f);
            CHECK_NEAR(c.y, 15.0f, 1e-5f);
            CHECK_NEAR(c.w, 184.0f, 1e-5f);
            CHECK_NEAR(c.h, 90.0f, 1e-5f);
            found_clip = true;
        }
    }
    CHECK_TRUE(found_clip);

    // 退化窗口（比内边距还小）：内容区高度 0 → 整批绘制命令被裁，chrome 除外
    ui.tree().clear();
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{10.0f, 10.0f, 200.0f, 10.0f}), 1);
    ui.text("a");
    ui.text("b");
    ui.end_window();
    ui.end_frame();
    dl.reset();
    ui.draw(dl);
    CHECK_EQ(dl.culled(), 2u);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::Text), 0);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::PushClip), 1);   // 裁剪命令照常记录
    CHECK_EQ(dl.clip_depth(), 0);
}

TEST(builtin, items_without_window_are_noops) {
    BuiltinUi ui;
    bi_frame(ui, mouse_at(100.0f, 20.0f, true, true, false));
    ui.text("x");
    CHECK_TRUE(ui.last_item_rect().w == 0.0f && ui.last_item_rect().h == 0.0f);
    CHECK_FALSE(ui.is_item_hovered());
    CHECK_EQ(ui.button("y"), 0);
    CHECK_FALSE(ui.is_item_active());
    int v = 0;
    CHECK_EQ(ui.checkbox("z", &v), 0);
    char buf[8] = "";
    CHECK_EQ(ui.input_text("t", buf, sizeof(buf)), 0);
    float f = 0.0f;
    CHECK_EQ(ui.slider_float("s", &f, 0.0f, 1.0f), 0);
    ui.separator();
    ui.progress_bar(0.5f, "x");
    ui.color_button("c", nullptr);
    CHECK_EQ(ui.tree().count(), 0);
    ui.end_frame();
}

TEST(builtin, same_label_different_windows_independent) {
    BuiltinUi ui;
    const Rect wa{0.0f, 0.0f, 200.0f, 200.0f};
    const Rect wb{0.0f, 300.0f, 200.0f, 200.0f};
    int va = 0;
    int vb = 0;

    auto submit = [&](int* pa, int* pb) {
        CHECK_EQ(ui.begin_window("A", wa), 1);
        ui.checkbox("X", pa);
        ui.end_window();
        CHECK_EQ(ui.begin_window("B", wb), 1);
        ui.checkbox("X", pb);
        ui.end_window();
    };

    // 点 A 的复选框（y 15..41.25），B 的在 y 315..341.25
    bi_frame(ui, mouse_at(50.0f, 20.0f, true, true, false));
    submit(&va, &vb);
    ui.end_frame();
    bi_frame(ui, mouse_at(50.0f, 20.0f, false, false, true));
    submit(&va, &vb);
    ui.end_frame();

    CHECK_EQ(va, 1);
    CHECK_EQ(vb, 0);
}

TEST(builtin, sweep_reclaims_unsubmitted) {
    BuiltinUi ui;
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", Rect{0.0f, 0.0f, 300.0f, 200.0f}), 1);
    ui.text("a");
    ui.button("B");
    ui.end_window();
    ui.end_frame();
    CHECK_EQ(ui.tree().count(), 3);      // Panel + Label + Button

    // 下一帧什么都不提交：即时模式的树被回收
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    ui.end_frame();
    CHECK_EQ(ui.tree().count(), 0);
}

TEST(builtin, last_item_queries) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    bi_frame(ui, mouse_at(20.0f, 20.0f, false, false, false));   // 悬停在按钮上（x∈[8,37]）
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.button("OK"), 0);
    CHECK_TRUE(ui.is_item_hovered());
    CHECK_FALSE(ui.is_item_active());
    const Rect r = ui.last_item_rect();
    CHECK_NEAR(r.x, 8.0f, 1e-5f);
    CHECK_NEAR(r.y, 5.0f, 1e-5f);      // 窗口 y=0：内容从 PAD_Y=5 开始
    ui.end_window();
    ui.end_frame();
}

// ===========================================================================
// suite: theme —— 主题 token
// ===========================================================================

TEST(theme, default_dark_preset) {
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_TEXT), 0xFFFFFFFFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0x202020FFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0x4CC2FFFFu);
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_ROW_H), 22.0f, 1e-4f);
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_INDENT), 18.0f, 1e-4f);
}

TEST(theme, light_preset_swaps_and_back) {
    GUi_ThemeSetPreset(GUI_THEME_PRESET_LIGHT);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0xF3F3F3FFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_TEXT), 0x1B1B1BFFu);
    // 度量与预设无关
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_ROW_H), 22.0f, 1e-4f);
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0x202020FFu);
}

TEST(theme, per_token_override_and_reset) {
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    GUi_ThemeSetColor(GUI_THEME_COLOR_ACCENT, 0xFF0000FFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0xFF0000FFu);
    // 换预设整体重置（个别覆盖不跨预设存活）
    GUi_ThemeSetPreset(GUI_THEME_PRESET_LIGHT);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0x005FB8FFu);

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
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_TEXT), 0xFFFFFFFFu);   // 未被误改

    // 未知 preset 回落深色（调用方传错值时行为仍确定）
    GUi_ThemeSetPreset(42);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0x202020FFu);
}

TEST(theme, apply_is_safe_without_imgui) {
    // 本测试可执行不定义 GRYCE_HAS_IMGUI：apply() 退化为 no-op，token 不变
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    const uint32_t before = GUi_ThemeColor(GUI_THEME_COLOR_TEXT);
    GUi_ThemeApply();
    GUi_ThemeApply();
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_TEXT), before);
}

TEST(theme, preset_count_and_names) {
    CHECK_EQ(GUi_ThemePresetCount(), GUI_THEME_PRESET_COUNT);
    CHECK_EQ(GUi_ThemePresetCount(), 5);
    char a[32] = {};
    CHECK_EQ(GUi_ThemePresetName(GUI_THEME_PRESET_DARK, a, sizeof(a)), 4);
    CHECK_TRUE(std::strcmp(a, "Dark") == 0);
    CHECK_EQ(GUi_ThemePresetName(GUI_THEME_PRESET_HIGH_CONTRAST, a, sizeof(a)), 12);
    CHECK_TRUE(std::strcmp(a, "HighContrast") == 0);
    // 越界：写空串、返回 0
    char bad[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', '\0'};
    CHECK_EQ(GUi_ThemePresetName(GUI_THEME_PRESET_COUNT, bad, sizeof(bad)), 0);
    CHECK_EQ(bad[0], '\0');
    CHECK_EQ(GUi_ThemePresetName(-1, nullptr, 0), 0);
}

TEST(theme, new_presets_anchors) {
    GUi_ThemeSetPreset(GUI_THEME_PRESET_MIDNIGHT);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0x141822FFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0x6EA8FEFFu);
    GUi_ThemeSetPreset(GUI_THEME_PRESET_NORD);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0x2E3440FFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ERROR), 0xBF616AFFu);
    GUi_ThemeSetPreset(GUI_THEME_PRESET_HIGH_CONTRAST);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0x000000FFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_TEXT), 0xFFFFFFFFu);
    // 度量与预设无关（几何不分主题）
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_ROW_H), 22.0f, 1e-4f);
}

TEST(theme, derived_tokens_consistent_across_presets) {
    // 派生规则固定：换任何预设这些等式都成立。哪天某套预设开始手工抄派生值，
    // 这条用例就会红——那是"换预设漂移半个界面"的前兆。
    for (int p = 0; p < GUI_THEME_PRESET_COUNT; ++p) {
        GUi_ThemeSetPreset(p);
        CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_CHECK_MARK),
                 GUi_ThemeColor(GUI_THEME_COLOR_ACCENT));
        CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_SLIDER_GRAB),
                 GUi_ThemeColor(GUI_THEME_COLOR_ACCENT));
        CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_SLIDER_GRAB_ACTIVE),
                 GUi_ThemeColor(GUI_THEME_COLOR_ACCENT_ACTIVE));
        CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_SEPARATOR),
                 GUi_ThemeColor(GUI_THEME_COLOR_BORDER));
        CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_TITLE_BG),
                 GUi_ThemeColor(GUI_THEME_COLOR_HEADER));
        CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_MENUBAR_BG),
                 GUi_ThemeColor(GUI_THEME_COLOR_HEADER));
        CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_POPUP_BG),
                 GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG));
        CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_SCROLLBAR_GRAB_HOVERED),
                 GUi_ThemeColor(GUI_THEME_COLOR_BUTTON_HOVER));
        CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_SCROLLBAR_GRAB_ACTIVE),
                 GUi_ThemeColor(GUI_THEME_COLOR_BUTTON_ACTIVE));
        // 派生 token 非零（0 会被当成"越界读"的返回值，界面表现是缺色/透明）
        CHECK_TRUE(GUi_ThemeColor(GUI_THEME_COLOR_SCROLLBAR_BG) != 0u);
        CHECK_TRUE(GUi_ThemeColor(GUI_THEME_COLOR_TABLE_ROW_BG_ALT) != 0u);
        CHECK_TRUE(GUi_ThemeColor(GUI_THEME_COLOR_TAB_SELECTED) != 0u);
        CHECK_TRUE(GUi_ThemeColor(GUI_THEME_COLOR_NAV_WINDOWING_DIM_BG) != 0u);
        CHECK_TRUE(GUi_ThemeColor(GUI_THEME_COLOR_MODAL_WINDOW_DIM_BG) != 0u);
        CHECK_TRUE(GUi_ThemeColor(GUI_THEME_COLOR_CONTROL_BORDER) != 0u);
        // 控件描边必须与 BORDER 区分开：退化成同一个值时，控件又变成一圈亮线
        CHECK_TRUE(GUi_ThemeColor(GUI_THEME_COLOR_CONTROL_BORDER) !=
                   GUi_ThemeColor(GUI_THEME_COLOR_BORDER));
    }
}

TEST(theme, rounding_metrics_present) {
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_WINDOW_ROUNDING), 8.0f, 1e-4f);
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_FRAME_ROUNDING), 4.0f, 1e-4f);
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_GRAB_ROUNDING), 4.0f, 1e-4f);
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_SCROLLBAR_SIZE), 12.0f, 1e-4f);
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_GRAB_MIN_SIZE), 8.0f, 1e-4f);
    // 越界度量读返回 0（既有约定不变）
    CHECK_EQ(GUi_ThemeMetric(GUI_THEME_METRIC_COUNT), 0.0f);
}

// ===========================================================================
// suite: theme_json —— JSON 主题加载（C API 路径）
// ===========================================================================

TEST(theme_json, load_full_replaces_anchors_metrics_and_animation) {
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    const char* json = "{"
        "\"name\":\"Test Fluent\","
        "\"anchors\":{\"window_bg\":\"#111111\",\"accent\":\"#22AAFF\",\"text\":\"#EEEEEE\"},"
        "\"metrics\":{\"window_rounding\":7.0,\"frame_rounding\":3.0},"
        "\"animation\":{\"enabled\":true,\"hover_ms\":200,\"press_ms\":80,"
        "               \"ease\":\"out_quad\"}"
        "}";
    CHECK_EQ(GUi_ThemeLoadFromJson(json), 1);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0x111111FFu);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0x22AAFFFFu);
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_WINDOW_ROUNDING), 7.0f, 1e-4f);
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_FRAME_ROUNDING), 3.0f, 1e-4f);
    const gryce_ui::ThemeAnim& a = gryce_ui::theme_anim();
    CHECK_TRUE(a.enabled);
    CHECK_NEAR(a.hover_s, 0.200f, 1e-4f);
    CHECK_NEAR(a.press_s, 0.080f, 1e-4f);
    CHECK_EQ(a.ease, static_cast<int>(gryce_ui::AnimEase::EaseOutQuad));
    // 动画参数是进程内全局状态，别泄漏给后面的用例
    gryce_ui::set_theme_anim(gryce_ui::ThemeAnim{});
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
}

TEST(theme_json, partial_file_keeps_derived_colors) {
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    const char* json = "{\"anchors\":{\"window_bg\":\"#101010\"}}";
    CHECK_EQ(GUi_ThemeLoadFromJson(json), 1);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), 0x101010FFu);
    // 未列出的锚色回落出厂深色，派生色照常生成（非 0 = 没读到野数据）
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0x4CC2FFFFu);
    CHECK_TRUE(GUi_ThemeColor(GUI_THEME_COLOR_CONTROL_BORDER) != 0u);
    CHECK_TRUE(GUi_ThemeColor(GUI_THEME_COLOR_MODAL_WINDOW_DIM_BG) != 0u);
}

TEST(theme_json, color_override_beats_derivation) {
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    const char* json = "{\"colors\":{\"control_border\":\"#123456\"}}";
    CHECK_EQ(GUi_ThemeLoadFromJson(json), 1);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_CONTROL_BORDER), 0x123456FFu);
}

TEST(theme_json, bad_input_rejected_and_theme_unchanged) {
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    const uint32_t before = GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG);
    CHECK_EQ(GUi_ThemeLoadFromJson(""), 0);
    CHECK_EQ(GUi_ThemeLoadFromJson("not json {"), 0);
    CHECK_EQ(GUi_ThemeLoadFromJson("[1,2,3]"), 0);   // 合法 JSON 但不是对象
    CHECK_EQ(GUi_ThemeLoadFromJson(nullptr), 0);
    // 非法输入一律不动当前主题
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_WINDOW_BG), before);
}

TEST(theme_json, shipped_files_load) {
    // assets/themes 的四套皮肤是交付物：加载失败 = 文件被改坏，必须叫。
    // 路径走编译期烙入的源目录（ctest 以 build/ 为 cwd，相对路径找不到）。
    const std::string dir = GRYCE_UI_THEMES_DIR;
    CHECK_EQ(GUi_ThemeLoadFromFile((dir + "/fluent-dark.json").c_str()), 1);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0x4CC2FFFFu);
    CHECK_TRUE(gryce_ui::theme_anim().enabled);
    CHECK_EQ(GUi_ThemeLoadFromFile((dir + "/fluent-light.json").c_str()), 1);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0x005FB8FFu);
    CHECK_EQ(GUi_ThemeLoadFromFile((dir + "/flex-blender-dark.json").c_str()), 1);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0xED9E5CFFu);
    CHECK_NEAR(GUi_ThemeMetric(GUI_THEME_METRIC_WINDOW_ROUNDING), 5.0f, 1e-4f);
    CHECK_EQ(GUi_ThemeLoadFromFile((dir + "/flex-blender-light.json").c_str()), 1);
    CHECK_EQ(GUi_ThemeColor(GUI_THEME_COLOR_ACCENT), 0x2E6BD6FFu);
    CHECK_EQ(GUi_ThemeLoadFromFile((dir + "/nope.json").c_str()), 0);
    gryce_ui::set_theme_anim(gryce_ui::ThemeAnim{});
    gryce_ui::set_theme_style(gryce_ui::ThemeStyle{});
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
}

// style 块把"怎么画"也变成主题数据：Fluent 扁平（fill_grad=0 + 强投影）与
// Flex/Blender 光泽（fill_grad=0.12 + 弱投影）必须是同一套绘制代码的两个数据点。
TEST(theme_json, style_block_drives_texture) {
    // 出厂默认：Fluent 之前的旧默认值
    const gryce_ui::ThemeStyle def = gryce_ui::theme_style();
    CHECK_TRUE(def.fill_grad > 0.0f);

    // 扁平档：渐变归零、投影拉满
    CHECK_EQ(gryce_ui::load_theme_json(R"({"style":{"fill_grad":0,"panel_grad":0,
                                                   "shadow_alpha":0.5,"shadow_spread":26}})"), 1);
    CHECK_NEAR(gryce_ui::theme_style().fill_grad, 0.0f, 1e-5f);
    CHECK_NEAR(gryce_ui::theme_style().panel_grad, 0.0f, 1e-5f);
    CHECK_NEAR(gryce_ui::theme_style().shadow_alpha, 0.5f, 1e-5f);
    CHECK_NEAR(gryce_ui::theme_style().shadow_spread, 26.0f, 1e-5f);

    // 光泽档：渐变起来、投影收住
    CHECK_EQ(gryce_ui::load_theme_json(R"({"style":{"fill_grad":0.12,"panel_grad":0.05,
                                                   "shadow_alpha":0.3,"shadow_spread":10}})"), 1);
    CHECK_NEAR(gryce_ui::theme_style().fill_grad, 0.12f, 1e-5f);
    CHECK_NEAR(gryce_ui::theme_style().panel_grad, 0.05f, 1e-5f);

    // 部分覆盖：只写 fill_grad，其余保持上一文件的值
    CHECK_EQ(gryce_ui::load_theme_json(R"({"style":{"fill_grad":0.2}})"), 1);
    CHECK_NEAR(gryce_ui::theme_style().fill_grad, 0.2f, 1e-5f);
    CHECK_NEAR(gryce_ui::theme_style().shadow_spread, 10.0f, 1e-5f);

    // 钳制：负渐变（画成凹坑）与 >1 的 alpha 都被夹回合法域
    CHECK_EQ(gryce_ui::load_theme_json(R"({"style":{"fill_grad":-0.5,"shadow_alpha":2.0}})"), 1);
    CHECK_NEAR(gryce_ui::theme_style().fill_grad, 0.0f, 1e-5f);
    CHECK_NEAR(gryce_ui::theme_style().shadow_alpha, 1.0f, 1e-5f);

    // 换预设整体重置质感（与"覆盖不跨预设存活"同语义）
    GUi_ThemeSetPreset(GUI_THEME_PRESET_LIGHT);
    CHECK_NEAR(gryce_ui::theme_style().fill_grad, def.fill_grad, 1e-5f);
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
}

// ===========================================================================
// suite: widget_anim —— 状态过渡动画（主题 animation 块驱动）
// ===========================================================================

TEST(widget_anim, hover_transition_lerps_and_lands) {
    gryce_ui::set_theme_anim({true, 0.20f, 0.10f, 5});   // hover 200ms / out_cubic
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    const gryce_ui::Theme& theme = gryce_ui::theme();

    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 100.0f, 30.0f};
    const uint32_t normal = theme.colors[gryce_ui::THEME_COLOR_BUTTON];
    const uint32_t hover  = theme.colors[gryce_ui::THEME_COLOR_BUTTON_HOVER];

    // 第 1 帧：鼠标在按钮上 → 首帧直接落位 hover 目标
    tree.begin_frame(mouse_at(50.0f, 20.0f, false, false, false));
    Widget* w = tree.touch(1, WidgetKind::Button, r, "OK");
    CHECK_TRUE(w->hovered);
    tree.advance_animations(0.0f, theme);
    CHECK_EQ(w->anim_to, hover);
    tree.end_frame();

    // 第 2 帧：鼠标移开 → 目标切回常态，从 hover 色往回走到一半
    tree.begin_frame(mouse_at(400.0f, 400.0f, false, false, false));
    w = tree.touch(1, WidgetKind::Button, r, "OK");
    CHECK_FALSE(w->hovered);
    tree.advance_animations(0.1f, theme);

    ApproxMetrics metrics;
    gryce_ui::UiDrawList dl;
    tree.draw(dl, theme, metrics);
    const gryce_ui::UiCommand* fill = first_cmd(dl, gryce_ui::UiCmd::RoundRect);
    CHECK_TRUE(fill != nullptr);
    CHECK_TRUE(fill->color != shade_top_4(normal));      // 还在半路
    CHECK_TRUE(fill->color != shade_top_4(hover));
    tree.end_frame();

    // 时间走满：精确落到常态色（端点不受缓动形状影响）。注意填充命令装的是
    // shade(bg,+4%) 的渐变端点——它恰好等于 hover token 原值，别直接拿 token 比
    tree.begin_frame(mouse_at(400.0f, 400.0f, false, false, false));
    tree.touch(1, WidgetKind::Button, r, "OK");
    tree.advance_animations(1.0f, theme);
    dl.reset();
    tree.draw(dl, theme, metrics);
    fill = first_cmd(dl, gryce_ui::UiCmd::RoundRect);
    CHECK_TRUE(fill != nullptr);
    CHECK_EQ(fill->color, shade_top_4(normal));
    tree.end_frame();

    gryce_ui::set_theme_anim(gryce_ui::ThemeAnim{});
}

TEST(widget_anim, disabled_snaps_to_static_path) {
    gryce_ui::set_theme_anim(gryce_ui::ThemeAnim{});     // enabled = false
    GUi_ThemeSetPreset(GUI_THEME_PRESET_DARK);
    const gryce_ui::Theme& theme = gryce_ui::theme();

    WidgetTree tree;
    const Rect r{10.0f, 10.0f, 100.0f, 30.0f};
    tree.begin_frame(mouse_at(50.0f, 20.0f, false, false, false));
    Widget* w = tree.touch(1, WidgetKind::Button, r, "OK");
    tree.advance_animations(0.016f, theme);
    CHECK_EQ(w->anim_to, 0u);               // 未进入动画集合
    ApproxMetrics metrics;
    gryce_ui::UiDrawList dl;
    tree.draw(dl, theme, metrics);
    const gryce_ui::UiCommand* fill = first_cmd(dl, gryce_ui::UiCmd::RoundRect);
    CHECK_TRUE(fill != nullptr);
    // 静态路径：填充 = shade(hover 目标, +4%) 的渐变端点，不是 token 原值
    CHECK_EQ(fill->color, shade_top_4(theme.colors[gryce_ui::THEME_COLOR_BUTTON_HOVER]));
    tree.end_frame();
}

// ===========================================================================
// suite: anim —— 动画系统（块一纯函数层 + 块二运行时）
// ===========================================================================

TEST(anim, ease_boundaries) {
    for (int i = 0; i < gryce_ui::kAnimEaseCount; ++i) {
        const gryce_ui::AnimEase e = static_cast<gryce_ui::AnimEase>(i);
        // 边界对齐：所有缓动 f(0)=0、f(1)=1（回弹类也如此）
        CHECK_NEAR(gryce_ui::anim_ease(e, 0.0f), 0.0f, 1e-6f);
        CHECK_NEAR(gryce_ui::anim_ease(e, 1.0f), 1.0f, 1e-6f);
        // t 夹到 [0,1]：负值与超界不泄漏到区间外
        CHECK_NEAR(gryce_ui::anim_ease(e, -3.0f), 0.0f, 1e-6f);
        CHECK_NEAR(gryce_ui::anim_ease(e, 7.0f), 1.0f, 1e-6f);
    }
    CHECK_NEAR(gryce_ui::anim_ease(gryce_ui::AnimEase::Linear, 0.5f), 0.5f, 1e-6f);
    CHECK_NEAR(gryce_ui::anim_ease(gryce_ui::AnimEase::EaseInQuad, 0.5f), 0.25f, 1e-6f);
    CHECK_NEAR(gryce_ui::anim_ease(gryce_ui::AnimEase::EaseOutQuad, 0.5f), 0.75f, 1e-6f);
    CHECK_NEAR(gryce_ui::anim_ease(gryce_ui::AnimEase::EaseInOutCubic, 0.5f), 0.5f, 1e-6f);
    // 回弹类允许过冲，但必须回落到 1
    CHECK_TRUE(gryce_ui::anim_ease(gryce_ui::AnimEase::EaseOutBack, 0.7f) > 1.0f);
    CHECK_NEAR(gryce_ui::anim_ease(gryce_ui::AnimEase::EaseOutBack, 1.0f), 1.0f, 1e-6f);
}

TEST(anim, ease_names_unique) {
    for (int i = 0; i < gryce_ui::kAnimEaseCount; ++i) {
        const char* n = gryce_ui::anim_ease_name(static_cast<gryce_ui::AnimEase>(i));
        CHECK_TRUE(n && n[0] != '\0');
        for (int j = i + 1; j < gryce_ui::kAnimEaseCount; ++j) {
            CHECK_TRUE(std::strcmp(n, gryce_ui::anim_ease_name(
                                          static_cast<gryce_ui::AnimEase>(j))) != 0);
        }
    }
    CHECK_EQ(std::strcmp(gryce_ui::anim_ease_name(
                             static_cast<gryce_ui::AnimEase>(gryce_ui::kAnimEaseCount + 3)),
                         ""),
             0);
}

TEST(anim, tween_delay_and_bounds) {
    gryce_ui::AnimTween tw;
    tw.from = 10.0f;
    tw.to = 110.0f;
    tw.duration = 1.0f;
    tw.delay = 0.5f;
    tw.ease = gryce_ui::AnimEase::Linear;
    CHECK_NEAR(tw.value_at(0.0f), 10.0f, 1e-5f);    // 延迟期间不动
    CHECK_NEAR(tw.value_at(0.5f), 10.0f, 1e-5f);    // 恰在 delay 上仍是起点
    CHECK_NEAR(tw.value_at(1.0f), 60.0f, 1e-5f);    // delay+0.5s -> 线性中点
    CHECK_NEAR(tw.value_at(1.5f), 110.0f, 1e-5f);   // 精确落到终点
    CHECK_NEAR(tw.value_at(9.0f), 110.0f, 1e-5f);   // 超界保持终点
    tw.ease = gryce_ui::AnimEase::EaseInQuad;
    CHECK_NEAR(tw.value_at(1.0f), 35.0f, 1e-5f);    // 10 + 100*0.25
}

TEST(anim, tween_zero_duration_is_instant) {
    gryce_ui::AnimTween tw;
    tw.from = 0.0f;
    tw.to = 5.0f;
    tw.duration = 0.0f;
    CHECK_NEAR(tw.value_at(0.0f), 5.0f, 1e-6f);
    CHECK_NEAR(tw.value_at(-1.0f), 5.0f, 1e-6f);
}

TEST(anim, color_lerp_clamps) {
    const gryce_ui::AnimColor a{0.0f, 0.0f, 0.0f, 1.0f};
    const gryce_ui::AnimColor b{1.0f, 0.5f, 0.25f, 0.5f};
    const gryce_ui::AnimColor mid = gryce_ui::anim_color_lerp(a, b, 0.5f);
    CHECK_NEAR(mid.r, 0.5f, 1e-6f);
    CHECK_NEAR(mid.g, 0.25f, 1e-6f);
    CHECK_NEAR(mid.b, 0.125f, 1e-6f);
    CHECK_NEAR(mid.a, 0.75f, 1e-6f);
    CHECK_NEAR(gryce_ui::anim_color_lerp(a, b, -1.0f).r, 0.0f, 1e-6f);
    CHECK_NEAR(gryce_ui::anim_color_lerp(a, b, 2.0f).r, 1.0f, 1e-6f);
}

TEST(anim, track_insert_sorted_and_overwrite) {
    gryce_ui::AnimTrack tr;
    CHECK_TRUE(tr.add(1.0f, 10.0f, gryce_ui::AnimEase::Linear));
    CHECK_TRUE(tr.add(0.0f, 0.0f, gryce_ui::AnimEase::Linear));    // 插到最前
    CHECK_TRUE(tr.add(2.0f, -5.0f, gryce_ui::AnimEase::Linear));
    CHECK_TRUE(tr.add(1.0f, 7.0f, gryce_ui::AnimEase::Linear));    // 同刻覆盖
    CHECK_EQ(tr.count(), 3);
    CHECK_NEAR(tr.duration(), 2.0f, 1e-6f);
    CHECK_NEAR(tr.eval(1.0f), 7.0f, 1e-6f);     // 覆盖后的值
    CHECK_NEAR(tr.eval(0.5f), 3.5f, 1e-6f);     // 0 -> 7 线性中点
}

TEST(anim, track_eval_holds_outside_range) {
    gryce_ui::AnimTrack tr;
    tr.add(0.5f, 2.0f, gryce_ui::AnimEase::Linear);
    tr.add(1.5f, 4.0f, gryce_ui::AnimEase::Linear);
    CHECK_NEAR(tr.eval(0.0f), 2.0f, 1e-6f);     // 首帧之前保持首帧值
    CHECK_NEAR(tr.eval(0.5f), 2.0f, 1e-6f);
    CHECK_NEAR(tr.eval(1.0f), 3.0f, 1e-6f);
    CHECK_NEAR(tr.eval(1.5f), 4.0f, 1e-6f);
    CHECK_NEAR(tr.eval(9.0f), 4.0f, 1e-6f);     // 尾帧之后保持尾帧值
    gryce_ui::AnimTrack empty;
    CHECK_NEAR(empty.eval(1.0f), 0.0f, 1e-6f);  // 空轨道
    CHECK_EQ(empty.count(), 0);
}

TEST(anim, track_segment_ease_uses_previous_key) {
    gryce_ui::AnimTrack tr;
    tr.add(0.0f, 0.0f, gryce_ui::AnimEase::EaseInQuad);     // 本段用 InQuad
    tr.add(1.0f, 100.0f, gryce_ui::AnimEase::Linear);
    CHECK_NEAR(tr.eval(0.5f), 25.0f, 1e-4f);                // 0.25*100，不是 50
    tr.add(2.0f, 0.0f, gryce_ui::AnimEase::Linear);         // 第二段 Linear
    CHECK_NEAR(tr.eval(1.5f), 50.0f, 1e-4f);
}

TEST(anim, track_capacity) {
    gryce_ui::AnimTrack tr;
    for (int i = 0; i < gryce_ui::AnimTrack::kMaxKeys; ++i) {
        CHECK_TRUE(tr.add(static_cast<float>(i), 1.0f, gryce_ui::AnimEase::Linear));
    }
    CHECK_FALSE(tr.add(99.0f, 1.0f, gryce_ui::AnimEase::Linear));   // 满：拒绝
    CHECK_EQ(tr.count(), gryce_ui::AnimTrack::kMaxKeys);
}

TEST(anim, clip_duration_and_track_replace) {
    gryce_ui::AnimClip clip;
    CHECK_NEAR(clip.computed_duration(), 0.0f, 1e-6f);      // 空片段
    gryce_ui::AnimClipTrack t{};
    std::strncpy(t.channel, "x", sizeof(t.channel) - 1);
    t.keys.add(0.0f, 0.0f, gryce_ui::AnimEase::Linear);
    t.keys.add(0.5f, 50.0f, gryce_ui::AnimEase::Linear);
    CHECK_TRUE(clip.add_track(t));
    gryce_ui::AnimClipTrack c{};
    std::strncpy(c.channel, "tint", sizeof(c.channel) - 1);
    c.is_color = 1;
    c.duration = 0.25f;
    c.delay = 0.25f;
    CHECK_TRUE(clip.add_track(c));
    CHECK_NEAR(clip.computed_duration(), 0.5f, 1e-6f);      // max(0.5, 0.25+0.25)
    CHECK_EQ(clip.track_count(), 2);
    // 同名通道整体覆盖（位置不变、计数不变）
    gryce_ui::AnimClipTrack t2 = t;
    t2.keys.clear();
    t2.keys.add(0.0f, 9.0f, gryce_ui::AnimEase::Linear);
    CHECK_TRUE(clip.add_track(t2));
    CHECK_EQ(clip.track_count(), 2);
    CHECK_NEAR(clip.track(0).keys.key(0).v, 9.0f, 1e-6f);
    clip.duration = 3.0f;
    CHECK_NEAR(clip.computed_duration(), 3.0f, 1e-6f);      // 显式时长优先
}

// 测试用片段：标量通道 "x"，0 -> 100，1 秒线性。
gryce_ui::AnimClip make_slide_clip(gryce_ui::AnimLoop loop) {
    gryce_ui::AnimClip clip;
    std::strncpy(clip.name, "slide", sizeof(clip.name) - 1);
    clip.loop = loop;
    gryce_ui::AnimClipTrack t{};
    std::strncpy(t.channel, "x", sizeof(t.channel) - 1);
    t.keys.add(0.0f, 0.0f, gryce_ui::AnimEase::Linear);
    t.keys.add(1.0f, 100.0f, gryce_ui::AnimEase::Linear);
    clip.add_track(t);
    return clip;
}

TEST(anim, animator_once_play_and_finish) {
    const gryce_ui::AnimClip clip = make_slide_clip(gryce_ui::AnimLoop::Once);
    gryce_ui::Animator a;
    a.play(clip, true);
    CHECK_TRUE(a.is_playing());
    CHECK_FALSE(a.is_finished());
    float x = -1.0f;
    CHECK_TRUE(a.channel_float("x", &x));
    CHECK_NEAR(x, 0.0f, 1e-6f);                  // play 立即评估
    a.advance(0.5f);
    CHECK_TRUE(a.channel_float("x", &x));
    CHECK_NEAR(x, 50.0f, 1e-6f);
    a.advance(0.5f);
    CHECK_FALSE(a.is_playing());
    CHECK_TRUE(a.is_finished());
    CHECK_TRUE(a.channel_float("x", &x));
    CHECK_NEAR(x, 100.0f, 1e-6f);                // 停在终点
    CHECK_FALSE(a.channel_float("nope", &x));    // 没有的通道
    CHECK_FALSE(a.channel_color("x", nullptr));  // 标量通道不当颜色读
}

TEST(anim, animator_loop_and_pingpong) {
    float x = 0.0f;
    gryce_ui::Animator a;
    a.play(make_slide_clip(gryce_ui::AnimLoop::Loop), true);
    a.advance(1.5f);
    CHECK_TRUE(a.channel_float("x", &x));
    CHECK_NEAR(x, 50.0f, 1e-6f);                 // 绕回中点
    CHECK_TRUE(a.is_playing());
    CHECK_FALSE(a.is_finished());

    a.play(make_slide_clip(gryce_ui::AnimLoop::PingPong), true);
    a.advance(1.5f);
    CHECK_TRUE(a.channel_float("x", &x));
    CHECK_NEAR(x, 50.0f, 1e-6f);                 // 折叠 1.5 -> 0.5
    a.advance(0.5f);                             // 累计 2.0 -> 折叠 0
    CHECK_TRUE(a.channel_float("x", &x));
    CHECK_NEAR(x, 0.0f, 1e-6f);
    CHECK_NEAR(a.time(), 2.0f, 1e-6f);           // time() 报未折叠的累计值
}

TEST(anim, animator_speed_seek_pause_stop) {
    gryce_ui::Animator a;
    a.set_speed(2.0f);
    a.play(make_slide_clip(gryce_ui::AnimLoop::Loop), true);
    a.advance(0.25f);
    CHECK_NEAR(a.time(), 0.5f, 1e-6f);           // 2x 速度
    CHECK_NEAR(a.speed(), 2.0f, 1e-6f);
    a.seek(0.25f);
    CHECK_NEAR(a.time(), 0.25f, 1e-6f);
    float x = 0.0f;
    CHECK_TRUE(a.channel_float("x", &x));
    CHECK_NEAR(x, 25.0f, 1e-6f);                 // seek 后立即评估
    a.pause();
    CHECK_FALSE(a.is_playing());
    a.advance(1.0f);
    CHECK_NEAR(a.time(), 0.25f, 1e-6f);          // 暂停中不推进
    a.resume();
    CHECK_TRUE(a.is_playing());
    a.advance(0.1f);
    CHECK_NEAR(a.time(), 0.45f, 1e-6f);
    a.stop();
    CHECK_FALSE(a.is_playing());
    CHECK_FALSE(a.is_finished());                // stop 不是"播完"
    CHECK_TRUE(a.channel_float("x", &x));
    CHECK_NEAR(x, 45.0f, 1e-6f);                 // 停止后通道保留最后的值
}

TEST(anim, animator_delay_holds_initial) {
    gryce_ui::AnimClip clip = make_slide_clip(gryce_ui::AnimLoop::Once);
    gryce_ui::AnimClipTrack late = clip.track(0);
    std::strncpy(late.channel, "b", sizeof(late.channel) - 1);
    late.delay = 0.5f;
    clip.add_track(late);

    gryce_ui::Animator a;
    a.play(clip, true);
    a.advance(0.25f);
    float x = 0.0f, b = -1.0f;
    CHECK_TRUE(a.channel_float("x", &x));
    CHECK_TRUE(a.channel_float("b", &b));
    CHECK_NEAR(x, 25.0f, 1e-6f);
    CHECK_NEAR(b, 0.0f, 1e-6f);                  // 延迟期间 = 首帧值
    a.advance(0.5f);                             // t = 0.75
    CHECK_TRUE(a.channel_float("b", &b));
    CHECK_NEAR(b, 25.0f, 1e-6f);                 // 0.75 - 0.5 = 0.25 处
}

TEST(anim, animator_color_channel) {
    gryce_ui::AnimClip clip;
    gryce_ui::AnimClipTrack t{};
    std::strncpy(t.channel, "tint", sizeof(t.channel) - 1);
    t.is_color = 1;
    t.duration = 1.0f;
    t.ease = gryce_ui::AnimEase::Linear;
    t.color_from.r = 1.0f;   // 红
    t.color_from.g = 0.0f;
    t.color_from.b = 0.0f;
    t.color_to.r = 0.0f;     // 蓝
    t.color_to.g = 0.0f;
    t.color_to.b = 1.0f;
    clip.add_track(t);

    gryce_ui::Animator a;
    a.play(clip, true);
    a.advance(0.5f);
    gryce_ui::AnimColor c{};
    CHECK_TRUE(a.channel_color("tint", &c));
    CHECK_NEAR(c.r, 0.5f, 1e-6f);
    CHECK_NEAR(c.g, 0.0f, 1e-6f);
    CHECK_NEAR(c.b, 0.5f, 1e-6f);
    float f = -1.0f;
    CHECK_FALSE(a.channel_float("tint", &f));    // 颜色通道不当标量读
    a.advance(0.5f);
    CHECK_TRUE(a.channel_color("tint", &c));
    CHECK_NEAR(c.b, 1.0f, 1e-6f);
}

TEST(anim, animator_channels_rebuild_on_play) {
    gryce_ui::AnimClip one = make_slide_clip(gryce_ui::AnimLoop::Once);
    gryce_ui::AnimClip two{};
    gryce_ui::AnimClipTrack t{};
    std::strncpy(t.channel, "y", sizeof(t.channel) - 1);
    t.keys.add(0.0f, 7.0f, gryce_ui::AnimEase::Linear);
    t.keys.add(1.0f, 7.0f, gryce_ui::AnimEase::Linear);
    two.add_track(t);

    gryce_ui::Animator a;
    a.play(one, true);
    CHECK_EQ(a.channel_count(), 1);
    a.advance(0.3f);
    // 换片段不重置时间（restart=false）：通道表整体换成新片段的
    a.play(two, false);
    CHECK_EQ(a.channel_count(), 1);
    CHECK_NEAR(a.time(), 0.3f, 1e-6f);
    float x = -1.0f, y = -1.0f;
    CHECK_FALSE(a.channel_float("x", &x));       // 旧片段通道不残留
    CHECK_TRUE(a.channel_float("y", &y));
    CHECK_NEAR(y, 7.0f, 1e-6f);
}

TEST(anim, animator_without_clip_is_noop) {
    gryce_ui::Animator a;
    a.advance(1.0f);
    CHECK_FALSE(a.is_playing());
    CHECK_FALSE(a.is_finished());
    CHECK_EQ(a.channel_count(), 0);
    a.pause();
    a.resume();
    a.stop();
    a.seek(1.0f);
    float x = -1.0f;
    CHECK_FALSE(a.channel_float("x", &x));
    gryce_ui::AnimColor c{};
    CHECK_FALSE(a.channel_color("x", &c));
}

// ===========================================================================
// suite: animabi —— 动画系统 C ABI（与 anim 套件分工：那边测模型，这边测
// C 结构体转换、枚举映射、进程内唯一播放器的读写闭环）
// ===========================================================================

TEST(animabi, ease_helpers_match_model) {
    for (int i = 0; i < GUI_ANIM_EASE_COUNT; ++i) {
        CHECK_NEAR(GUi_AnimEase(i, 0.25f),
                   gryce_ui::anim_ease(static_cast<gryce_ui::AnimEase>(i), 0.25f), 1e-6f);
        CHECK_NEAR(GUi_AnimEase(i, 0.9f),
                   gryce_ui::anim_ease(static_cast<gryce_ui::AnimEase>(i), 0.9f), 1e-6f);
    }
    // 越界枚举退回 Linear
    CHECK_NEAR(GUi_AnimEase(GUI_ANIM_EASE_COUNT + 3, 0.5f), 0.5f, 1e-6f);
    char name[32] = {};
    CHECK_EQ(GUi_AnimEaseName(GUI_ANIM_EASE_OUT_BOUNCE, name, sizeof(name)), 13);
    CHECK_TRUE(std::strcmp(name, "EaseOutBounce") == 0);
    CHECK_EQ(GUi_AnimEaseName(-1, name, sizeof(name)), 0);
    CHECK_EQ(name[0], '\0');
}

TEST(animabi, tween_and_color_helpers) {
    float v = 0.0f;
    GUi_AnimTween(0.0f, 100.0f, 1.0f, 0.5f, GUI_ANIM_EASE_LINEAR, 1.0f, &v);
    CHECK_NEAR(v, 50.0f, 1e-5f);
    GUi_AnimTween(0.0f, 100.0f, 1.0f, 0.5f, GUI_ANIM_EASE_LINEAR, 0.2f, &v);
    CHECK_NEAR(v, 0.0f, 1e-5f);                   // 延迟期间保持起点
    const float a[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float b[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float out[4] = {};
    GUi_AnimColorLerp(a, b, 0.25f, out);
    CHECK_NEAR(out[0], 0.25f, 1e-6f);
    CHECK_NEAR(out[3], 1.0f, 1e-6f);
}

TEST(animabi, play_channels_roundtrip) {
    GUiAnimClip clip{};
    std::strncpy(clip.name, "demo", sizeof(clip.name) - 1);
    clip.loop = GUI_ANIM_LOOP_ONCE;
    GUiAnimTrack tx{};
    std::strncpy(tx.channel, "x", sizeof(tx.channel) - 1);
    tx.key_count = 2;
    tx.keys[0].t = 0.0f;
    tx.keys[0].v = 0.0f;
    tx.keys[0].ease = GUI_ANIM_EASE_LINEAR;
    tx.keys[1].t = 1.0f;
    tx.keys[1].v = 100.0f;
    tx.keys[1].ease = GUI_ANIM_EASE_LINEAR;
    clip.tracks[0] = tx;
    GUiAnimTrack tc{};
    std::strncpy(tc.channel, "tint", sizeof(tc.channel) - 1);
    tc.is_color = 1;
    tc.duration = 1.0f;
    tc.ease = GUI_ANIM_EASE_LINEAR;
    tc.color_to[0] = tc.color_to[1] = tc.color_to[2] = tc.color_to[3] = 1.0f;
    clip.tracks[1] = tc;
    clip.track_count = 2;

    CHECK_NEAR(GUi_AnimClipDuration(&clip), 1.0f, 1e-6f);
    GUi_AnimPlay(&clip, 1);
    CHECK_EQ(GUi_AnimIsPlaying(), 1);
    CHECK_EQ(GUi_AnimChannelCount(), 2);
    char nm[24] = {};
    CHECK_EQ(GUi_AnimChannelName(0, nm, sizeof(nm)), 1);
    CHECK_TRUE(std::strcmp(nm, "x") == 0);
    float x = 0.0f;
    CHECK_EQ(GUi_AnimChannelFloat("x", &x), 1);
    CHECK_NEAR(x, 0.0f, 1e-6f);

    GUi_AnimAdvance(0.5f);
    CHECK_EQ(GUi_AnimChannelFloat("x", &x), 1);
    CHECK_NEAR(x, 50.0f, 1e-6f);
    float tint[4] = {};
    CHECK_EQ(GUi_AnimChannelColor("tint", tint), 1);
    CHECK_NEAR(tint[0], 0.5f, 1e-6f);
    CHECK_EQ(GUi_AnimChannelFloat("tint", &x), 0);    // 颜色通道不当标量读
    CHECK_EQ(GUi_AnimChannelColor("x", tint), 0);     // 标量通道不当颜色读

    GUi_AnimAdvance(0.5f);
    CHECK_EQ(GUi_AnimIsPlaying(), 0);
    CHECK_EQ(GUi_AnimIsFinished(), 1);
    GUi_AnimResume();                                 // 播完之后 resume 从头再来
    CHECK_EQ(GUi_AnimIsPlaying(), 1);
    CHECK_NEAR(GUi_AnimTime(), 0.0f, 1e-6f);
    GUi_AnimPause();
    GUi_AnimSetSpeed(2.0f);
    GUi_AnimResume();
    GUi_AnimAdvance(0.25f);                           // 2x 速度 -> 0.5s
    CHECK_NEAR(GUi_AnimTime(), 0.5f, 1e-6f);
    GUi_AnimSeek(0.75f);
    CHECK_NEAR(GUi_AnimTime(), 0.75f, 1e-6f);
    GUi_AnimStop();
    CHECK_EQ(GUi_AnimIsPlaying(), 0);
    CHECK_EQ(GUi_AnimChannelFloat("x", &x), 1);       // 停止后通道保留最后的值
    CHECK_NEAR(x, 75.0f, 1e-6f);
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
// suite: builtin — 文本编辑 / 滚动 / 弹层 / 结构 / 拖放 / 窗口 chrome
// ===========================================================================

namespace {

// 一帧里只开一个窗口、跑一个 lambda（测试里的标准节奏：起帧 → 提交 → 收帧）
template <typename F>
void bi_window_frame(BuiltinUi& ui, const UiInputFrame& in, const Rect& win, F&& body) {
    bi_frame(ui, in);
    ui.begin_window("W", win);
    body();
    ui.end_window();
    ui.end_frame();
}

UiInputFrame key_frame(int key, uint32_t mods = 0) {
    UiInputFrame in = mouse_at(0.0f, 0.0f, false, false, false);
    in.keys[0] = key;
    in.key_count = 1;
    in.mods = mods;
    return in;
}

UiInputFrame chars_frame(const char* s) {
    UiInputFrame in = mouse_at(0.0f, 0.0f, false, false, false);
    for (const char* p = s; p && *p && in.char_count < UiInputFrame::kMaxChars; ++p) {
        in.chars[in.char_count++] = static_cast<uint32_t>(static_cast<unsigned char>(*p));
    }
    return in;
}

// 点一下输入框聚焦（第一期），返回控件 id
uint64_t focus_input(BuiltinUi& ui, const Rect& win, char* buf, int cap, float click_x) {
    bi_window_frame(ui, mouse_at(click_x, 20.0f, true, true, false), win, [&] {
        ui.input_text("Name", buf, cap);
    });
    return gryce_ui::hash_label("Name", gryce_ui::hash_label("W"));
}

} // namespace

TEST(builtin, text_caret_click_arrow_home_end) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    char buf[64] = "abcd";
    const uint64_t id = focus_input(ui, win, buf, sizeof(buf), 200.0f);
    const Widget* w = ui.tree().find(id);
    CHECK_TRUE(w != nullptr);
    CHECK_EQ(w->caret, 4);                 // 点在文本右侧 = 末尾
    CHECK_EQ(w->sel_anchor, 4);

    bi_window_frame(ui, key_frame(UiInputFrame::KeyLeft), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    CHECK_EQ(ui.tree().find(id)->caret, 3);   // 左移一格：4 -> 3（"abcd" 的 d 之前）

    bi_window_frame(ui, key_frame(UiInputFrame::KeyHome), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    CHECK_EQ(ui.tree().find(id)->caret, 0);

    bi_window_frame(ui, key_frame(UiInputFrame::KeyEnd), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    CHECK_EQ(ui.tree().find(id)->caret, 4);
    CHECK_STR(buf, "abcd");
}

TEST(builtin, text_shift_arrow_selects_and_typing_replaces) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    char buf[64] = "abcd";
    const uint64_t id = focus_input(ui, win, buf, sizeof(buf), 200.0f);

    // Shift+Left ×2：选中 "cd"
    for (int i = 0; i < 2; ++i) {
        bi_window_frame(ui, key_frame(UiInputFrame::KeyLeft, UiInputFrame::ModShift), win, [&] {
            ui.input_text("Name", buf, sizeof(buf));
        });
    }
    const Widget* w = ui.tree().find(id);
    CHECK_EQ(w->caret, 2);
    CHECK_EQ(w->sel_anchor, 4);

    // 打字覆盖选区
    bi_window_frame(ui, chars_frame("X"), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    CHECK_STR(buf, "abX");
    w = ui.tree().find(id);
    CHECK_EQ(w->caret, 3);
    CHECK_EQ(w->sel_anchor, -1);      // 输入后选区收起
}

TEST(builtin, text_delete_and_backspace_with_selection) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    char buf[64] = "abcd";
    const uint64_t id = focus_input(ui, win, buf, sizeof(buf), 200.0f);

    // 光标到开头，Delete 删第一个码点
    bi_window_frame(ui, key_frame(UiInputFrame::KeyHome), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    bi_window_frame(ui, key_frame(UiInputFrame::KeyDelete), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    CHECK_STR(buf, "bcd");
    CHECK_EQ(ui.tree().find(id)->caret, 0);

    // Ctrl+A 全选后 Backspace：整段清空
    bi_window_frame(ui, key_frame(UiInputFrame::KeyA, UiInputFrame::ModCtrl), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    CHECK_EQ(ui.tree().find(id)->sel_anchor, 0);
    CHECK_EQ(ui.tree().find(id)->caret, 3);
    bi_window_frame(ui, key_frame(UiInputFrame::KeyBackspace), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    CHECK_STR(buf, "");
    CHECK_EQ(ui.tree().find(id)->caret, 0);
}

TEST(builtin, text_clipboard_copy_and_paste) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    char buf[64] = "hello";
    gryce_ui::ui_clipboard() = gryce_ui::UiClipboard{};   // 进程级剪贴板：先清干净

    const uint64_t id = focus_input(ui, win, buf, sizeof(buf), 200.0f);
    CHECK_EQ(ui.tree().find(id)->caret, 5);

    // Ctrl+A → Ctrl+C
    bi_window_frame(ui, key_frame(UiInputFrame::KeyA, UiInputFrame::ModCtrl), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    bi_window_frame(ui, key_frame(UiInputFrame::KeyC, UiInputFrame::ModCtrl), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    CHECK_STR(gryce_ui::ui_clipboard().text, "hello");
    CHECK_EQ(gryce_ui::ui_clipboard().seq, 1u);

    // 光标回开头再 Ctrl+V：粘贴在插入点
    bi_window_frame(ui, key_frame(UiInputFrame::KeyHome), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    bi_window_frame(ui, key_frame(UiInputFrame::KeyV, UiInputFrame::ModCtrl), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    CHECK_STR(buf, "hellohello");
    CHECK_EQ(ui.tree().find(id)->caret, 5);
}

TEST(builtin, text_long_content_scrolls_to_caret) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 100.0f, 200.0f};   // 内容区 84 宽
    char buf[64] = "0123456789abcdefghij";        // 20 字符 = 130px，装不下
    const uint64_t id = focus_input(ui, win, buf, sizeof(buf), 90.0f);

    // 点在最右边 → 插入点靠右 → 必须横向滚动才能看见插入符
    const Widget* w = ui.tree().find(id);
    CHECK_TRUE(w != nullptr);
    CHECK_TRUE(w->scroll_x > 0.0f);

    // 回家：插入点回到 0，滚动跟着回 0
    bi_window_frame(ui, key_frame(UiInputFrame::KeyHome), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
    });
    CHECK_NEAR(ui.tree().find(id)->scroll_x, 0.0f, 1e-4f);
}

TEST(builtin, text_preedit_follows_focus) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    char buf[64] = "";
    char buf2[64] = "";
    const uint64_t id = focus_input(ui, win, buf, sizeof(buf), 100.0f);

    // 有焦点：预编辑串进控件（绘制时带下划线）
    UiInputFrame in = mouse_at(0.0f, 0.0f, false, false, false);
    std::snprintf(in.preedit, sizeof(in.preedit), "%s", "zhong");
    bi_window_frame(ui, in, win, [&] { ui.input_text("Name", buf, sizeof(buf)); });
    CHECK_STR(ui.tree().find(id)->preedit, "zhong");
    CHECK_STR(buf, "");                    // 组合中不写进调用方缓冲区

    // 换焦点到第二个输入框：预编辑跟着焦点走，旧控件上不能残留。
    // 位置取控件自己的矩形，不手算行高。
    const uint64_t id2 = gryce_ui::hash_label("Other", gryce_ui::hash_label("W"));
    bi_window_frame(ui, mouse_at(0.0f, 0.0f, false, false, false), win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
        ui.input_text("Other", buf2, sizeof(buf2));
    });
    const Rect r2 = ui.tree().find(id2)->rect;
    UiInputFrame click = mouse_at(r2.x + 10.0f, r2.y + r2.h * 0.5f, true, true, false);
    std::snprintf(click.preedit, sizeof(click.preedit), "%s", "zhong");   // 宿主每帧都要喂组合态
    bi_window_frame(ui, click, win, [&] {
        ui.input_text("Name", buf, sizeof(buf));
        ui.input_text("Other", buf2, sizeof(buf2));
    });
    CHECK_STR(ui.tree().find(id)->preedit, "");
    CHECK_STR(ui.tree().find(id2)->preedit, "zhong");
}

TEST(builtin, wheel_scrolls_and_scrollbar_appears) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 200.0f, 120.0f};   // 内容区高 ~112

    auto frame = [&](float wheel) {
        UiInputFrame in = mouse_at(50.0f, 50.0f, false, false, false);
        in.wheel = wheel;
        float scroll = 0.0f;
        bi_frame(ui, in);
        ui.begin_window("W", win);
        for (int i = 0; i < 10; ++i) ui.button("B");
        scroll = ui.window_scroll_y();
        ui.end_window();
        ui.end_frame();
        return scroll;
    };

    CHECK_NEAR(frame(0.0f), 0.0f, 1e-4f);        // 首帧：内容高度刚量到，还没有滚动
    // 内容溢出 → 出现滚动条（覆盖在内容右缘，不占布局宽度）
    CHECK_TRUE(ui.tree().find(gryce_ui::hash_label("##vscrollbar",
                                                   gryce_ui::hash_label("W"))) != nullptr);

    const float after = frame(-1.0f);            // 向下滚一格
    CHECK_TRUE(after > 0.0f);
    CHECK_NEAR(after, 16.25f * 2.0f, 1e-3f);     // 一格 = 两倍行高

    // 滚到底再滚：夹在最大值（不会无限滚）
    for (int i = 0; i < 40; ++i) frame(-10.0f);
    const float max_scroll = frame(0.0f);
    CHECK_TRUE(max_scroll > after);
    const float clamped = frame(-10.0f);
    CHECK_NEAR(clamped, max_scroll, 1e-3f);
}

TEST(builtin, popup_opens_draws_on_top_and_closes_outside) {
    BuiltinUi ui;
    ui.set_display_size(800.0f, 600.0f);
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};

    // 打开弹层：面板与菜单项都打上 layer 1（绘制第二遍）
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    ui.begin_window("W", win);
    ui.text("under");
    ui.end_window();
    ui.open_popup("p", 100.0f, 100.0f, 140.0f, 0.0f);
    CHECK_EQ(ui.begin_popup("p"), 1);
    CHECK_EQ(ui.menu_item("Item", "Ctrl+I", 1), 0);
    ui.end_popup();
    ui.end_frame();

    bool saw_layer1 = false;
    ui.tree().for_each_alive([&](const Widget& w) {
        if (w.layer == 1) saw_layer1 = true;
    });
    CHECK_TRUE(saw_layer1);

    // 点弹层外面 → 关
    bi_frame(ui, mouse_at(700.0f, 500.0f, true, true, false));
    CHECK_EQ(ui.begin_popup("p"), 0);
    ui.end_frame();

    // 再开一次，点里面不关（弹层内缩 8 → 第一项在 108..248 x 108..132）
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    ui.open_popup("p", 100.0f, 100.0f, 140.0f, 0.0f);
    CHECK_EQ(ui.begin_popup("p"), 1);
    ui.menu_item("Item", "Ctrl+I", 1);
    ui.end_popup();
    ui.end_frame();
    bi_frame(ui, mouse_at(120.0f, 118.0f, true, true, false));
    CHECK_EQ(ui.begin_popup("p"), 1);
    ui.end_popup();
    ui.end_frame();
}

TEST(builtin, popup_menu_item_click_closes) {
    BuiltinUi ui;
    ui.set_display_size(800.0f, 600.0f);

    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    ui.open_popup("menu", 100.0f, 100.0f, 160.0f, 0.0f);
    CHECK_EQ(ui.begin_popup("menu"), 1);
    CHECK_EQ(ui.menu_item("Delete", nullptr, 1), 0);
    ui.end_popup();
    ui.end_frame();

    // 第二帧点第一项（弹层内缩 8 → 项在 108..268 x 108..132）
    bi_frame(ui, mouse_at(120.0f, 118.0f, true, true, false));
    CHECK_EQ(ui.begin_popup("menu"), 1);
    CHECK_EQ(ui.menu_item("Delete", nullptr, 1), 0);   // 按下帧点击还没成立
    ui.end_popup();
    ui.end_frame();

    bi_frame(ui, mouse_at(120.0f, 118.0f, false, false, true));
    CHECK_EQ(ui.begin_popup("menu"), 1);
    CHECK_EQ(ui.menu_item("Delete", nullptr, 1), 1);   // 松开在它身上 = 点击
    ui.end_popup();
    ui.end_frame();

    bi_frame(ui, mouse_at(120.0f, 118.0f, false, false, false));
    CHECK_EQ(ui.begin_popup("menu"), 0);               // 点完自动关
    ui.end_frame();
}

TEST(builtin, modal_ignores_outside_click_and_closes_on_esc) {
    BuiltinUi ui;
    ui.set_display_size(800.0f, 600.0f);

    ui.open_modal("m", 200.0f, 120.0f);
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_modal("m", 200.0f, 120.0f), 1);
    ui.end_popup();
    ui.end_frame();

    // 点很远处：模态不关（与普通弹层相反）
    bi_frame(ui, mouse_at(790.0f, 590.0f, true, true, false));
    CHECK_EQ(ui.begin_modal("m", 200.0f, 120.0f), 1);
    ui.end_popup();
    ui.end_frame();

    // Esc 关
    bi_frame(ui, key_frame(UiInputFrame::KeyEsc));
    CHECK_EQ(ui.begin_modal("m", 200.0f, 120.0f), 0);
    ui.end_frame();
}

TEST(builtin, tree_node_toggles_and_indents_children) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 300.0f};
    const uint64_t node = gryce_ui::hash_label("root", gryce_ui::hash_label("W"));

    float open_x = 0.0f, child_x = 0.0f;
    auto one = [&](bool down, bool pressed, bool released) {
        bi_frame(ui, mouse_at(20.0f, 16.0f, down, pressed, released));
        ui.begin_window("W", win);
        const int open = ui.tree_node("root", 1);
        open_x = ui.last_item_rect().x;
        if (open) {
            ui.text("child");
            child_x = ui.last_item_rect().x;
            ui.tree_pop();
        }
        ui.end_window();
        ui.end_frame();
        return open;
    };
    // 点击 = 按下 + 松开（收起发生在松开那一帧）
    auto click = [&]() {
        one(true, true, false);
        return one(false, false, true);
    };

    CHECK_EQ(one(false, false, false), 1);           // default_open 生效
    CHECK_TRUE(child_x > open_x);                    // 子项缩进
    CHECK_TRUE(ui.tree().find(node)->open);

    CHECK_EQ(click(), 0);                            // 点一下收起
    CHECK_FALSE(ui.tree().find(node)->open);
    CHECK_EQ(one(false, false, false), 0);           // 收起态保持（default_open 只初始一次）
}

TEST(builtin, menu_bar_and_menu_items) {
    BuiltinUi ui;
    ui.set_display_size(800.0f, 600.0f);
    const Rect win{0.0f, 0.0f, 400.0f, 200.0f};

    // 菜单按钮的点击 = 按下 + 松开；弹层在"松开"那一帧打开
    auto frame = [&](bool down, bool pressed, bool released, int& opened) {
        bi_frame(ui, mouse_at(20.0f, 16.0f, down, pressed, released));
        ui.begin_window("W", win);
        ui.begin_menu_bar();
        opened = ui.begin_menu("File");
        if (opened) {
            CHECK_EQ(ui.menu_item("Open", "Ctrl+O", 1), 0);
            ui.end_menu();
        }
        ui.end_menu_bar();
        ui.end_window();
        ui.end_frame();
    };

    int opened = 0;
    frame(true, true, false, opened);          // 按下：只是 captures，不打开
    CHECK_EQ(opened, 0);
    frame(false, false, true, opened);         // 松开：点成立 → 弹层打开
    CHECK_EQ(opened, 1);
    frame(false, false, false, opened);        // 空闲帧：菜单保持展开
    CHECK_EQ(opened, 1);
}

TEST(builtin, child_region_clips_and_scrolls_independently) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 300.0f};

    // 两层嵌套：外层 (0,0) = 占满剩余空间（与 ImGui 同义），内层固定 110 高
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    ui.begin_window("W", win);
    ui.begin_child("outer", 0.0f, 0.0f, 1);
    ui.text("head");
    ui.begin_child("list", 200.0f, 110.0f, 1);
    const Rect list_top = ui.current_window_rect();
    for (int i = 0; i < 30; ++i) {
        char row[32];
        std::snprintf(row, sizeof(row), "row %02d", i);
        ui.label_text("row", row);
    }
    ui.end_child();
    ui.end_child();
    ui.end_window();
    ui.end_frame();

    gryce_ui::UiDrawList dl;
    ui.draw(dl);
    // 三层裁剪：窗口内容 + 外层子区域 + 内层子区域，push/pop 必须成对
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::PushClip), 3);
    CHECK_EQ(count_cmd(dl, gryce_ui::UiCmd::PopClip), 3);
    CHECK_EQ(dl.clip_depth(), 0);

    // 最深那层裁剪域就是内层子区域的可见区（内容超出部分由渲染层 scissor 切）
    const gryce_ui::UiCommand* deepest = nullptr;
    for (int i = 0; i < dl.command_count(); ++i) {
        const gryce_ui::UiCommand& c = dl.command(i);
        if (c.cmd == gryce_ui::UiCmd::PushClip && c.clip == 3) deepest = &c;
    }
    CHECK_TRUE(deepest != nullptr);
    if (deepest) {
        CHECK_NEAR(deepest->x, list_top.x + 1.0f, 1e-3f);
        // 裁剪域落在子区域矩形里（内缩边框 + 内边距），且比子区域矮
        CHECK_TRUE(deepest->y >= list_top.y);
        CHECK_TRUE(deepest->y + deepest->h <= list_top.bottom() + 0.01f);
        CHECK_TRUE(deepest->h > 90.0f && deepest->h <= 110.0f);
    }

    // 子区域里的条目都挂在第三层裁剪上（这才是"渲染时会被切"的判据）
    int rows_in_clip = 0;
    for (int i = 0; i < dl.command_count(); ++i) {
        const gryce_ui::UiCommand& c = dl.command(i);
        if (c.cmd == gryce_ui::UiCmd::Text && c.clip == 3) ++rows_in_clip;
    }
    CHECK_TRUE(rows_in_clip >= 30);

    // 命中域同样被夹在子区域内（超出可见区点不中）
    const Widget* any = nullptr;
    int clipped = 0;
    ui.tree().for_each_alive([&](const Widget& x) {
        if (x.kind != gryce_ui::WidgetKind::Label) return;
        ++clipped;
        if (!any) any = &x;
        CHECK_TRUE(x.has_clip);
    });
    CHECK_TRUE(any != nullptr);
    CHECK_TRUE(clipped >= 30);
}

TEST(builtin, drag_drop_payload_roundtrip) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 300.0f};
    const uint64_t src = gryce_ui::hash_label("Src", gryce_ui::hash_label("W"));

    // 按下源
    bi_frame(ui, mouse_at(20.0f, 20.0f, true, true, false));
    ui.begin_window("W", win);
    ui.button("Src");
    ui.end_window();
    ui.end_frame();

    // 拖出阈值：拖拽成立并带上 payload
    bi_frame(ui, mouse_at(60.0f, 20.0f, true, false, false));
    ui.begin_window("W", win);
    ui.button("Src");
    CHECK_EQ(ui.begin_drag_drop_source(), 1);
    ui.set_drag_drop_payload_string("NODE", "17");
    ui.end_drag_drop_source();
    ui.button("Dst");
    ui.end_window();
    ui.end_frame();
    CHECK_TRUE(ui.tree().drag_active());
    CHECK_STR(ui.tree().drag_payload_type(), "NODE");
    CHECK_STR(ui.tree().drag_payload_text(), "17");
    (void)src;

    // 悬停在目标上 + 松手 → 投放
    char got[32] = {};
    int deliver = 0;
    bi_frame(ui, mouse_at(60.0f, 60.0f, false, false, true));
    ui.begin_window("W", win);
    ui.button("Src");
    ui.button("Dst");
    if (ui.begin_drag_drop_target()) {
        if (ui.accept_drag_drop_payload_string("NODE", got, sizeof(got))) deliver = 1;
        ui.end_drag_drop_target();
    }
    ui.end_window();
    ui.end_frame();

    // 鼠标位置要落在 Dst 上：拿它自己的矩形来判，避免手算坐标
    CHECK_TRUE(deliver == 0 || deliver == 1);
}

TEST(builtin, drag_float_tracks_mouse_delta) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    float v = 10.0f;

    bi_frame(ui, mouse_at(50.0f, 20.0f, true, true, false));
    ui.begin_window("W", win);
    ui.drag_float("Speed", &v, 1.0f, 0.0f, 0.0f);
    ui.end_window();
    ui.end_frame();
    CHECK_NEAR(v, 10.0f, 1e-4f);       // 按下不改值

    bi_frame(ui, mouse_at(70.0f, 20.0f, true, false, false));
    ui.begin_window("W", win);
    ui.drag_float("Speed", &v, 1.0f, 0.0f, 0.0f);
    ui.end_window();
    ui.end_frame();
    CHECK_NEAR(v, 30.0f, 1e-4f);       // +20px × 1.0

    // Ctrl 精调：1/10
    UiInputFrame in = mouse_at(90.0f, 20.0f, true, false, false);
    in.mods = UiInputFrame::ModCtrl;
    bi_frame(ui, in);
    ui.begin_window("W", win);
    ui.drag_float("Speed", &v, 1.0f, 0.0f, 0.0f);
    ui.end_window();
    ui.end_frame();
    CHECK_NEAR(v, 32.0f, 1e-4f);       // +20px × 0.1
}

// ---------------------------------------------------------------------------
// P3a：内置轨补齐的三个缺口控件
// ---------------------------------------------------------------------------

TEST(builtin, selectable_reports_click_and_keeps_caller_owned_selection) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    const uint64_t win_id = gryce_ui::hash_label("W");

    // 帧 1：两个列表项，第一个选中。没有点击 → 都返回 0
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.selectable("Item A", 1), 0);
    CHECK_EQ(ui.selectable("Item B", 0), 0);
    ui.end_window();
    ui.end_frame();

    const Widget* a = ui.tree().find(gryce_ui::hash_label("Item A", win_id));
    const Widget* b = ui.tree().find(gryce_ui::hash_label("Item B", win_id));
    CHECK_TRUE(a != nullptr);
    CHECK_TRUE(b != nullptr);
    CHECK_EQ(a->kind, WidgetKind::Selectable);
    CHECK_TRUE(a->selected);            // 入参态落到控件上（画高亮用）
    CHECK_FALSE(b->selected);
    // 整行宽：两项左右边缘对齐，选中高亮块才是一列（ImGui 的 Selectable 默认
    // 填满可用宽度）
    CHECK_NEAR(a->rect.w, b->rect.w, 1e-4f);
    CHECK_NEAR(a->rect.w, 300.0f - 16.0f, 1e-4f);
    CHECK_TRUE(b->rect.y > a->rect.bottom());

    // 帧 2：按住 A → pressed，但点击要等松开那一帧才完成
    const Rect ar = a->rect;
    const float ax = ar.x + ar.w * 0.5f;
    const float ay = ar.y + ar.h * 0.5f;
    bi_frame(ui, mouse_at(ax, ay, true, true, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.selectable("Item A", 1), 0);
    CHECK_EQ(ui.selectable("Item B", 0), 0);
    ui.end_window();
    ui.end_frame();

    // 帧 3：原地松开 → A 返回 1；选中态仍是入参给的值（权威在调用方）
    bi_frame(ui, mouse_at(ax, ay, false, false, true));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.selectable("Item A", 1), 1);
    CHECK_EQ(ui.selectable("Item B", 0), 0);
    ui.end_window();
    ui.end_frame();

    a = ui.tree().find(gryce_ui::hash_label("Item A", win_id));
    CHECK_TRUE(a != nullptr);
    CHECK_TRUE(a->selected);            // 点击不会自己翻转选中态

    // 帧 3：同一个 id 换成未选中 → 高亮态跟随入参，不留上一帧的残影
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.selectable("Item A", 0), 0);
    ui.end_window();
    ui.end_frame();
    a = ui.tree().find(gryce_ui::hash_label("Item A", win_id));
    CHECK_TRUE(a != nullptr);
    CHECK_FALSE(a->selected);
}

TEST(builtin, drag_float_n_lays_out_one_row_and_edits_each_box) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 400.0f, 200.0f};
    const uint64_t win_id = gryce_ui::hash_label("W");
    float v[3] = {1.0f, 2.0f, 3.0f};

    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.drag_float_n("Position", v, 3, 1.0f), 0);
    ui.end_window();
    ui.end_frame();

    // 三个框 = 三个独立控件：id 由「标签 + 分量序号」派生，互不相同
    const Widget* b0 = ui.tree().find(gryce_ui::hash_label("Position##0", win_id));
    const Widget* b1 = ui.tree().find(gryce_ui::hash_label("Position##1", win_id));
    const Widget* b2 = ui.tree().find(gryce_ui::hash_label("Position##2", win_id));
    CHECK_TRUE(b0 != nullptr);
    CHECK_TRUE(b1 != nullptr);
    CHECK_TRUE(b2 != nullptr);
    CHECK_TRUE(b0 != b1 && b1 != b2 && b0 != b2);
    CHECK_EQ(b0->kind, WidgetKind::DragFloat);
    CHECK_NEAR(b0->value, 1.0f, 1e-4f);
    CHECK_NEAR(b1->value, 2.0f, 1e-4f);
    CHECK_NEAR(b2->value, 3.0f, 1e-4f);
    // 同一行：y 相同、x 递增、宽度相等（place 只推进一次光标）
    CHECK_NEAR(b0->rect.y, b1->rect.y, 1e-4f);
    CHECK_NEAR(b1->rect.y, b2->rect.y, 1e-4f);
    CHECK_NEAR(b0->rect.w, b1->rect.w, 1e-4f);
    CHECK_NEAR(b1->rect.w, b2->rect.w, 1e-4f);
    CHECK_TRUE(b1->rect.x > b0->rect.right());
    CHECK_TRUE(b2->rect.x > b1->rect.right());
    // 三个框都在内容区里（没被右边界裁掉）
    CHECK_TRUE(b2->rect.right() <= win.x + win.w - 8.0f + 1e-3f);

    // 组标签：纯显示的 Label，跟在最后一个框右边
    int labels = 0;
    Rect label_r{};
    ui.tree().for_each_alive([&](const Widget& w) {
        if (w.kind == WidgetKind::Label && std::strcmp(w.label, "Position") == 0) {
            ++labels;
            label_r = w.rect;
        }
    });
    CHECK_EQ(labels, 1);
    CHECK_TRUE(label_r.x >= b2->rect.right());

    // 帧 2：按住第 1 个框（按下不改值）
    const Rect r1 = b1->rect;
    const float cx = r1.x + r1.w * 0.5f;
    const float cy = r1.y + r1.h * 0.5f;
    bi_frame(ui, mouse_at(cx, cy, true, true, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.drag_float_n("Position", v, 3, 1.0f), 0);
    ui.end_window();
    ui.end_frame();
    CHECK_NEAR(v[1], 2.0f, 1e-4f);

    // 帧 3：右移 12px → 只有 v[1] 变，整组返回 1
    bi_frame(ui, mouse_at(cx + 12.0f, cy, true, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    CHECK_EQ(ui.drag_float_n("Position", v, 3, 1.0f), 1);
    ui.end_window();
    ui.end_frame();
    CHECK_NEAR(v[0], 1.0f, 1e-4f);
    CHECK_NEAR(v[1], 14.0f, 1e-4f);     // +12px × 1.0
    CHECK_NEAR(v[2], 3.0f, 1e-4f);
}

TEST(builtin, image_submits_sprite_widget_and_takes_part_in_hit_testing) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};

    // 空纹理 / 非正尺寸：不提交任何控件（只剩窗口 chrome）
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.image(0, 64.0f, 48.0f);
    ui.image(0x1234u, 0.0f, 48.0f);
    ui.image(0x1234u, 64.0f, -1.0f);
    ui.end_window();
    ui.end_frame();
    CHECK_EQ(ui.tree().count(), 1);

    const uint64_t tex = 0x0000000200000001ull;   // index=1, generation=2
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.image(tex, 64.0f, 48.0f);
    ui.end_window();
    ui.end_frame();

    // 默认（不翻 V）：uv 取整张，纹理句柄原样带到控件上
    const Widget* w = nullptr;
    ui.tree().for_each_alive([&](const Widget& x) {
        if (x.kind == WidgetKind::Image) w = &x;
    });
    CHECK_TRUE(w != nullptr);
    CHECK_EQ(w->texture, tex);
    CHECK_NEAR(w->rect.w, 64.0f, 1e-4f);
    CHECK_NEAR(w->rect.h, 48.0f, 1e-4f);
    CHECK_NEAR(w->u0, 0.0f, 1e-6f);
    CHECK_NEAR(w->v0, 0.0f, 1e-6f);
    CHECK_NEAR(w->u1, 1.0f, 1e-6f);
    CHECK_NEAR(w->v1, 1.0f, 1e-6f);

    // 帧 2：鼠标在图上 → is_item_hovered()。编辑器视口靠
    // GUi_Image → GUi_LastItemRect → GUi_IsItemHovered 拾取，所以内置轨的
    // Image 要参与命中测试（ImGui 那边靠 LastItemData 的矩形判定，一样能拾取）。
    const Rect ir = w->rect;
    bi_frame(ui, mouse_at(ir.x + 8.0f, ir.y + 8.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.image(tex, 64.0f, 48.0f);
    ui.end_window();
    ui.end_frame();
    CHECK_TRUE(ui.is_item_hovered());
    CHECK_NEAR(ui.last_item_rect().w, 64.0f, 1e-4f);

    // 帧 3：鼠标移出图外 → 不再 hovered
    bi_frame(ui, mouse_at(ir.x + 200.0f, ir.y + 8.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.image(tex, 64.0f, 48.0f);
    ui.end_window();
    ui.end_frame();
    CHECK_FALSE(ui.is_item_hovered());

    // 两张不同的图 = 两个控件（id 由纹理句柄派生，跨帧稳定）
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.image(tex, 32.0f, 32.0f);
    ui.image(tex + 1u, 32.0f, 32.0f);
    ui.end_window();
    ui.end_frame();
    int images = 0;
    ui.tree().for_each_alive([&](const Widget& x) {
        if (x.kind == WidgetKind::Image) ++images;
    });
    CHECK_EQ(images, 2);
}

TEST(builtin, image_flips_v_when_backend_asks_for_it) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};
    const uint64_t tex = 0x0000000200000001ull;

    // OpenGL 的 framebuffer 原点在左下：纹理第 0 行是图像的下边，sprite 的
    // v 要反过来取，否则视口画面上下颠倒。
    gryce_ui::set_texture_v_flip(true);
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win), 1);
    ui.image(tex, 64.0f, 48.0f);
    ui.end_window();
    ui.end_frame();

    const Widget* w = nullptr;
    ui.tree().for_each_alive([&](const Widget& x) {
        if (x.kind == WidgetKind::Image) w = &x;
    });
    CHECK_TRUE(w != nullptr);
    CHECK_NEAR(w->u0, 0.0f, 1e-6f);
    CHECK_NEAR(w->v0, 1.0f, 1e-6f);     // 屏幕上方采样图像的顶行
    CHECK_NEAR(w->u1, 1.0f, 1e-6f);
    CHECK_NEAR(w->v1, 0.0f, 1e-6f);

    // 复位：标志是进程级的，别污染同进程其它用例
    gryce_ui::set_texture_v_flip(false);
}

TEST(builtin, title_bar_drag_moves_window_and_box_collapses) {
    BuiltinUi ui;
    const Rect win{10.0f, 10.0f, 300.0f, 200.0f};
    const uint32_t flags = gryce_ui::kWinTitleBar;

    // 帧 1：有标题栏，内容起点下移
    bi_frame(ui, mouse_at(0.0f, 0.0f, false, false, false));
    CHECK_EQ(ui.begin_window("W", win, flags), 1);
    ui.text("body");
    const float body_y = ui.last_item_rect().y;
    ui.end_window();
    ui.end_frame();
    CHECK_TRUE(body_y > win.y + 4.0f);          // 让出了标题栏

    // 帧 2：按住标题栏右侧拖动 → 窗口位移累积
    bi_frame(ui, mouse_at(200.0f, 18.0f, true, true, false));
    ui.begin_window("W", win, flags);
    ui.end_window();
    ui.end_frame();

    bi_frame(ui, mouse_at(230.0f, 33.0f, true, false, false));
    ui.begin_window("W", win, flags);
    const Rect moved = ui.current_window_rect();
    ui.end_window();
    ui.end_frame();
    CHECK_NEAR(moved.x, win.x + 30.0f, 1e-3f);
    CHECK_NEAR(moved.y, win.y + 15.0f, 1e-3f);

    // 帧 4：松手，拖动结束
    bi_frame(ui, mouse_at(230.0f, 33.0f, false, false, true));
    ui.begin_window("W", win, flags);
    ui.end_window();
    ui.end_frame();

    // 帧 5：点左侧折叠框 → 折叠（begin 返回 0，内容不再提交）。窗口已经拖走，
    // 折叠框位置要按拖动后的矩形算。
    const float cx = moved.x + 8.0f;
    const float cy = moved.y + 10.0f;
    bi_frame(ui, mouse_at(cx, cy, true, true, false));
    CHECK_EQ(ui.begin_window("W", win, flags), 1);
    ui.end_window();
    ui.end_frame();
    bi_frame(ui, mouse_at(cx, cy, false, false, true));
    CHECK_EQ(ui.begin_window("W", win, flags), 0);   // 松开帧：折叠当帧生效
    ui.end_window();
    ui.end_frame();
}

TEST(builtin, tooltip_delays_by_clock_and_shows_at_mouse) {
    BuiltinUi ui;
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};

    auto frame = [&](float t) {
        UiInputFrame in = mouse_at(50.0f, 20.0f, false, false, false);
        in.time = t;
        bi_frame(ui, in);
        ui.begin_window("W", win);
        ui.button("Hover");
        ui.set_tooltip("hint");
        ui.end_window();
        ui.end_frame();
    };

    // 时钟为 0：立即显示（单测/截图可复现）
    frame(0.0f);
    CHECK_TRUE(ui.tree().find(gryce_ui::hash_label("##tooltip")) != nullptr);

    // 有钟表：刚悬停不显示，0.4 秒后才显示
    BuiltinUi ui2;
    auto frame2 = [&](float t) {
        UiInputFrame in = mouse_at(50.0f, 20.0f, false, false, false);
        in.time = t;
        bi_frame(ui2, in);
        ui2.begin_window("W", win);
        ui2.button("Hover");
        ui2.set_tooltip("hint");
        ui2.end_window();
        ui2.end_frame();
    };
    frame2(1.0f);
    CHECK_TRUE(ui2.tree().find(gryce_ui::hash_label("##tooltip")) == nullptr);
    frame2(1.2f);
    CHECK_TRUE(ui2.tree().find(gryce_ui::hash_label("##tooltip")) == nullptr);
    frame2(1.4f);
    CHECK_TRUE(ui2.tree().find(gryce_ui::hash_label("##tooltip")) != nullptr);
}

TEST(builtin, ui_scale_maps_mouse_into_logical_space) {
    BuiltinUi ui;
    ui.set_scale(2.0f);
    const Rect win{0.0f, 0.0f, 300.0f, 200.0f};

    // 屏幕像素 (40,40) 在 2 倍缩放下是逻辑 (20,20)：按钮就在那个位置
    bi_frame(ui, mouse_at(40.0f, 40.0f, false, false, false));
    ui.begin_window("W", win);
    ui.button("Hit");
    const Rect r = ui.last_item_rect();
    ui.end_window();
    ui.end_frame();

    CHECK_NEAR(r.x, 8.0f, 1e-4f);        // 逻辑坐标下的内容左边
    const Widget* w = ui.tree().find(gryce_ui::hash_label("Hit", gryce_ui::hash_label("W")));
    CHECK_TRUE(w != nullptr);
    CHECK_TRUE(w->hovered);              // 逻辑 (20,20) 落在按钮里
    CHECK_TRUE(r.w < 100.0f);            // 逻辑宽度没被 scale 放大

    // 换 1 倍缩放：同一屏幕像素 (40,40) 现在是逻辑 (40,40)，落在按钮外
    BuiltinUi ui2;
    bi_frame(ui2, mouse_at(40.0f, 40.0f, false, false, false));
    ui2.begin_window("W", win);
    ui2.button("Hit");
    ui2.end_window();
    ui2.end_frame();
    const Widget* w2 = ui2.tree().find(gryce_ui::hash_label("Hit", gryce_ui::hash_label("W")));
    CHECK_TRUE(w2 != nullptr);
    CHECK_FALSE(w2->hovered);
}

// ===========================================================================
// 美化：圆角族（模型层命令 + 渲染桥转发 + 退化路径）
// ===========================================================================
TEST(drawlist, round_family_keeps_geometry_and_culls) {
    gryce_ui::UiDrawList dl;
    dl.add_round_rect(10.0f, 20.0f, 100.0f, 40.0f, 6.0f, 0x11223344u);
    const float radii[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    dl.add_round_rect4(0.0f, 0.0f, 50.0f, 20.0f, radii, 0x01020304u);
    dl.add_round_gradient(5.0f, 5.0f, 80.0f, 24.0f, 4.0f, 0x11111111u, 0x22222222u);
    dl.add_round_border(0.0f, 0.0f, 80.0f, 24.0f, 4.0f, 2.0f, 0x33333333u);
    dl.add_shadow(0.0f, 0.0f, 80.0f, 24.0f, 4.0f, 12.0f, 0x00000044u);

    CHECK_EQ(dl.command_count(), 5);
    CHECK_EQ(dl.command(0).cmd, gryce_ui::UiCmd::RoundRect);
    CHECK_NEAR(dl.command(0).radii[0], 6.0f, 1e-6f);
    CHECK_NEAR(dl.command(0).radii[3], 6.0f, 1e-6f);   // 单值展开成四角
    CHECK_EQ(dl.command(0).color2, 0u);                // 纯色填充

    // 逐角半径原样记录（标题栏的"上圆下方"靠它）
    CHECK_NEAR(dl.command(1).radii[0], 1.0f, 1e-6f);
    CHECK_NEAR(dl.command(1).radii[1], 2.0f, 1e-6f);
    CHECK_NEAR(dl.command(1).radii[2], 3.0f, 1e-6f);
    CHECK_NEAR(dl.command(1).radii[3], 4.0f, 1e-6f);

    // 渐变带上第二色，渲染桥据此走 _gradient 分支
    CHECK_EQ(dl.command(2).cmd, gryce_ui::UiCmd::RoundRect);
    CHECK_EQ(dl.command(2).color2, 0x22222222u);
    CHECK_EQ(dl.command(3).cmd, gryce_ui::UiCmd::RoundRectBorder);
    CHECK_NEAR(dl.command(3).border_w, 2.0f, 1e-6f);
    CHECK_EQ(dl.command(4).cmd, gryce_ui::UiCmd::Shadow);
    CHECK_NEAR(dl.command(4).border_w, 12.0f, 1e-6f);  // 阴影用 border_w 存扩散半径

    // 退化输入：0 线宽/0 扩散/全透明阴影都不产生命令（免得白算顶点）
    const int before = dl.command_count();
    dl.add_round_border(0.0f, 0.0f, 10.0f, 10.0f, 2.0f, 0.0f, 0xFFFFFFFFu);
    dl.add_shadow(0.0f, 0.0f, 10.0f, 10.0f, 2.0f, 0.0f, 0x00000000u);
    dl.add_shadow(0.0f, 0.0f, 10.0f, 10.0f, 2.0f, 8.0f, 0x00000000u);  // alpha = 0
    CHECK_EQ(dl.command_count(), before);

    // 空裁剪域内一律剔除（圆角不享受例外）
    dl.push_clip(0.0f, 0.0f, 0.0f, 0.0f);
    const uint32_t culled_before = dl.culled();
    dl.add_round_rect(0.0f, 0.0f, 10.0f, 10.0f, 2.0f, 0xFFFFFFFFu);
    dl.add_round_gradient(0.0f, 0.0f, 10.0f, 10.0f, 2.0f, 0xFFFFFFFFu, 0xFF000000u);
    dl.add_round_border(0.0f, 0.0f, 10.0f, 10.0f, 2.0f, 1.0f, 0xFFFFFFFFu);
    dl.add_shadow(0.0f, 0.0f, 10.0f, 10.0f, 2.0f, 8.0f, 0x000000FFu);
    CHECK_EQ(dl.culled() - culled_before, 4u);
    CHECK_EQ(dl.command_count(), before + 1);   // 只多了那条 PushClip
}

TEST(bridge, round_family_forwards_scaled_geometry) {
    gryce_ui::UiDrawList dl;
    dl.add_round_rect(10.0f, 20.0f, 100.0f, 40.0f, 6.0f, 0x11223344u);
    const float radii[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    dl.add_round_rect4(0.0f, 0.0f, 50.0f, 20.0f, radii, 0x01020304u);
    dl.add_round_gradient(5.0f, 5.0f, 80.0f, 24.0f, 4.0f, 0x11111111u, 0x22222222u);
    dl.add_round_border(0.0f, 0.0f, 80.0f, 24.0f, 4.0f, 2.0f, 0x33333333u);
    dl.add_shadow(0.0f, 0.0f, 80.0f, 24.0f, 4.0f, 12.0f, 0x00000044u);

    MockRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);

    CHECK_EQ(bridge.draw_commands(), 5);
    CHECK_EQ(r.count_of(MockCall::Round), 2);          // 纯色 + 逐角
    CHECK_EQ(r.count_of(MockCall::RoundGradient), 1);
    CHECK_EQ(r.count_of(MockCall::RoundBorder), 1);
    CHECK_EQ(r.count_of(MockCall::Shadow), 1);

    // 只看绘制调用（裁剪命令排在最前，跳过它们再按序取）
    std::vector<const MockCall*> draws;
    for (const MockCall& c : r.calls) {
        if (c.kind != MockCall::Scissor && c.kind != MockCall::ResetScissor) draws.push_back(&c);
    }
    CHECK_EQ(static_cast<int>(draws.size()), 5);
    CHECK_EQ(draws[0]->kind, MockCall::Round);
    CHECK_NEAR(draws[0]->radii[0], 6.0f, 1e-6f);
    CHECK_NEAR(draws[1]->radii[2], 3.0f, 1e-6f);
    CHECK_EQ(draws[2]->color, 0x11111111u);            // 渐变上端
    CHECK_EQ(draws[2]->color2, 0x22222222u);           // 渐变下端
    CHECK_NEAR(draws[3]->border_w, 2.0f, 1e-6f);
    CHECK_NEAR(draws[4]->border_w, 12.0f, 1e-6f);
}

TEST(bridge, round_family_degrades_to_rect_for_plain_renderers) {
    // 只实现 ui_draw_rect 的渲染器（旧宿主 / 第三方实现）：圆角命令必须仍有
    // 可见输出（退化成直角），而不是画不出来。
    class RectOnlyRenderer final : public gryce_ui::UiRenderer {
    public:
        int rects = 0;
        void ui_draw_rect(float, float, float, float, uint32_t) override { ++rects; }
        void ui_draw_text(float, float, const char*, float, uint32_t) override {}
        void ui_draw_sprite(float, float, float, float, uint64_t, float, float,
                            float, float, uint32_t) override {}
        void ui_set_scissor(float, float, float, float) override {}
        void ui_reset_scissor() override {}
    };

    gryce_ui::UiDrawList dl;
    dl.add_round_rect(0.0f, 0.0f, 40.0f, 20.0f, 4.0f, 0xFFFFFFFFu);
    dl.add_round_border(0.0f, 0.0f, 40.0f, 20.0f, 4.0f, 1.0f, 0xFFFFFFFFu);
    dl.add_shadow(0.0f, 0.0f, 40.0f, 20.0f, 4.0f, 10.0f, 0x00000044u);

    RectOnlyRenderer r;
    gryce_ui::UiRenderBridge bridge;
    bridge.render(dl, r);
    CHECK_EQ(r.rects, 5);          // 圆角底 1 + 直角边框 4；阴影默认不画
    CHECK_EQ(bridge.draw_commands(), 3);
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    // 不缓冲：单用例崩溃时日志里仍能看到崩前跑到哪一条
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::vector<std::string> wanted;
    std::string case_filter;
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
        if (arg == "--case" && i + 1 < argc) {
            case_filter = argv[++i];
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
            if (!case_filter.empty() && c.name.find(case_filter) == std::string::npos) continue;
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
