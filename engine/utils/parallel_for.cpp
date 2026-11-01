// ===========================================================================
// engine/utils/parallel_for.cpp
// 常驻线程池实现。设计口径见 parallel_for.h。
//
// 两种调度形态共用同一批 worker：
//   parallel_for     —— 取号分发，块数 ≈ 线程数，适合"一遍就算完"的循环
//                       （平流、涡量、边界清零）。
//   parallel_region  —— 固定分区 + 迭代间屏障，适合"多次迭代、每轮都要看到
//                       上一轮完整结果"的求解器（压力 Jacobi）。之所以不能
//                       每轮调一次 parallel_for，是因为一次 condvar 唤醒 +
//                       等待在 8 线程上要几十微秒，而压力求解一轮就是 240 次
//                       （迭代 × 双色），光调度就能吃掉并行省下的全部时间。
// ===========================================================================
#include "utils/parallel_for.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace gryce_engine::utils {

namespace {

struct Pool {
    std::vector<std::thread> workers;

    std::mutex mutex;
    std::condition_variable cv_start;
    std::condition_variable cv_done;

    // ---- 取号式任务（parallel_for）----
    const std::function<void(int, int)>* job = nullptr;
    int chunks = 0;
    int next = 0;
    int remaining = 0;
    uint64_t epoch = 0;

    // ---- 分区式任务（parallel_region）----
    const std::function<void(int, int, int)>* region_fn = nullptr;
    int region_begin = 0;
    int region_end = 0;
    int region_chunks = 0;
    int region_repeats = 0;
    int region_remaining = 0;
    uint64_t region_epoch = 0;

    // ---- 分区任务内部的屏障 ----
    // 高频屏障（压力求解一轮就是几十微秒的工作）不能用 condition_variable：
    // 一次 notify_all + 7 个线程抢锁在 Windows 上要几百微秒，240 轮就吃掉
    // 几十毫秒，比串行还慢。这里用原子计数 + yield 自旋，单次 ~1us 量级。
    // participants 在 parallel_region 发布时已确定，屏障内只读。
    std::atomic<int> barrier_participants{0};
    std::atomic<int> barrier_arrived{0};
    std::atomic<uint32_t> barrier_gen{0};

    bool quit = false;

    void barrier_wait() {
        // 两阶段计数屏障：最后到达者负责"开闸"（清零计数 + 推进代数），
        // 其余线程按代数自旋。yield 而不是忙等，避免把核占满影响同机其它线程。
        const uint32_t gen = barrier_gen.load(std::memory_order_acquire);
        const int participants = barrier_participants.load(std::memory_order_relaxed);
        if (barrier_arrived.fetch_add(1, std::memory_order_acq_rel) + 1 >= participants) {
            barrier_arrived.store(0, std::memory_order_relaxed);
            barrier_gen.fetch_add(1, std::memory_order_release);
        } else {
            while (barrier_gen.load(std::memory_order_acquire) == gen) {
                std::this_thread::yield();
            }
        }
    }

    // [begin, end) 的第 index 块（共 total 块）的边界；最后一块吃掉余数。
    static void chunk_bounds(int begin, int end, int index, int total, int& lo, int& hi) {
        const long long count = end - begin;
        lo = begin + static_cast<int>((count * index) / total);
        hi = begin + static_cast<int>((count * (index + 1)) / total);
    }

    void run_region(const int worker_index, const std::function<void(int, int, int)>* fn,
                    const int begin, const int end, const int total, const int repeats) {
        // worker_index 就是块号；块比参与者少时（小网格）该 worker 只参与屏障。
        const bool has_work = worker_index < total;
        int lo = 0;
        int hi = 0;
        if (has_work) chunk_bounds(begin, end, worker_index, total, lo, hi);
        for (int iter = 0; iter < repeats; ++iter) {
            if (has_work) (*fn)(lo, hi, iter);
            barrier_wait();
        }
        std::lock_guard<std::mutex> lk(mutex);
        if (--region_remaining == 0) cv_done.notify_one();
    }

