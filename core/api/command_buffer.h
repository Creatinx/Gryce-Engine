#pragma once

#include "GryceCore/types.h"

#include <atomic>
#include <array>
#include <mutex>

namespace gryce_core {

// ---------------------------------------------------------------------------
// Lock-free double-buffered command queue.
// Producer (Editor thread) writes to front; Consumer (Core thread) reads back.
// ---------------------------------------------------------------------------
inline constexpr size_t k_cmd_buffer_capacity = 8192;

class CommandBuffer {
public:
    CommandBuffer();

    // Thread-safe: called from Editor thread.
    bool push(const GCommand& cmd);
    bool push_batch(const GCommand* cmds, int count, int* out_dropped);

    // Called from Core thread only (BeginFrame).
    void swap();
    const GCommand* consume(int& out_count);

    int capacity_remaining() const;
    int dropped_since_last_call();

private:
    using Buffer = std::array<GCommand, k_cmd_buffer_capacity>;

    Buffer buffers_[2];
    std::atomic<size_t> write_idx_{0};   // write position in front buffer
    std::atomic<size_t> front_{0};       // 0 or 1: which buffer is front (producer writes)
    size_t read_count_ = 0;              // how many commands are in back buffer after swap

    std::atomic<int> dropped_total_{0};
    int dropped_snapshot_ = 0;
};

} // namespace gryce_core
