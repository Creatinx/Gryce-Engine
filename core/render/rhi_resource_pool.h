#pragma once

#include "render/rhi_handle.h"

#include <vector>
#include <cstdint>
#include <limits>
#include <utility>
#include <memory>

namespace gryce_engine::render {

// ---------------------------------------------------------------------------
// RHIResourcePool — 类型安全 GPU 资源池
// ---------------------------------------------------------------------------
// - 使用 unique_ptr 堆存储，避免对象必须可移动/可复制
// - creation 在主线程；deletion 通过标记或渲染线程安全执行
// - 不保证线程安全，调用方负责在合适线程访问

template<typename T>
class RHIResourcePool {
public:
    struct Slot {
        std::unique_ptr<T> object;
        uint32_t generation = 1;
        bool alive = false;
    };

    RHIResourcePool() = default;
    explicit RHIResourcePool(uint32_t initial_capacity) { slots_.reserve(initial_capacity); }

    template<typename... Args>
    uint32_t allocate(Args&&... args) {
        if (!free_list_.empty()) {
            uint32_t index = free_list_.back();
            free_list_.pop_back();
            Slot& slot = slots_[index];
            slot.object = std::make_unique<T>(std::forward<Args>(args)...);
            slot.generation++;
            if (slot.generation == 0) slot.generation = 1;
            slot.alive = true;
            return index;
        }
        uint32_t index = static_cast<uint32_t>(slots_.size());
        Slot slot;
        slot.object = std::make_unique<T>(std::forward<Args>(args)...);
        slot.alive = true;
        slots_.push_back(std::move(slot));
        return index;
    }

    void deallocate(uint32_t index) {
        if (index >= slots_.size() || !slots_[index].alive) return;
        slots_[index].alive = false;
        slots_[index].object.reset();
        free_list_.push_back(index);
    }

    bool is_alive(uint32_t index, uint32_t generation) const {
        return index < slots_.size() && slots_[index].alive && slots_[index].generation == generation;
    }

    T* get(uint32_t index) {
        if (index >= slots_.size() || !slots_[index].alive) return nullptr;
        return slots_[index].object.get();
    }

    const T* get(uint32_t index) const {
        if (index >= slots_.size() || !slots_[index].alive) return nullptr;
        return slots_[index].object.get();
    }

    T* get_if_alive(uint32_t index, uint32_t generation) {
        if (!is_alive(index, generation)) return nullptr;
        return slots_[index].object.get();
    }

    const T* get_if_alive(uint32_t index, uint32_t generation) const {
        if (!is_alive(index, generation)) return nullptr;
        return slots_[index].object.get();
    }

    uint32_t generation(uint32_t index) const {
        if (index >= slots_.size()) return 0;
        return slots_[index].generation;
    }

    size_t size() const { return slots_.size(); }
    size_t alive_count() const {
        size_t count = 0;
        for (const auto& slot : slots_) {
            if (slot.alive) ++count;
        }
        return count;
    }

    void clear() {
        for (auto& slot : slots_) {
            if (slot.alive) {
                slot.alive = false;
                slot.object.reset();
            }
        }
        free_list_.clear();
        for (uint32_t i = 0; i < static_cast<uint32_t>(slots_.size()); ++i) {
            free_list_.push_back(i);
        }
    }

private:
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_list_;
};

} // namespace gryce_engine::render
