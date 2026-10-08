#pragma once

// ---------------------------------------------------------------------------
// GpuProfilerStats — GPU 分段计时的累计统计（GL / Vulkan 后端共用）
//
// 设计要点：
//   - 时间戳由后端各自的硬件计时器采集（GL: GL_TIME_ELAPSED query；
//     Vulkan: vkCmdWriteTimestamp），**不在 CPU 上估算**，也不强制 GPU 同步；
//   - 结果在"帧槽被复用时"结算（那时帧 fence 已保证该帧执行完毕），
//     因此测量本身几乎不影响帧时间；
//   - 每个 pass 按名字累计，帧数由 set_frames() 记录，便于输出平均值。
//   - 打印时区分 "ms"（单次执行耗时）与 "ms/frame"（摊到所有帧的成本）：
//     被缓存/节流的 pass（probe、shadow）前者可能远大于后者，按前者判读
//     会得出错误的"占比"结论。详见 print() 的注释。
//
// 注意：至少需要一个热身帧，第一帧的 pass 集合可能不同（着色器编译、
// 目标重建等），dump 时记得跳过前几帧。
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gryce_engine::render {

class GpuProfilerStats {
public:
    void add(const std::string& name, double ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& e = entries_[name];
        e.sum_ms += ms;
        e.samples += 1;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        frames_ = 0;
    }

    void set_frames(int frames) {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_ = frames;
    }
    int frames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.empty();
    }

    // 按平均耗时降序打印。frames <= 0 时按 samples 求平均。
    //
    // 输出两列关键指标，避免"按样本平均"被误读成"每帧成本"：
    //   ms           = sum_ms / samples —— 该 pass **实际执行时**的平均单次耗时
    //   ms/frame.am  = sum_ms / frames  —— **摊到每一帧**的成本
    //   n            = 样本数（该 pass 在多少帧里真正跑过）
    // 对每帧都跑的 pass（scene/prepass/tonemap）两者相等；对被缓存/节流的
    // pass（probe/shadow）两者可能差 1~2 个数量级 —— 这时按 ms 判读会得到
    // 完全错误的"占比"，必须看 ms/frame.am。
    void print(const char* api_name) const {
        std::vector<std::pair<std::string, Entry>> rows;
        int frames = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rows.assign(entries_.begin(), entries_.end());
            frames = frames_;
        }
        if (rows.empty()) {
            std::fprintf(stdout, "[gpu] %s: 没有可用样本（后端不支持 GPU 计时或未渲染）\n",
                         api_name ? api_name : "?");
            return;
        }
        const int denom = frames > 0 ? frames : 1;
        std::sort(rows.begin(), rows.end(), [denom](const auto& a, const auto& b) {
            return amortized(a.second, denom) > amortized(b.second, denom);
        });
        double total_am = 0.0;
        for (const auto& r : rows) total_am += amortized(r.second, denom);

        std::fprintf(stdout, "[gpu] %s  平均 GPU 耗时（%d 帧）\n", api_name ? api_name : "?", denom);
        std::fprintf(stdout, "[gpu] %-20s %9s %9s %7s  %s\n",
                     "pass", "ms", "ms/frame", "%", "n");
        for (const auto& r : rows) {
            const double ms = avg(r.second);
            const double am = amortized(r.second, denom);
            const double pct = total_am > 0.0 ? (am / total_am * 100.0) : 0.0;
            std::fprintf(stdout, "[gpu] %-20s %9.3f %9.3f %6.1f%%  n=%llu\n",
                         r.first.c_str(), ms, am, pct,
                         static_cast<unsigned long long>(r.second.samples));
        }
        std::fprintf(stdout, "[gpu] %-20s %9s %9.3f\n", "合计(摊到每帧)", "", total_am);
    }

private:
    struct Entry {
        double sum_ms = 0.0;
        uint64_t samples = 0;
    };

    static double avg(const Entry& e) {
        return e.samples > 0 ? e.sum_ms / static_cast<double>(e.samples) : 0.0;
    }

    // 摊到每一帧的成本：即使该 pass 只跑过 1 帧，也均摊到全部 frames。
    static double amortized(const Entry& e, int frames) {
        return frames > 0 ? e.sum_ms / static_cast<double>(frames) : 0.0;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    int frames_ = 0;
};

} // namespace gryce_engine::render