    void worker_loop(int worker_index) {
        uint64_t seen_epoch = 0;
        uint64_t seen_region = 0;
        for (;;) {
            bool run_job = false;
            bool run_region_now = false;
            const std::function<void(int, int, int)>* rfn = nullptr;
            int rbegin = 0;
            int rend = 0;
            int rchunks = 0;
            int rrepeats = 0;
            {
                std::unique_lock<std::mutex> lk(mutex);
                cv_start.wait(lk, [this, seen_epoch, seen_region] {
                    return quit || epoch != seen_epoch || region_epoch != seen_region;
                });
                if (quit) return;
                if (region_epoch != seen_region) {
                    seen_region = region_epoch;
                    run_region_now = true;
                    // 在锁内快照：发布后这些字段要到下一轮才会被改写，
                    // 但读侧不该依赖"下一次写还没发生"。
                    rfn = region_fn;
                    rbegin = region_begin;
                    rend = region_end;
                    rchunks = region_chunks;
                    rrepeats = region_repeats;
                } else {
                    seen_epoch = epoch;
                    run_job = true;
                }
            }
            if (run_region_now) {
                run_region(worker_index, rfn, rbegin, rend, rchunks, rrepeats);
                continue;
            }
            if (run_job) {
                for (;;) {
                    int index = 0;
                    {
                        std::lock_guard<std::mutex> lk(mutex);
                        if (next >= chunks) break;
                        index = next++;
                    }
                    (*job)(index, chunks);
                }
                {
                    std::lock_guard<std::mutex> lk(mutex);
                    if (--remaining == 0) cv_done.notify_one();
                }
            }
        }
    }
};

// 刻意泄漏：线程池的生命周期与进程同长。做成 static 对象的话，全局析构阶段
// 会有 worker 仍在等 cv_start（join 不掉），退出时可能卡住或崩在已销毁的锁上。
Pool& global_pool() {
    static Pool* pool = [] {
        Pool* p = new Pool();
        const unsigned hw = std::thread::hardware_concurrency();
        int worker_count = (hw > 1) ? static_cast<int>(hw) - 1 : 0;
        // 一次性调优开关：GRYCE_WORKERS 覆盖线程数（0 = 纯串行，用于 A/B 对照）。
        // 只读一次，不在任何热路径上。
        if (const char* env = std::getenv("GRYCE_WORKERS")) {
            const int v = std::atoi(env);
            if (v >= 0) worker_count = v;
        }
        p->workers.reserve(static_cast<size_t>(worker_count));
        for (int i = 0; i < worker_count; ++i) {
            p->workers.emplace_back([p, i] { p->worker_loop(i); });
        }
        return p;
    }();
    return *pool;
}

} // namespace

int parallel_worker_count() {
    return static_cast<int>(global_pool().workers.size());
}

void parallel_for(int begin, int end, const std::function<void(int, int)>& range_fn) {
    const int count = end - begin;
    if (count <= 0) return;

    Pool& pool = global_pool();
    const int worker_count = static_cast<int>(pool.workers.size());
    // 单个元素或没有 worker：直接串行，省掉唤醒/等待的往返。
    if (worker_count <= 0 || count == 1) {
        range_fn(begin, end);
        return;
    }

    const int chunk_count = std::min(count, worker_count + 1);
    // 这个 lambda 留在栈上：它会活到 remaining 归零（下面 cv_done 等到那一刻），
    // worker 在等待期间通过 pool.job 调用它，返回前已不会被引用。
    const std::function<void(int, int)> chunk_fn = [&](int index, int total) {
        int lo = 0;
        int hi = 0;
        Pool::chunk_bounds(begin, end, index, total, lo, hi);
        if (lo < hi) range_fn(lo, hi);
    };

    {
        std::lock_guard<std::mutex> lk(pool.mutex);
        pool.job = &chunk_fn;
        pool.chunks = chunk_count;
        pool.next = 0;
        pool.remaining = worker_count;
        ++pool.epoch;
    }
    pool.cv_start.notify_all();

    for (;;) {
        int index = 0;
        {
            std::lock_guard<std::mutex> lk(pool.mutex);
            if (pool.next >= chunk_count) break;
            index = pool.next++;
        }
        chunk_fn(index, chunk_count);
    }

    std::unique_lock<std::mutex> lk(pool.mutex);
    pool.cv_done.wait(lk, [&pool] { return pool.remaining == 0; });
    pool.job = nullptr;
}

void parallel_region(int begin, int end, int repeats, const std::function<void(int, int, int)>& range_fn) {
    const int count = end - begin;
    if (count <= 0 || repeats <= 0) return;

    Pool& pool = global_pool();
    const int worker_count = static_cast<int>(pool.workers.size());
    if (worker_count <= 0 || count == 1) {
        for (int iter = 0; iter < repeats; ++iter) range_fn(begin, end, iter);
        return;
    }

    const int participants = worker_count + 1;   // worker + 调用线程
    const int chunk_count = std::min(count, participants);
    // 调用线程固定领最后一块：worker i 领块 i，块不够时调用线程只参与屏障。
    const int main_index = participants - 1;
    const bool main_has_work = main_index < chunk_count;
    int main_lo = 0;
    int main_hi = 0;
    if (main_has_work) Pool::chunk_bounds(begin, end, main_index, chunk_count, main_lo, main_hi);

    {
        std::lock_guard<std::mutex> lk(pool.mutex);
        pool.region_fn = &range_fn;
        pool.region_begin = begin;
        pool.region_end = end;
        pool.region_chunks = chunk_count;
        pool.region_repeats = repeats;
        pool.region_remaining = worker_count;
        pool.barrier_participants.store(participants, std::memory_order_relaxed);
        pool.barrier_arrived.store(0, std::memory_order_relaxed);
        ++pool.region_epoch;
    }
    pool.cv_start.notify_all();

    for (int iter = 0; iter < repeats; ++iter) {
        if (main_has_work) range_fn(main_lo, main_hi, iter);
        pool.barrier_wait();
    }

    std::unique_lock<std::mutex> lk(pool.mutex);
    pool.cv_done.wait(lk, [&pool] { return pool.region_remaining == 0; });
    pool.region_fn = nullptr;
}

} // namespace gryce_engine::utils
