#include "command_buffer.h"

#include <cstring>

namespace gryce_core {

CommandBuffer::CommandBuffer() {
    buffers_[0].fill(GCommand{});
    buffers_[1].fill(GCommand{});
}

bool CommandBuffer::push(const GCommand& cmd) {
    size_t front = front_.load(std::memory_order_relaxed);
    size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed);
    if (idx >= k_cmd_buffer_capacity) {
        // Buffer full: drop this command cleanly instead of wrapping and
        // overwriting entries that swap() may be about to consume.
        dropped_total_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    buffers_[front][idx] = cmd;
    return true;
}

bool CommandBuffer::push_batch(const GCommand* cmds, int count, int* out_dropped) {
    int dropped = 0;
    for (int i = 0; i < count; ++i) {
        if (!push(cmds[i])) {
            ++dropped;
        }
    }
    if (out_dropped) *out_dropped = dropped;
    return dropped == 0;
}

void CommandBuffer::swap() {
    size_t old_front = front_.load(std::memory_order_relaxed);
    size_t new_front = 1 - old_front;

    // Capture how many commands were written to the old front
    read_count_ = write_idx_.exchange(0, std::memory_order_relaxed);
    if (read_count_ > k_cmd_buffer_capacity) {
        read_count_ = k_cmd_buffer_capacity;
    }

    front_.store(new_front, std::memory_order_relaxed);

    // Snapshot dropped count
    dropped_snapshot_ = dropped_total_.exchange(0, std::memory_order_relaxed);
}

const GCommand* CommandBuffer::consume(int& out_count) {
    out_count = static_cast<int>(read_count_);
    read_count_ = 0;
    // Return the buffer that WAS front (now back after swap)
    size_t back = 1 - front_.load(std::memory_order_relaxed);
    return buffers_[back].data();
}

int CommandBuffer::capacity_remaining() const {
    size_t idx = write_idx_.load(std::memory_order_relaxed);
    return static_cast<int>(k_cmd_buffer_capacity - std::min(idx, k_cmd_buffer_capacity));
}

int CommandBuffer::dropped_since_last_call() {
    return dropped_snapshot_;
}

} // namespace gryce_core
