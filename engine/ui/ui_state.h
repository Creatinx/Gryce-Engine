// ===========================================================================
// engine/ui/ui_state.h — 跨帧控件状态存储（无 ImGui 依赖的纯逻辑）
//
// 解决的问题：ImGui 本身不保存控件的**值**——滑块的值、复选框的勾选、
// 输入框的内容都由调用方持有缓冲。于是每个面板都要自己写一套"本地缓冲"，
// 忘写的症状是"拖一下弹回去"（控件显示的值每帧被外部旧值覆盖）。
//
// 本存储把这份缓冲收归引擎：按稳定 id（ui_id.h）索引，跨帧存活。
// 调用方的模式变成——
//     float v = 0.5f; GUi_StateGetFloat(id, &v);
//     if (GUi_SliderFloat("Opacity", &v, 0, 1, "%.2f")) GUi_StateSetFloat(id, v);
// 缓冲不再属于任何一个栈帧，面板删掉本地缓冲也不抖。
//
// 附带解决"曾经出现过、现在不在了"的状态膨胀：条目带帧戳，压缩时丢弃
// 超过 max_age 没被 touch 的（可见控件每 UI 帧都会被 touch，丢不到它）。
//
// 线程模型：只在 UI 线程使用（回调内，以及渲染层每帧的 begin_frame）。
// 不带锁——它的全部调用点都在同一线程上（UI 回调由 GRender_EndFrame 在
// 主线程驱动，异步渲染线程只碰命令缓冲）。跨线程观察 UI 状态请走操作
// 队列（ui_op_queue.h），那才是 sanctioned 的通道。
// ===========================================================================

#ifndef GRYCE_UI_STATE_H
#define GRYCE_UI_STATE_H

#include <cstdint>
#include <cstddef>

namespace gryce_ui {

// 单个控件的跨帧状态。id == 0 表示空槽。
struct StateEntry {
    uint64_t id         = 0;
    uint32_t last_frame = 0;   // 最后一次被 touch 的帧号（老化判定用）
    float    f          = 0.0f; // 标量（滑块 / 拖拽）
    int32_t  i          = 0;    // 整数（下拉索引 / 计数）
    int32_t  b          = 0;    // 布尔（复选框 / 开关）
    float    vec[4]     = {0.0f, 0.0f, 0.0f, 0.0f};  // DragFloat3 / ColorEdit4
    static constexpr size_t kTextCap = 128;
    char     text[kTextCap] = {};                  // InputText 内容
};

class StateStore {
public:
    // 2 的幂。同时持有上千个带状态控件的界面不存在，装不下说明 id 在泄漏
    // （每帧生成新 id），那时逐出老化条目而不是无限增长。
    static constexpr size_t kCapacity = 2048;
    static constexpr size_t kMask     = kCapacity - 1;
    // 超过这么多帧没被 touch 的条目在压缩时丢弃。默认按 60fps 约 15 秒：
    // 任何可见控件每 UI 帧都会被 touch，间隔节流（--ui-interval N）下也
    // 只是每 N 帧一次，远到不了这个阈值。
    static constexpr uint32_t kMaxAgeFrames = 900;

    // 每帧开头调一次（渲染层自动调；自持帧循环的宿主手动调）。
    void begin_frame(uint32_t frame);

    // 取（没有则新建）并标记本帧用过。新建条目的标量清零、文本清空。
    StateEntry& touch(uint64_t id);
    const StateEntry* find(uint64_t id) const;

    //  typed 快捷方式：读不到时返回调用方给的默认值。
    float get_float(uint64_t id, float default_value) const;
    int   get_int(uint64_t id, int default_value) const;
    bool  get_bool(uint64_t id, bool default_value) const;
    void  get_vec4(uint64_t id, float out[4]) const;
    // 文本：至多拷贝 cap-1 个字符并补 '\0'。返回实际长度（不含结尾）。
    size_t get_text(uint64_t id, char* out, size_t cap) const;

    void set_float(uint64_t id, float v);
    void set_int(uint64_t id, int v);
    void set_bool(uint64_t id, bool v);
    void set_vec4(uint64_t id, const float v[4]);
    void set_text(uint64_t id, const char* s);

    // 立即丢弃超过 max_age 未被 touch 的条目（begin_frame 负载过高时也会调）。
    void prune_stale();
    void clear();

    size_t  size() const { return used_; }
    uint32_t frame() const { return frame_; }

private:
    // 开放寻址（线性探测）的后向移位删除：删一个槽位时，把后继条目里
    // "天然家位不在此坑之后"的搬回来填坑，探测链不断。
    void erase_at(size_t i);

    StateEntry slots_[kCapacity];
    size_t     used_  = 0;
    uint32_t   frame_ = 0;
};

} // namespace gryce_ui

#endif // GRYCE_UI_STATE_H
