// ===========================================================================
// engine/ui/ui_id.h — 稳定 id：跨帧控件状态的键
//
// 控件状态（滑块的值、折叠头的展开态、拖拽缓冲）要跨帧存活，就需要一个稳定
// 的键。用标签文本做键不行：同一个面板里"Position"会出现多次（每个实体一
// 份）；用指针更不行（实体可能每帧重建）。所以收编成"标签 + 作用域种子"
// 的 64 位哈希：同一个逻辑控件传同样的 (label, seed)，跨帧拿到同一个 id。
//
// seed 的取法：面板/实体/资产的身份（UUID、句柄、路径哈希）。没有身份可循
// 的全局控件（工具面板自己的开关）用 0 即可。
//
// 哈希冲突的处置：不处置。64 位 FNV-1a 在几千个控件的量级上冲突概率可以
// 忽略；真冲突的表现是"两个控件共享一份状态"，肉眼可见、可复现，比加一摊
// 探测代码便宜。需要绝对唯一时用引擎实体 UUID 做 seed。
// ===========================================================================

#ifndef GRYCE_UI_ID_H
#define GRYCE_UI_ID_H

#include <cstdint>

namespace gryce_ui {

// FNV-1a 64。返回非 0：0 被状态存储保留为"空槽"标记（见 ui_state.h）。
inline uint64_t hash_label(const char* label, uint64_t seed = 0) {
    uint64_t h = 1469598103934665603ULL ^ seed;
    if (label) {
        for (const char* p = label; *p; ++p) {
            h ^= static_cast<uint64_t>(static_cast<unsigned char>(*p));
            h *= 1099511628211ULL;
        }
    }
    // 种子也参与哈希：同标签不同 seed 必须是不同的 id
    for (int i = 0; i < 8; ++i) {
        h ^= (seed >> (i * 8)) & 0xFFu;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

} // namespace gryce_ui

#endif // GRYCE_UI_ID_H
