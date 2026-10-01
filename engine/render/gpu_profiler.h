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
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return avg(a.second) > avg(b.second);
        });
        double total = 0.0;
        for (const auto& r : rows) total += avg(r.second);

        const int denom = frames > 0 ? frames : 1;
        std::fprintf(stdout, "[gpu] %s  平均 GPU 耗时（%d 帧）\n", api_name ? api_name : "?", denom);
        std::fprintf(stdout, "[gpu] %-22s %10s %10s\n", "pass", "ms/frame", "%");
        for (const auto& r : rows) {
            const double ms = avg(r.second);
            const double pct = total > 0.0 ? (ms / total * 100.0) : 0.0;
            std::fprintf(stdout, "[gpu] %-22s %10.3f %9.1f%%\n", r.first.c_str(), ms, pct);
        }
        std::fprintf(stdout, "[gpu] %-22s %10.3f\n", "合计", total);
    }

private:
    struct Entry {
        double sum_ms = 0.0;
        uint64_t samples = 0;
    };

    static double avg(const Entry& e) {
        return e.samples > 0 ? e.sum_ms / static_cast<double>(e.samples) : 0.0;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> entries_;
    int frames_ = 0;
};

} // namespace gryce_engine::render
