// ===========================================================================
// engine/api/ui_op_queue.cpp — UI 操作队列实现（纯逻辑，无 ImGui / 无引擎依赖）
//
// 单独一个 TU 是为了可测：GryceUiTests 直接把它编进测试可执行文件，
// 不需要 GryceRenderer.dll、不需要 GPU 与窗口（与 GryceMathTests 同构）。
// ===========================================================================

#include "api/ui_op_queue.h"
#include "api/ui_op_queue_internal.h"

#include <algorithm>

namespace gryce_ui {

OpQueue& op_queue() {
    static OpQueue queue;
    return queue;
}

void OpQueue::set_policy(Policy p) {
    std::lock_guard lock(mu_);
    policy_ = p;
}

OpQueue::Policy OpQueue::policy() const {
    std::lock_guard lock(mu_);
    return policy_;
}

bool OpQueue::push(const Op& op) {
    std::lock_guard lock(mu_);

    Op entry = op;
    entry.seq = ++seq_;

    if (entry.key != 0) {
        const int same = find_same(entry.type, entry.key);
        if (same >= 0) {
            // 就地覆盖：FIFO 位置与 seq 都不动。seq 保持首次入队的值，
            // 因为"这个键的当前值"从第一次入队就算待处理了。
            Op& dst = ops_[slot(same)];
            dst.arg = entry.arg;
            for (int i = 0; i < 4; ++i) dst.vec[i] = entry.vec[i];
            ++stats_.pushed;
            ++stats_.coalesced;
            return true;
        }
    }

    if (count_ == kCapacity) {
        // 满队：只逐出最旧的"可合并项"（key != 0 的连续编辑项——拖拽期间
        // 每帧都会重推同键，逐出一条中间值不丢最终结果），离散项（点击这类）
        // 绝不逐出。没有可逐出的就拒绝入队并计数，让调用方与测试都能看见
        // "消费方没在排"这个事实，而不是静默吞掉用户操作。
        const int victim = find_oldest_coalescing();
        if (victim < 0) {
            ++stats_.rejected;
            return false;
        }
        remove_at(victim);
        ++stats_.evicted;
    }

    ops_[slot(count_)] = entry;
    ++count_;
    ++stats_.pushed;
    stats_.pending = count_;
    return true;
}

int OpQueue::drain(Op* out, int max) {
    std::lock_guard lock(mu_);

    if (out == nullptr || max <= 0) return 0;
    if (count_ == 0) return 0;

    // 间隔闸：首次排出不受限（ever_drained_ 为假），之后两次排出之间
    // 至少要隔 min_frames_between_drain 个推进过的帧。
    if (ever_drained_ && policy_.min_frames_between_drain > 0 &&
        frame_ - last_drain_frame_ < static_cast<uint64_t>(policy_.min_frames_between_drain)) {
        ++stats_.throttled;
        return 0;
    }

    int n = max;
    if (policy_.max_per_drain > 0 && n > policy_.max_per_drain) n = policy_.max_per_drain;
    if (n > count_) n = count_;

    // 先按**原始**队头取齐再统一推进：循环里边取槽位边改 head_ 会让
    // slot(i) 跳到已前进过的位置，第二条起每条都跳过一位。
    const int base = head_;
    for (int i = 0; i < n; ++i) {
        out[i] = ops_[(base + i) % kCapacity];
    }
    head_ = (base + n) % kCapacity;
    count_ -= n;

    stats_.drained += static_cast<uint64_t>(n);
    stats_.pending = count_;
    last_drain_frame_ = frame_;
    ever_drained_ = true;
    return n;
}

void OpQueue::clear() {
    std::lock_guard lock(mu_);
    head_ = 0;
    count_ = 0;
    stats_.pending = 0;
}

void OpQueue::advance_frame() {
    std::lock_guard lock(mu_);
    ++frame_;
}

int OpQueue::pending() const {
    std::lock_guard lock(mu_);
    return count_;
}

OpQueue::Stats OpQueue::stats() const {
    std::lock_guard lock(mu_);
    return stats_;
}

int OpQueue::find_same(uint32_t type, uint32_t key) const {
    for (int i = 0; i < count_; ++i) {
        const Op& o = ops_[slot(i)];
        if (o.type == type && o.key == key) return i;
    }
    return -1;
}

int OpQueue::find_oldest_coalescing() const {
    for (int i = 0; i < count_; ++i) {
        if (ops_[slot(i)].key != 0) return i;
    }
    return -1;
}

void OpQueue::remove_at(int i) {
    for (int k = i; k < count_ - 1; ++k) {
        ops_[slot(k)] = ops_[slot(k + 1)];
    }
    --count_;
    stats_.pending = count_;
}

} // namespace gryce_ui
