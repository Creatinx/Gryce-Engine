// ===========================================================================
// engine/ui/ui_component.cpp — 组件树布局的实现
//
// 存储：定长扁平数组 + 入侵式兄弟链表。id 与槽位解耦——id 单调递增永不复用，
// 槽位走空闲链表复用；移除整棵子树时先收集再统一释放（逐点释放会在已释放
// 节点上写 next_sib，而空闲链表正串在那个字段上，会把链表本身写坏）。
//
// 解析：resolve_node 先安排自己的子节点、再递归进子节点——父布局写回子
// 节点 rect 的动作一定发生在"以该 rect 摆布孙节点"之前。父级不是布局组件
// 时子节点 rect 原样保留，但递归继续（那些子节点自己可能是布局组件）。
// ===========================================================================

#include "ui/ui_component.h"

namespace gryce_ui {
namespace {

// rect 向内缩四边内边距（上 右 下 左）。缩过量时宽 / 高钳到 0。
Rect shrink(const Rect& r, const float pad[4]) {
    Rect out = r;
    out.x += pad[3];
    out.y += pad[0];
    out.w -= pad[3] + pad[1];
    out.h -= pad[0] + pad[2];
    if (out.w < 0.0f) out.w = 0.0f;
    if (out.h < 0.0f) out.h = 0.0f;
    return out;
}

// 主轴 = main_is_y ? 竖直 : 水平；交叉轴是另一个。
float main_lead_margin(const Component& c, bool main_is_y) {
    return main_is_y ? c.margin[0] : c.margin[3];
}

float main_trail_margin(const Component& c, bool main_is_y) {
    return main_is_y ? c.margin[2] : c.margin[1];
}

float main_margin_total(const Component& c, bool main_is_y) {
    return main_lead_margin(c, main_is_y) + main_trail_margin(c, main_is_y);
}

float cross_margin_total(const Component& c, bool main_is_y) {
    return main_is_y ? (c.margin[3] + c.margin[1]) : (c.margin[0] + c.margin[2]);
}

// 子节点在主轴上的请求尺寸（flex == 0 时使用；负值钳到 0）。
float main_request(const Component& c, bool main_is_y) {
    const float v = main_is_y ? c.rect.h : c.rect.w;
    return v > 0.0f ? v : 0.0f;
}

// 交叉轴尺寸：Stretch 吃满交叉区（扣掉交叉 margin），否则用请求尺寸。
float cross_size_for(const Component& c, float cross_avail, bool main_is_y) {
    if (c.align == CompAlign::Stretch) {
        const float v = cross_avail - cross_margin_total(c, main_is_y);
        return v > 0.0f ? v : 0.0f;
    }
    const float v = main_is_y ? c.rect.w : c.rect.h;
    return v > 0.0f ? v : 0.0f;
}

// 交叉轴起点：Start 贴内容区前缘（加引导 margin），Center / End 在扣掉双侧
// margin 的剩余区里对齐；超出（剩余 < 请求）时钳到前缘，不产生负坐标。
float cross_pos_for(const Component& c, const Rect& content, float cross_avail,
                    float cross_sz, bool main_is_y) {
    const float lead = main_is_y ? c.margin[3] : c.margin[0];
    const float trail = main_is_y ? c.margin[1] : c.margin[2];
    const float start = (main_is_y ? content.x : content.y) + lead;
    float avail = cross_avail - lead - trail;
    if (avail < 0.0f) avail = 0.0f;
    const float slack = avail - cross_sz;
    switch (c.align) {
        case CompAlign::Center:  return slack > 0.0f ? start + slack * 0.5f : start;
        case CompAlign::End:     return slack > 0.0f ? start + slack : start;
        case CompAlign::Stretch:
        case CompAlign::Start:
        default:                 return start;
    }
}

void write_rect(Component& c, float main_pos, float cross_pos,
                float main_sz, float cross_sz, bool main_is_y) {
    if (main_is_y) {
        c.rect.y = main_pos; c.rect.h = main_sz;
        c.rect.x = cross_pos; c.rect.w = cross_sz;
    } else {
        c.rect.x = main_pos; c.rect.w = main_sz;
        c.rect.y = cross_pos; c.rect.h = cross_sz;
    }
}

} // namespace

int ComponentTree::find_index(uint64_t id) const {
    if (id == 0) return -1;
    for (int i = 0; i < high_water_; ++i) {
        if (nodes_[i].alive && nodes_[i].comp.id == id) return i;
    }
    return -1;
}

int ComponentTree::alloc_node() {
    int idx = -1;
    if (free_head_ >= 0) {
        idx = free_head_;
        free_head_ = nodes_[idx].next_sib;   // 空闲链表借用 next_sib
    } else if (high_water_ < kCapacity) {
        idx = high_water_++;
    } else {
        return -1;   // 表满
    }
    nodes_[idx] = Node{};
    nodes_[idx].alive = true;
    ++used_;
    return idx;
}

void ComponentTree::append_child(int parent_idx, int idx) {
    Node& p = nodes_[parent_idx];
    Node& c = nodes_[idx];
    c.parent = parent_idx;
    c.prev_sib = p.last_child;
    c.next_sib = -1;
    if (p.last_child >= 0) nodes_[p.last_child].next_sib = idx;
    else p.first_child = idx;
    p.last_child = idx;
}

void ComponentTree::unlink(int idx) {
    Node& n = nodes_[idx];
    if (n.prev_sib >= 0) {
        nodes_[n.prev_sib].next_sib = n.next_sib;
    } else if (n.parent >= 0) {
        nodes_[n.parent].first_child = n.next_sib;
    }
    if (n.next_sib >= 0) {
        nodes_[n.next_sib].prev_sib = n.prev_sib;
    } else if (n.parent >= 0) {
        nodes_[n.parent].last_child = n.prev_sib;
    }
    n.parent = -1;
    n.prev_sib = -1;
    n.next_sib = -1;
}

uint64_t ComponentTree::add(const Component& c) {
    int parent_idx = -1;
    if (c.parent != 0) {
        parent_idx = find_index(c.parent);
        if (parent_idx < 0) return 0;   // 父级不存在
    }
    const int idx = alloc_node();
    if (idx < 0) return 0;              // 表满
    Node& n = nodes_[idx];
    n.comp = c;
    n.comp.id = next_id_++;             // 传入的 id 被忽略（与 OpQueue 的 seq 同约定）
    if (parent_idx >= 0) append_child(parent_idx, idx);
    return n.comp.id;
}

bool ComponentTree::set(uint64_t id, const Component& c) {
    const int idx = find_index(id);
    if (idx < 0) return false;
    int parent_idx = -1;
    if (c.parent != 0) {
        parent_idx = find_index(c.parent);
        if (parent_idx < 0) return false;          // 父级不存在
        if (parent_idx == idx) return false;       // 自挂
        if (is_descendant(parent_idx, idx)) return false;   // 成环
    }
    Component next = c;
    next.id = id;
    if (parent_idx == nodes_[idx].parent) {
        // 父级没变：只改数据，不动子列表位置（顺序变了会 silently 改变布局）
        nodes_[idx].comp = next;
        return true;
    }
    unlink(idx);
    nodes_[idx].comp = next;
    if (parent_idx >= 0) append_child(parent_idx, idx);
    return true;
}

bool ComponentTree::is_descendant(int idx, int ancestor) const {
    // idx 是否是 ancestor 的后代（含自身）
    for (int i = idx; i >= 0; i = nodes_[i].parent) {
        if (i == ancestor) return true;
    }
    return false;
}

bool ComponentTree::remove(uint64_t id) {
    const int idx = find_index(id);
    if (idx < 0) return false;
    remove_subtree(idx);
    return true;
}

void ComponentTree::remove_subtree(int idx) {
    // 先序收集整棵子树，再统一进空闲链表。逐点释放会在已释放节点上写
    // next_sib（空闲链表正串在这个字段上），把链表本身写坏。
    int list[kCapacity];
    int stack[kCapacity];
    int sp = 0;
    int n = 0;
    stack[sp++] = idx;
    while (sp > 0) {
        const int cur = stack[--sp];
        list[n++] = cur;
        for (int c = nodes_[cur].first_child; c >= 0; c = nodes_[c].next_sib) {
            // 树无环，每个节点至多入栈一次，sp 不会超过容量
            stack[sp++] = c;
        }
    }
    unlink(idx);
    for (int i = 0; i < n; ++i) {
        const int k = list[i];
        nodes_[k].alive = false;
        nodes_[k].next_sib = free_head_;
        free_head_ = k;
    }
    used_ -= n;
}

void ComponentTree::clear() {
    for (int i = 0; i < high_water_; ++i) nodes_[i] = Node{};
    free_head_ = -1;
    high_water_ = 0;
    used_ = 0;
    // next_id_ 不复位：clear 之后新组件的 id 不与 clear 之前的冲突
}

Component* ComponentTree::find(uint64_t id) {
    const int i = find_index(id);
    return i >= 0 ? &nodes_[i].comp : nullptr;
}

const Component* ComponentTree::find(uint64_t id) const {
    const int i = find_index(id);
    return i >= 0 ? &nodes_[i].comp : nullptr;
}

const Rect* ComponentTree::rect_of(uint64_t id) const {
    const int i = find_index(id);
    return i >= 0 ? &nodes_[i].comp.rect : nullptr;
}

int ComponentTree::child_count(uint64_t id) const {
    const int i = find_index(id);
    if (i < 0) return -1;
    int n = 0;
    for (int c = nodes_[i].first_child; c >= 0; c = nodes_[c].next_sib) ++n;
    return n;
}

uint64_t ComponentTree::child_at(uint64_t id, int index) const {
    const int i = find_index(id);
    if (i < 0 || index < 0) return 0;
    int n = 0;
    for (int c = nodes_[i].first_child; c >= 0; c = nodes_[c].next_sib) {
        if (n == index) return nodes_[c].comp.id;
        ++n;
    }
    return 0;
}

uint64_t ComponentTree::parent_of(uint64_t id) const {
    const int i = find_index(id);
    return i >= 0 ? nodes_[i].comp.parent : 0;
}

void ComponentTree::resolve() {
    for (int i = 0; i < high_water_; ++i) {
        if (!nodes_[i].alive) continue;
        if (nodes_[i].parent < 0) resolve_node(i);
    }
    // alive 的非根节点必然挂在 alive 的父节点上（remove 摘整棵子树、
    // set 原子重挂），所以从根走一遍即可覆盖全树。
}

void ComponentTree::resolve_node(int idx) {
    Node& n = nodes_[idx];
    // 布局组件先安排子节点（写回子节点 rect），再递归进子节点——父布局的
    // 写回一定发生在"以新 rect 摆布孙节点"之前。
    if (is_layout(n.comp.kind)) arrange_children(idx);
    // 父级不是布局组件时子节点 rect 不动（直接写 pos），但递归继续：
    // 那些子节点自己可能是布局组件，要安排它们自己的孩子。
    for (int c = n.first_child; c >= 0; c = nodes_[c].next_sib) {
        resolve_node(c);
    }
}

void ComponentTree::arrange_children(int ci) {
    const Component& k = nodes_[ci].comp;
    const Rect content = shrink(k.rect, k.padding);
    int n = 0;
    for (int c = nodes_[ci].first_child; c >= 0; c = nodes_[c].next_sib) ++n;
    if (n == 0) return;                            // 没有子节点可摆
    if (content.w <= 0.0f || content.h <= 0.0f) return;   // 容器没有内容区：子节点 rect 保持原值
    switch (k.kind) {
        case CompKind::Column: arrange_stack(ci, content, true);  break;
        case CompKind::Row:    arrange_stack(ci, content, false); break;
        case CompKind::Grid:   arrange_grid(ci, content);         break;
        default: break;
    }
}

void ComponentTree::arrange_stack(int ci, const Rect& content, bool main_is_y) {
    const Component& k = nodes_[ci].comp;
    const float main_avail = main_is_y ? content.h : content.w;
    const float cross_avail = main_is_y ? content.w : content.h;
    const float gap = k.gap > 0.0f ? k.gap : 0.0f;

    // pass 1：固定尺寸总量与弹性权重
    float fixed = 0.0f;
    float flex_total = 0.0f;
    int n = 0;
    for (int c = nodes_[ci].first_child; c >= 0; c = nodes_[c].next_sib) {
        const Component& ch = nodes_[c].comp;
        ++n;
        if (ch.flex > 0.0f) {
            flex_total += ch.flex;
        } else {
            fixed += main_request(ch, main_is_y) + main_margin_total(ch, main_is_y);
        }
    }
    float remaining = main_avail - fixed - gap * (n - 1);
    if (remaining < 0.0f) remaining = 0.0f;   // 放不下：弹性项拿 0，不产生负尺寸
    const float unit = flex_total > 0.0f ? remaining / flex_total : 0.0f;

    // pass 2：沿主轴落位。子节点自己的 pos 被覆盖（这正是"自动布局"的含义），
    // rect 的请求尺寸在覆盖前已读走，不会自我反馈。
    float cursor = main_is_y ? content.y : content.x;
    for (int c = nodes_[ci].first_child; c >= 0; c = nodes_[c].next_sib) {
        Component& ch = nodes_[c].comp;
        cursor += main_lead_margin(ch, main_is_y);
        const float main_sz = ch.flex > 0.0f ? ch.flex * unit : main_request(ch, main_is_y);
        const float cross_sz = cross_size_for(ch, cross_avail, main_is_y);
        const float cross_pos = cross_pos_for(ch, content, cross_avail, cross_sz, main_is_y);
        write_rect(ch, cursor, cross_pos, main_sz, cross_sz, main_is_y);
        cursor += main_sz + main_trail_margin(ch, main_is_y) + gap;
    }
}

void ComponentTree::arrange_grid(int ci, const Rect& content) {
    const Component& k = nodes_[ci].comp;
    int n = 0;
    for (int c = nodes_[ci].first_child; c >= 0; c = nodes_[c].next_sib) ++n;

    int cols = k.cols;
    if (cols < 1) cols = 1;
    if (cols > n) cols = n;               // 列数不超过子节点数（否则出现零宽列）
    const float gap = k.gap > 0.0f ? k.gap : 0.0f;
    const float col_w = (content.w - gap * (cols - 1)) / cols;
    if (col_w <= 0.0f) return;            // 内容区放不下：子节点 rect 保持原值

    // 逐行：行高 = 行内请求高的最大值；子节点填满格子（w = 列宽，h = 行高）。
    // 子节点的 margin / flex / align 对 Grid 无效——格内要精细摆放时，在
    // 格子里再放一个 Row / Column。
    float y = content.y;
    int c = nodes_[ci].first_child;
    while (c >= 0) {
        const int row_first = c;
        int row_n = 0;
        float row_h = 0.0f;
        for (int j = 0; j < cols && c >= 0; ++j) {
            ++row_n;
            const float h = nodes_[c].comp.rect.h;
            if (h > row_h) row_h = h;
            c = nodes_[c].next_sib;
        }
        float x = content.x;
        int cc = row_first;
        for (int j = 0; j < row_n; ++j) {
            nodes_[cc].comp.rect.x = x;
            nodes_[cc].comp.rect.y = y;
            nodes_[cc].comp.rect.w = col_w;
            nodes_[cc].comp.rect.h = row_h;
            x += col_w + gap;
            cc = nodes_[cc].next_sib;
        }
        y += row_h + gap;
    }
}

} // namespace gryce_ui
