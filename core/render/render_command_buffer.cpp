 #include "render_command_buffer.h"

#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

RenderCommandBuffer::RenderCommandBuffer() {
    for (auto& f : frames_) {
        f.commands.reserve(1024);
    }
}

RenderCommandBuffer::~RenderCommandBuffer() {
    shutdown();
}

void RenderCommandBuffer::push(RenderCommand&& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_[write_index_].commands.emplace_back(std::move(cmd));
}

void RenderCommandBuffer::push_typed(RenderCommandTyped&& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_[write_index_].commands.emplace_back(std::move(cmd));
}

void RenderCommandBuffer::submit() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (shutdown_) return;

    int submitted = write_index_;
    frames_[submitted].state = Frame::Submitted;
    frames_[submitted].seq = next_seq_++;

    // 找一个 Free 的 frame 作为下一帧写入目标
    int new_write = -1;
    for (int i = 0; i < kFrameCount; ++i) {
        if (i != submitted && frames_[i].state == Frame::Free) {
            new_write = i;
            break;
        }
    }

    // 如果没有 Free，等待渲染线程释放（正常三缓冲不会触发）
    if (new_write < 0) {
        cv_main_.wait(lock, [this, &new_write] {
            if (shutdown_) return true;
            for (int i = 0; i < kFrameCount; ++i) {
                if (frames_[i].state == Frame::Free) {
                    new_write = i;
                    return true;
                }
            }
            return false;
        });
    }

    if (shutdown_) return;

    write_index_ = new_write;
    frames_[write_index_].commands.clear();
    frames_[write_index_].state = Frame::Free;

    lock.unlock();
    cv_render_.notify_one();
}

std::vector<RenderCommandItem>* RenderCommandBuffer::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_render_.wait(lock, [this] {
        if (shutdown_) return true;
        for (int i = 0; i < kFrameCount; ++i) {
            if (frames_[i].state == Frame::Submitted) return true;
        }
        return false;
    });

    // 即使 shutdown，也先把已提交的 frame 处理完
    int idx = -1;
    uint64_t min_seq = UINT64_MAX;
    for (int i = 0; i < kFrameCount; ++i) {
        if (frames_[i].state == Frame::Submitted && frames_[i].seq < min_seq) {
            min_seq = frames_[i].seq;
            idx = i;
        }
    }

    if (idx < 0) return nullptr;

    frames_[idx].state = Frame::Rendering;
    return &frames_[idx].commands;
}

void RenderCommandBuffer::release() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < kFrameCount; ++i) {
        if (frames_[i].state == Frame::Rendering) {
            frames_[i].state = Frame::Free;
            frames_[i].commands.clear();
            if (frames_[i].seq > completed_seq_) {
                completed_seq_ = frames_[i].seq;
            }
            cv_main_.notify_one();
            return;
        }
    }
}

void RenderCommandBuffer::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    cv_main_.notify_all();
    cv_render_.notify_all();
}

std::vector<RenderCommandItem> RenderCommandBuffer::drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RenderCommandItem> pending;
    pending.swap(frames_[write_index_].commands);
    return pending;
}

bool RenderCommandBuffer::is_shutdown() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shutdown_;
}

void RenderCommandBuffer::wait_for_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_main_.wait(lock, [this] {
        if (shutdown_) return true;
        for (int i = 0; i < kFrameCount; ++i) {
            if (frames_[i].state == Frame::Submitted || frames_[i].state == Frame::Rendering) {
                return false;
            }
        }
        return true;
    });
}

uint64_t RenderCommandBuffer::current_write_seq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_seq_;
}

uint64_t RenderCommandBuffer::completed_seq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_seq_;
}

} // namespace gryce_engine::render
