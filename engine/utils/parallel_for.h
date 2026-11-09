// ===========================================================================
// engine/utils/parallel_for.h
// 进程级常驻线程池 + 按区间切分的 parallel_for。
//
// 为什么不用 std::async / 每帧新建线程：流体、蒙皮、剔除这类每帧都要跑的
// 数据并行循环，走到这里时单次任务量在毫秒量级，而创建/回收一个线程在
// Windows 上要几十微秒，按帧新建会把并行省下来的时间又还回去。这里懒初始化
// 一次、之后只做"唤醒 + 取块"，空闲线程在条件变量上睡。
//
// 调用线程**也参与计算**（chunks = workers + 1），所以单核机器上退化为串行
// 也不会出现"任务在后台、调用方空等"的浪费。
//
// 使用约束：range_fn 对同一个 i 只被一个线程访问，调用方保证不同 i 之间没有
// 写冲突（典型做法：按 z 切片，每个 pass 只写自己那一片的网格）。
// ===========================================================================
#pragma once

#include <functional>

#include "export.h"

namespace gryce_engine::utils {

// 把 [begin, end) 切成若干连续区间，在 <= workers+1 个线程上并发执行
// range_fn(lo, hi)。返回时所有区间必定已执行完成。
//
// 区间总数取 min(end-begin, workers+1)：块数少于线程数时多切只会增加调度
// 开销；块数远多于线程数则不适合（这里按 z 切片，块数天然等于行数）。
GRYCE_API void parallel_for(int begin, int end, const std::function<void(int, int)>& range_fn);

// 固定分区 + 迭代间屏障：[begin, end) 切成固定块，每块由一个线程全程负责，
// 每完成一轮 repeats 就与其余线程同步一次。range_fn(lo, hi, iter)。
//
// 用于"迭代之间必须看到上一轮完整结果"的求解器（压力 Jacobi 的 240 轮双色
// 扫描）。这类场景不能用 repeats 次 parallel_for：一次唤醒+等待在 8 线程上是
// 几十微秒，240 次调度开销就能吃光并行省下的时间。
//
// 同一 iter 内不同块之间不允许有写冲突；跨 iter 的依赖由屏障保证。
GRYCE_API void parallel_region(int begin, int end, int repeats,
                              const std::function<void(int, int, int)>& range_fn);

// 线程池可用工作线程数（不含调用线程）。仅用于调用方自己决定分块策略。
GRYCE_API int parallel_worker_count();

} // namespace gryce_engine::utils
