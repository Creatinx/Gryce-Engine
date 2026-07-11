#include "async_loader.h"

#include <thread>
#include "utils/glog/glog_lib.h"

namespace gryce_engine::assets {

AsyncLoader::AsyncLoader() = default;

AsyncLoader::~AsyncLoader() {
    shutdown();
}

AsyncLoader& AsyncLoader::instance() {
    static AsyncLoader loader;
    return loader;
}

void AsyncLoader::start(int num_threads) {
    if (running_) return;

    if (num_threads <= 0) {
        num_threads = static_cast<int>(std::thread::hardware_concurrency());
        if (num_threads <= 0) num_threads = 2;
    }

    running_ = true;
    for (int i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&AsyncLoader::worker_thread, this);
    }
    GLOG_INFO("AsyncLoader started with {} worker threads", num_threads);
}

void AsyncLoader::shutdown() {
    if (!running_) return;

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        running_ = false;
    }
    cv_.notify_all();

    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();

    // 清空所有任务
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        tasks_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        completed_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_.clear();
    }

    GLOG_INFO("AsyncLoader shutdown");
}

bool AsyncLoader::submit(const std::string& path, std::type_index type,
                         std::function<void()> worker,
                         std::function<void()> on_complete) {
    if (!running_) start();

    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = tasks_.find(path);
        if (it != tasks_.end()) {
            // 已有任务，追加回调
            auto& task = it->second;
            if (on_complete) {
                LoadingState expected = LoadingState::Pending;
                if (task->state.compare_exchange_strong(expected, LoadingState::Pending)) {
                    // 还在 Pending，可以追加回调
                    task->callbacks.push_back(std::move(on_complete));
                } else {
                    // 已经在 Loading / Ready / Failed
                    if (task->state.load() == LoadingState::Ready || task->state.load() == LoadingState::Failed) {
                        // 已完成，立即回调
                        if (on_complete) on_complete();
                    } else {
                        task->callbacks.push_back(std::move(on_complete));
                    }
                }
            }
            return false;
        }

        // 新建任务
        auto task = std::make_shared<LoadingTask>(path, type, std::move(worker), std::move(on_complete));
        task->state.store(LoadingState::Pending);
        tasks_[path] = task;

        {
            std::lock_guard<std::mutex> qlock(queue_mutex_);
            queue_.push_back(task);
        }
        cv_.notify_one();
        return true;
    }
}

void AsyncLoader::poll() {
    std::deque<std::shared_ptr<LoadingTask>> done;
    {
        std::lock_guard<std::mutex> lock(completed_mutex_);
        done.swap(completed_);
    }

    for (auto& task : done) {
        for (auto& cb : task->callbacks) {
            if (cb) cb();
        }
        task->callbacks.clear();

        // 从 tasks_ 中移除已完成任务
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            tasks_.erase(task->path);
        }
    }
}

LoadingState AsyncLoader::get_state(const std::string& path) const {
    std::lock_guard<std::mutex> lock(tasks_mutex_);
    auto it = tasks_.find(path);
    if (it != tasks_.end()) {
        return it->second->state.load();
    }
    return LoadingState::Pending; // 未知状态
}

bool AsyncLoader::is_loading(const std::string& path) const {
    LoadingState s = get_state(path);
    return s == LoadingState::Pending || s == LoadingState::Loading;
}

void AsyncLoader::worker_thread() {
    while (running_) {
        std::shared_ptr<LoadingTask> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
            if (!running_) break;
            if (queue_.empty()) continue;
            task = queue_.front();
            queue_.pop_front();
        }

        if (!task) continue;

        task->state.store(LoadingState::Loading);

        try {
            task->worker();
            task->state.store(LoadingState::Ready);
        } catch (const std::exception& e) {
            GLOG_ERROR("AsyncLoader: failed to load '{}': {}", task->path, e.what());
            task->state.store(LoadingState::Failed);
        }

        {
            std::lock_guard<std::mutex> lock(completed_mutex_);
            completed_.push_back(task);
        }
    }
}

} // namespace gryce_engine::assets
