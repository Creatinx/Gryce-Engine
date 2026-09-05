#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>

namespace gryce_engine::assets {

// ---------------------------------------------------------------------------
// LoadingState — 异步加载状态
// ---------------------------------------------------------------------------
enum class LoadingState { Pending, Loading, Ready, Failed };

// ---------------------------------------------------------------------------
// LoadingTask — 单个异步加载任务
// ---------------------------------------------------------------------------
struct LoadingTask {
    std::string path;
    std::type_index type;
    std::function<void()> worker;
    std::vector<std::function<void()>> callbacks;
    std::atomic<LoadingState> state{LoadingState::Pending};

    LoadingTask(const std::string& p, std::type_index t,
                std::function<void()> w,
                std::function<void()> cb)
        : path(p), type(t), worker(std::move(w)) {
        if (cb) callbacks.push_back(std::move(cb));
    }
};

// ---------------------------------------------------------------------------
// AsyncLoader — 异步资源加载器（线程池）
// 按 path 去重，避免同一资源并发加载两次。
// 加载完成后通过 poll() 在主线程执行回调。
// ---------------------------------------------------------------------------
class AsyncLoader {
public:
    static AsyncLoader& instance();

    ~AsyncLoader();

    // 启动线程池。num_threads=0 则自动检测。
    void start(int num_threads = 0);
    void shutdown();

    // 提交异步加载任务。返回 true 表示新任务已提交；
    // 若同 path 已存在任务，则只追加回调，返回 false。
    bool submit(const std::string& path, std::type_index type,
                std::function<void()> worker,
                std::function<void()> on_complete);

    // 执行所有已完成任务的回调（主线程每帧调用）
    void poll();

    // 查询某 path 的加载状态
    LoadingState get_state(const std::string& path) const;

    // 某 path 是否正在加载（Pending / Loading）
    bool is_loading(const std::string& path) const;

private:
    AsyncLoader();
    AsyncLoader(const AsyncLoader&) = delete;
    AsyncLoader& operator=(const AsyncLoader&) = delete;

    void worker_thread();

    std::vector<std::thread> workers_;
    std::deque<std::shared_ptr<LoadingTask>> queue_;
    std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::atomic<bool> running_{false};

    // path -> 任务，用于去重
    std::unordered_map<std::string, std::shared_ptr<LoadingTask>> tasks_;
    mutable std::mutex tasks_mutex_;

    // 已完成任务（等待主线程 poll 回调）
    std::deque<std::shared_ptr<LoadingTask>> completed_;
    std::mutex completed_mutex_;
};

} // namespace gryce_engine::assets
