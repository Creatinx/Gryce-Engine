// ===========================================================================
// engine/ui/ui_component.h — 组件树布局（保留模式，无 ImGui 依赖的纯逻辑）
//
// 解决的问题：ui_layout.h 的布局栈是即时模式——矩形按调用序列一帧一算，
// 表达不了树形结构（父子、嵌套、重排），而"这个面板里各部分的排列规则"
// 本来是与帧无关的事实，应该作为数据存在树上，而不是每帧重演一遍
// imperative 代码。组件树把结构提成数据：组件是节点，布局组件
// （Column / Row / Grid）是安排子节点几何的容器，叶子组件只画自己的内容。
//
// 定位规则只有一条：**父级是不是布局组件**。
//   * 父级是布局组件      -> 子节点的 rect 由布局算出并写回（自动布局）；
//   * 父级不是布局组件    -> 子节点保留调用方写的 pos，即"直接写 pos"
//                            （绝对定位；父级是 Leaf 或无父级都走这条）。
// 一条规则同时覆盖两种习惯用法：整棵界面挂在布局树下自动排列；个别浮窗、
// 叠加层不挂父级（或挂在叶子上），自己写坐标。把某个叶子换成布局组件，
// 它的子节点立刻从"直写"翻转为"自动摆位"——规则是查表，不是 if 散落。
//
// 布局只摆位、不决定容器自身大小：容器的 rect 同样来自它的父布局，或在
// 它不是任何布局组件的子节点时由调用方直接写。容器内容区 = rect 向内缩
// padding。弹性（flex）只分配主轴剩余空间；放不下时剩余钳到 0，不产生
// 负尺寸。
//
// 与 ui_layout.h 的分工：布局栈服务"每帧 imperative 算矩形"（视口分区、
// gizmo 原点）；本组件树服务"结构是数据、跨帧存活"（面板定义）。两者
// 可共存——组件树算出的矩形可以直接喂给 GUi_* 画。
//
// 线程模型：只在 UI 线程使用（回调内建树 / 改树 / 解析 / 读矩形）。
// 不带锁——跨线程传递意图请走操作队列（ui_op_queue.h）。
// ===========================================================================

#ifndef GRYCE_UI_COMPONENT_H
#define GRYCE_UI_COMPONENT_H

#include <cstdint>

#include "ui/ui_layout.h"

namespace gryce_ui {

// 组件种类。布局组件（Column / Row / Grid）安排子节点几何；Leaf 只画自己
// 的内容。"父级是不是布局组件"是定位规则的唯一判据（见文件头）。
enum class CompKind : uint32_t {
    Leaf   = 0,
    Column = 1,   // 垂直栈：子节点沿主轴（竖直）依次排
    Row    = 2,   // 水平栈：子节点沿主轴（水平）依次排
    Grid   = 3,   // 网格：等宽列、行高取行内请求高最大值，子节点填满格子
};

// 交叉轴对齐（Column 的交叉轴是水平，Row 的交叉轴是竖直）。
enum class CompAlign : uint32_t {
    Start   = 0,
    Center  = 1,
    End     = 2,
    Stretch = 3,   // 交叉轴吃满内容区（扣掉交叉 margin）
};

// 一个组件。同一棵树里只有这一个结构——布局组件也是组件，只是 kind 不同。
// 全部字段有默认值：{} 出来的组件是一个无父级、零大小的叶子。
struct Component {
    uint64_t id     = 0;      // 由树填写（add/set 忽略传入值），恒非 0
    uint64_t parent = 0;      // 0 = 无父级；否则必须是已存在的组件 id

    CompKind  kind = CompKind::Leaf;
    Rect      rect{};         // 自身几何（绝对坐标，左上角原点）

    // —— 子侧参数：作为子节点时被父布局读取 ——
    float margin[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // 上 右 下 左
    float flex      = 0.0f;   // 主轴弹性权重；>0 参与剩余空间分配，0 = 用 rect 尺寸
    CompAlign align = CompAlign::Start;

    // —— 容器侧参数：作为布局组件时被读取 ——
    float gap        = 0.0f;   // 子节点间距
    float padding[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // 上 右 下 左
    int   cols       = 1;      // Grid 列数（<1 按 1，大于子节点数按子节点数）
};

class ComponentTree {
public:
    // 容量。装不下说明组件在泄漏（每帧新建不删），不是调大容量的理由。
    static constexpr int kCapacity = 512;

    // 添加组件。parent 为 0 或已存在的 id。返回新 id（恒非 0、单调递增、
    // 不复用——移除后旧 id 永久失效，避免野引用落到新组件上）；失败返回 0
    // （表满，或 parent 不存在）。
    uint64_t add(const Component& c);

    // 整体覆盖一个组件（parent 可改）。新父级是自己或自己的后代时拒绝。
    // 父级未变时只改数据，不改变它在子列表里的位置。返回 1 = 成功。
    bool set(uint64_t id, const Component& c);

    // 移除组件及其整棵子树。返回 1 = 移除了什么，0 = 没有这个 id。
    bool remove(uint64_t id);
    void clear();

    Component*       find(uint64_t id);
    const Component* find(uint64_t id) const;

    // 布局解析（规则见文件头）。幂等：同一棵树解析多次结果相同。
    void resolve();

    // 解析后的矩形；没有这个 id 返回 nullptr。指针在增删组件后仍有效
    // （存储是定长数组），但对应组件被移除后内容即失效。
    const Rect* rect_of(uint64_t id) const;

    int  count() const { return used_; }
    int  child_count(uint64_t id) const;             // -1 = 没有该组件
    uint64_t child_at(uint64_t id, int index) const; // 0 = 越界或没有该组件
    uint64_t parent_of(uint64_t id) const;           // 0 = 无父级或没有该组件

    static bool is_layout(CompKind k) {
        return k == CompKind::Column || k == CompKind::Row || k == CompKind::Grid;
    }

private:
    struct Node {
        Component comp;
        int parent      = -1;   // 内部索引；-1 = 根
        int first_child = -1;
        int last_child  = -1;   // 子节点保持插入顺序（尾插）
        int next_sib    = -1;
        int prev_sib    = -1;
        bool alive      = false;
    };

    int  find_index(uint64_t id) const;
    int  alloc_node();
    void remove_subtree(int idx);
    void unlink(int idx);
    void append_child(int parent_idx, int idx);
    bool is_descendant(int idx, int ancestor) const;

    void resolve_node(int idx);
    void arrange_children(int ci);
    void arrange_stack(int ci, const Rect& content, bool main_is_y);
    void arrange_grid(int ci, const Rect& content);

    Node nodes_[kCapacity];
    int  free_head_ = -1;    // 空闲链表（借用 Node.next_sib 串）
    int  high_water_ = 0;    // 已触达的最高槽位
    int  used_ = 0;
    uint64_t next_id_ = 1;   // id 单调递增，永不复用
};

} // namespace gryce_ui

#endif // GRYCE_UI_COMPONENT_H
