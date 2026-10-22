// ===========================================================================
// engine/ui/ui_state.cpp — 跨帧控件状态存储的实现
//
// 开放寻址 + 线性探测。id 本身就是 64 位哈希（ui_id.h），槽位直接取
// id & kMask，不需要再算一遍。
//
// 删除用后向移位而不是墓碑：墓碑会让探测链变长、且需要额外一遍重建，
// 而压缩本来就是低频操作（负载过 3/4 才触发），一遍 walk 即可。
// ===========================================================================

#include "ui/ui_state.h"

#include <cstring>

namespace gryce_ui {

void StateStore::begin_frame(uint32_t frame) {
    frame_ = frame;
    // 负载过 3/4 才压缩：常见负载远低于此，这条判断每帧只是两次整数运算。
    if (used_ * 4 > kCapacity * 3) prune_stale();
}

StateEntry& StateStore::touch(uint64_t id) {
    size_t i = static_cast<size_t>(id) & kMask;
    while (slots_[i].id != 0) {
        if (slots_[i].id == id) {
            slots_[i].last_frame = frame_;
            return slots_[i];
        }
        i = (i + 1) & kMask;
    }

    // 未命中且接近满载：先压缩（丢老化条目）再重新找插入点。压缩后表一定
    // 变空（至少丢掉刚越界的那部分），下面的探测必然能找到空槽。
    if ((used_ + 1) * 4 > kCapacity * 3) {
        prune_stale();
        i = static_cast<size_t>(id) & kMask;
        while (slots_[i].id != 0) {
            if (slots_[i].id == id) {
                slots_[i].last_frame = frame_;
                return slots_[i];
            }
            i = (i + 1) & kMask;
        }
    }

    slots_[i].id = id;
    slots_[i].last_frame = frame_;
    slots_[i].f = 0.0f;
    slots_[i].i = 0;
    slots_[i].b = 0;
    for (float& v : slots_[i].vec) v = 0.0f;
    slots_[i].text[0] = '\0';
    ++used_;
    return slots_[i];
}

const StateEntry* StateStore::find(uint64_t id) const {
    size_t i = static_cast<size_t>(id) & kMask;
    while (slots_[i].id != 0) {
        if (slots_[i].id == id) return &slots_[i];
        i = (i + 1) & kMask;
    }
    return nullptr;
}

float StateStore::get_float(uint64_t id, float default_value) const {
    const StateEntry* e = find(id);
    return e ? e->f : default_value;
}

int StateStore::get_int(uint64_t id, int default_value) const {
    const StateEntry* e = find(id);
    return e ? static_cast<int>(e->i) : default_value;
}

bool StateStore::get_bool(uint64_t id, bool default_value) const {
    const StateEntry* e = find(id);
    return e ? (e->b != 0) : default_value;
}

void StateStore::get_vec4(uint64_t id, float out[4]) const {
    const StateEntry* e = find(id);
    if (e) {
        for (int k = 0; k < 4; ++k) out[k] = e->vec[k];
    } else {
        for (int k = 0; k < 4; ++k) out[k] = 0.0f;
    }
}

size_t StateStore::get_text(uint64_t id, char* out, size_t cap) const {
    if (!out || cap == 0) return 0;
    const StateEntry* e = find(id);
    if (!e) {
        out[0] = '\0';
        return 0;
    }
    const size_t n = std::strlen(e->text);
    const size_t copy = n < cap - 1 ? n : cap - 1;
    std::memcpy(out, e->text, copy);
    out[copy] = '\0';
    return copy;
}

void StateStore::set_float(uint64_t id, float v) { touch(id).f = v; }
void StateStore::set_int(uint64_t id, int v) { touch(id).i = static_cast<int32_t>(v); }
void StateStore::set_bool(uint64_t id, bool v) { touch(id).b = v ? 1 : 0; }

void StateStore::set_vec4(uint64_t id, const float v[4]) {
    StateEntry& e = touch(id);
    for (int k = 0; k < 4; ++k) e.vec[k] = v ? v[k] : 0.0f;
}

void StateStore::set_text(uint64_t id, const char* s) {
    StateEntry& e = touch(id);
    if (!s) {
        e.text[0] = '\0';
        return;
    }
    size_t n = std::strlen(s);
    if (n >= StateEntry::kTextCap) n = StateEntry::kTextCap - 1;
    std::memcpy(e.text, s, n);
    e.text[n] = '\0';
}

void StateStore::prune_stale() {
    // 边删边重查当前坑位：erase_at 会把后面某条搬来填坑，而它同样可能已老化。
    // 不重查就会漏掉这些"被搬过来后又过期"的条目（表现为压缩后残留一片）。
    size_t i = 0;
    while (i < kCapacity) {
        if (slots_[i].id == 0) { ++i; continue; }
        // 帧号回绕（极端长时间运行）时差值会非常大，条目被判为老化丢弃——
        // 可接受：真用到的控件下一帧就被重新 touch 建出来。
        if (frame_ - slots_[i].last_frame > kMaxAgeFrames) {
            erase_at(i);
            continue;   // 坑位已被填入，重新判断这一格
        }
        ++i;
    }
}

void StateStore::clear() {
    for (size_t i = 0; i < kCapacity; ++i) slots_[i].id = 0;
    used_ = 0;
}

void StateStore::erase_at(size_t hole) {
    --used_;
    size_t j = hole;
    for (;;) {
        slots_[hole].id = 0;   // 当前坑位
        // 向后扫描：找到第一个"可以搬来填坑"的条目就搬，然后坑位随它后移
        for (;;) {
            j = (j + 1) & kMask;
            if (slots_[j].id == 0) return;   // 后面全空，这条链修完了
            const size_t home = static_cast<size_t>(slots_[j].id) & kMask;
            // home 是否落在 (hole, j] 循环区间内：在，说明它的探测链经过
            // hole，不能前移（前移它的后继就找不到它）；不在，就可以填坑。
            const bool between = (hole <= j) ? (hole < home && home <= j)
                                             : (hole < home || home <= j);
            if (!between) break;
        }
        slots_[hole] = slots_[j];
        hole = j;
    }
}

} // namespace gryce_ui
