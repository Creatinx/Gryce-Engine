// ===========================================================================
// engine/api/ui_op_queue.h — UI 操作队列（无 ImGui 依赖的纯逻辑）
//
// 解决的问题：UI 的刷新率不需要跟渲染帧率一样高，因此"UI 想改引擎状态"
// 这类操作不该在回调里当场执行（回调持着渲染器锁，也不能调 GCore_*/GEdit_*），
// 而要先进队列，由宿主在锁外按策略受控执行。
//
// 三条设计约束，各自都对应用户可见的症状：
//   1) 回调内只入队、不执行 —— 回调运行在 g_renderer.mutex 已持有的路径上，
//      当场调引擎 API 会二次加锁死锁；入队只是写一段纯数据，永远安全。
//   2) 同键合并 —— 拖拽滑块每帧都产生一条"设变换"，不合并的话一次拖拽
//      会往队列里灌几十条，撤销栈被截成几十步（合并判据见 ui-api.md）。
//   3) 受控排出 —— 每次排出有预算上限、排出之间有最小帧间隔；UI 操作
//      批量落地而不是每帧一次，正好匹配"UI 刷新率不用那么高"。
//
// 队列本身不认识任何操作语义：type / key / arg / vec 全部由调用方定义，
// 引擎只保证顺序、合并、预算与统计。语义（选中谁、改什么）属于消费方。
//
// 线程模型：push 在 UI 线程（回调内）、drain 在主线程（锁外）——内部
// 自备互斥量，且它是叶子锁（持有它时绝不调用任何外部接口），不会与
// 渲染器锁 / 核心锁形成环。
// ===========================================================================

#ifndef GRYCE_UI_OP_QUEUE_H
#define GRYCE_UI_OP_QUEUE_H

#include <cstdint>
#include <mutex>

namespace gryce_ui {

// 一条待执行的 UI 操作。布局与 C ABI 的 GUiOp 完全一致（ui_api.h），
// 由 ui_api.cpp 负责两种表示之间的原样搬运。
struct Op {
    uint32_t type = 0;      // 调用方自定义的操作码
    uint32_t key  = 0;      // 合并键：同 (type,key) 只保留最新；0 = 离散操作，不合并
    int64_t  arg  = 0;      // 整数参数（实体句柄 / 枚举 / 布尔）
    float    vec[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // 浮点参数（变换 / 颜色 / 范围）
    uint64_t seq  = 0;      // 入队序号（队列填写，调用方勿改）；合并不改变它
};

class OpQueue {
public:
    // 容量固定：UI 一帧产生的操作数是有限的，满了说明消费方没在排，
    // 那时逐出/拒绝都是明确的失败，不该靠无限增长把内存吃光。
    static constexpr int kCapacity = 256;

    struct Policy {
        int max_per_drain = 64;             // 单次排出上限；<=0 表示不限制
        int min_frames_between_drain = 1;   // 两次排出的最小间隔帧数；<=0 表示每次都可排
    };

    struct Stats {
        int      pending   = 0;   // 当前待处理条数
        uint64_t pushed    = 0;   // 成功入队次数（含被合并的）
        uint64_t coalesced = 0;   // 其中合并进已有操作的次数
        uint64_t drained   = 0;   // 累计排出条数
        uint64_t evicted   = 0;   // 满队时为腾位置逐出的条数（只逐最旧的可合并项）
        uint64_t rejected  = 0;   // 满队且无可逐出而被拒绝的入队次数
        uint64_t throttled = 0;   // 因未到排出间隔而空排的次数
    };

    void set_policy(Policy p);
    Policy policy() const;

    // 入队。key == 0 或没有同键待处理项时追加到队尾；
    // 有同键待处理项时就地覆盖其参数（位置与 seq 不变）。
    // 返回 false = 队列满且没有可逐出的旧项（全是离散操作，一条都不能丢）。
    // op.seq 由队列填写，传入值被忽略。
    bool push(const Op& op);

    // 排出至多 max 条到 out（调用方保证容量）。返回实际条数：
    // 0 = 队列空，或未到策略允许的排出时机（后者计入 stats.throttled）。
    // 空队列不会消耗排出间隔——只有真的排过才更新间隔基准。
    int drain(Op* out, int max);

    void clear();

    // 帧计数推进：由渲染层每渲染帧调一次，排出间隔以它为基准。
    void advance_frame();

    int pending() const;
    Stats stats() const;

private:
    // 逻辑序号 i（0 = 队头）到环形槽位的换算。
    int slot(int i) const { return (head_ + i) % kCapacity; }

    // 找同 (type,key) 的待处理项，没有返回 -1。
    int find_same(uint32_t type, uint32_t key) const;
    // 找最旧的可合并项（key != 0，满队时的逐出候选），没有返回 -1。
    // 只逐出连续编辑项：拖拽期间每帧都会重推同键，丢一条中间值不丢最终值；
    // 离散项（点击 / 创建 / 删除）一旦逐出就再也不会来，绝不碰。
    int find_oldest_coalescing() const;
    // 保序移除逻辑序号 i 处的项。
    void remove_at(int i);

    mutable std::mutex mu_;
    Op ops_[kCapacity];
    int head_  = 0;
    int count_ = 0;

    Policy policy_;
    Stats  stats_;

    uint64_t seq_             = 0;
    uint64_t frame_           = 0;
    uint64_t last_drain_frame_ = 0;
    bool     ever_drained_    = false;
};

} // namespace gryce_ui

#endif // GRYCE_UI_OP_QUEUE_H
