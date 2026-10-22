// ===========================================================================
// engine/ui/ui_snapshot.h — 面板视图模型（纯数据、可序列化）
//
// 解决的问题：面板逻辑（有哪些行、什么类型、当前值）如果只活在回调里的
// 一串 GUi_* 调用中，就没法在没有 GPU、没有窗口的环境里验证它——"检视器
// 显示的值与模型一致"这类断言根本无处安放。
//
// 视图模型把面板描述成一张纯数据表：UI 回调只读快照、只产意图（操作
// 队列），模型本身的构建 / 校验 / 序列化全部可单测。JSON 往返同时服务
// 存档与调试（面板状态可以直接打出来看）。
//
// 固定容量、零动态分配：与引擎其余部分一致，面板行数是调用方可知的量。
//
// 64 位 id 在 JSON 里走**十进制字符串**：JS 的 JSON.parse 只有 53 位整数
// 精度，数字形式过一轮就丢低位，两个控件会莫名其妙共状态。
// ===========================================================================

#ifndef GRYCE_UI_SNAPSHOT_H
#define GRYCE_UI_SNAPSHOT_H

#include <cstdint>
#include <cstddef>

#include <nlohmann/json.hpp>

namespace gryce_ui {

enum class RowKind : uint32_t {
    Label     = 0,   // 纯文本
    Button    = 1,   // 按钮（点击产离散操作）
    Checkbox  = 2,   // 布尔（flag）
    Slider    = 3,   // 浮点区间（value / min_v / max_v）
    Drag      = 4,   // 拖拽（vec 前三分量）
    Text      = 5,   // 文本输入（text）
    Separator = 6,   // 分隔线（无参数）
    Custom    = 7,   // 调用方自绘（label / text 自由解释）
};

struct Row {
    RowKind  kind = RowKind::Label;
    uint64_t id   = 0;
    static constexpr size_t kLabelCap = 48;
    static constexpr size_t kTextCap  = 96;
    char label[kLabelCap] = {};
    float value = 0.0f;    // Slider / Checkbox(0/1) / Drag.x
    float min_v = 0.0f;
    float max_v = 1.0f;
    int32_t  flag = 0;     // Checkbox / Button 的按下态
    char text[kTextCap] = {};  // Text / Custom
};

struct Panel {
    static constexpr size_t kMaxRows = 64;
    static constexpr size_t kTitleCap = 48;

    char   title[kTitleCap] = {};
    Row    rows[kMaxRows];
    size_t row_count = 0;

    // 追加一行（拷贝）。满返回 false——容量是设计输入，不是运行时报错。
    bool add(const Row& r);
    void clear() { row_count = 0; title[0] = '\0'; }
};

// JSON 往返。to_json 输出对象；from_json 宽容（缺字段用默认值、坏行跳过），
// 返回 false 仅表示顶层结构不对（不是对象）。
nlohmann::json to_json(const Panel& p);
bool from_json(const nlohmann::json& j, Panel& out);

// kind 的名字（"slider" 这类），未知输入返回 false。
const char* row_kind_name(RowKind k);
bool row_kind_from_name(const char* s, RowKind& out);

} // namespace gryce_ui

#endif // GRYCE_UI_SNAPSHOT_H
