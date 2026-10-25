// engine/ui/ui_drawlist.h
// UI 自绘命令缓冲（引擎内置 UI 的第一块"手脚"）。
//
// 定位：脱离 Dear ImGui 的引擎内置 UI 渲染路径上，本文件是**模型层**——
// 只记录"画什么"（命令 + 裁剪栈 + 字符串池），不碰 GPU、不依赖渲染器，
// 因此可以无窗口单测。光栅化由后续的渲染桥把命令翻成 IRenderer2D 调用
// （draw_rect / draw_round_rect / draw_text / draw_sprite_region / set_scissor），
// 三后端（D3D12 / Vulkan / GL）自动继承，不需要为 UI 写新 shader。
//
// 圆角在模型层只是"四个角的半径 + 线宽"，怎么光栅化（折线扇形 + 1px alpha
// 渐隐边）是渲染器的事——这样直角与圆角共用同一套批次与裁剪策略，圆角不会
// 因为引入新的图元类型而丢掉裁剪/剔除语义。
//
// 与 ImDrawList 的差异：ImDrawList 每帧由 ImGui 内部生成并直接进后端；
// 本类是 retained 控件层（阶段 3）与渲染桥（阶段 2）之间的稳定契约，
// 控件只追加命令，裁剪与容量策略在这里统一。

#pragma once

#include <cstdint>

namespace gryce_ui {

// ---------------------------------------------------------------------------
// UiCmd — 命令类型
// ---------------------------------------------------------------------------
enum class UiCmd : uint32_t {
    Rect     = 0,  // 纯色矩形（x/y/w/h + color）
    Text     = 1,  // 文本（x/y + 字符串池引用 + font_size + color）
    Sprite   = 2,  // 贴图（x/y/w/h + texture + uv + tint）
    PushClip = 3,  // 压入裁剪矩形（与父裁剪相交）
    PopClip  = 4,  // 弹出裁剪矩形
    // ---- 美化：圆角族（radii 四角、border_w 线宽、color2 渐变下端色）----
    // 单独成命令而不是在 Rect 上挂可选字段，是为了让渲染桥的分支保持
    // "一种命令一次调用"——直角矩形是绝大多数背景板/分隔线，不该为圆角付代价。
    RoundRect       = 5,  // 圆角填充（color2 != 0 时竖直渐变 color → color2）
    RoundRectBorder = 6,  // 圆角描边（border_w = 线宽，向内生长）
    Shadow          = 7,  // 圆角外阴影（border_w = 扩散半径，color 带 alpha）
    // 凸多边形填充：顶点走多边形池（poly_off / poly_len）。
    // 单独一个命令而不是"用一堆矩形拼"：勾选标记与下拉箭头都是斜线，
    // 拼矩形既画不出斜边也没法抗锯齿。ImGui 画的勾/箭头本身就是一条
    // 8 点凸路径（RenderCheckMark / RenderArrow），这里与它一一对应。
    Polygon = 8,
};

// ---------------------------------------------------------------------------
// UiCommand — 单条命令（POD，测试与渲染桥共用）
// clip 字段 = **执行该命令后**生效的裁剪栈深度（PushClip 记压入后的深度，
// PopClip 记弹出后的深度，绘制命令记当前深度；0 = 无裁剪）。渲染桥据此
// 判断"裁剪状态变没变"，不在每条命令里存裁剪矩形，省 16 字节/条。
//
// 例外是 PushClip：它的 x/y/w/h **不是**调用方请求的矩形，而是与父裁剪
// 相交后的**生效矩形**。命令流因此是自描述的——渲染桥不需要在读命令时
// 回查 drawlist 的裁剪栈（那是构建期的活状态，双缓冲下会被下一帧覆盖，
// 见 ui_render_bridge.h 的线程模型）。
// ---------------------------------------------------------------------------
struct UiCommand {
    UiCmd    cmd = UiCmd::Rect;
    float    x = 0.0f;
    float    y = 0.0f;
    float    w = 0.0f;
    float    h = 0.0f;
    uint32_t color = 0xFFFFFFFF;  // 0xRRGGBBAA，与主题 token 一致
    uint64_t texture = 0;         // Sprite：引擎纹理稳定 id（0 = 无）
    float    u0 = 0.0f;
    float    v0 = 0.0f;
    float    u1 = 1.0f;
    float    v1 = 1.0f;
    float    font_size = 0.0f;    // Text：像素高度
    uint32_t text_off = 0;        // Text：字符串池偏移
    uint32_t text_len = 0;        // Text：字节数（不含 '\0'）
    uint32_t clip = 0;            // 发出时的裁剪栈深度

    // ---- 圆角族（RoundRect / RoundRectBorder / Shadow）----
    // 四角半径按「左上、右上、右下、左下」排列，单位逻辑像素。单值半径由
    // add_round_rect 展开成四角相同；只有标题栏那种"上圆下方"才需要逐角给。
    float    radii[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float    border_w = 0.0f;     // Border：线宽；Shadow：扩散半径
    uint32_t color2 = 0;          // RoundRect：渐变下端色（0 = 纯色填充）

    // ---- Polygon ----
    // 顶点池偏移（单位 = float，成对存 x/y）与顶点数。与字符串池同一套语义：
    // 放不下整条多边形就整条丢弃并计 dropped_，绝不画半个形状。
    uint32_t poly_off = 0;
    uint32_t poly_len = 0;
};

// ---------------------------------------------------------------------------
// UiClipRect — 裁剪栈槽位（渲染桥按 index 1..depth 读取）
// ---------------------------------------------------------------------------
struct UiClipRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// ---------------------------------------------------------------------------
// UiDrawList — 定长命令缓冲 + 裁剪栈 + 字符串池
//
// 语义（均有单测覆盖）：
// - 裁剪相交：push_clip 与当前裁剪取交集；交集为空（w<=0 或 h<=0）时进入
//   剔除态，后续**绘制命令**直接丢弃并计 culled_，但 PushClip/PopClip 照常
//   记录（否则恢复不回来）
// - 容量：命令表满 → 丢弃并计 dropped_；字符串池放不下整条文本 → 整条丢弃
//   并计 dropped_（不截断：半条字符串比没有更难调试）
// - 坐标系：左上原点、+y 向下，与 IRenderer2D 的 top_left_origin 一致
// - 不规范化输入：w/h 为负也原样记录，几何正确性由布局层负责
// ---------------------------------------------------------------------------
class UiDrawList {
public:
    static constexpr int kMaxCommands = 4096;
    static constexpr int kMaxClips     = 64;
    static constexpr int kTextPoolSize = 32768;
    // 多边形池（单位 float）。勾/箭头各 8~10 个点，正常一帧几十个图元；
    // 4096 float = 2048 个点，够 200 个多边形同时在场。
    static constexpr int kPolyPoolSize = 4096;

    void reset();

    // 裁剪栈
    void push_clip(float x, float y, float w, float h);
    void pop_clip();  // 空栈时无操作（不崩、不计数）

    // 绘制命令（剔除态/容量满时按上面的策略丢弃）
    void add_rect(float x, float y, float w, float h, uint32_t rgba);
    // 圆角矩形填充。radius 会被渲染器钳到 min(w,h)/2，负值按 0 处理。
    void add_round_rect(float x, float y, float w, float h, float radius, uint32_t rgba);
    // 逐角半径（左上/右上/右下/左下）。
    void add_round_rect4(float x, float y, float w, float h, const float radii[4],
                         uint32_t rgba);
    // 圆角竖直渐变（top → bottom）：面板/标题栏用得上，比纯色更有层次。
    void add_round_gradient(float x, float y, float w, float h, float radius,
                            uint32_t top, uint32_t bottom);
    // 逐角半径的渐变（标题栏"上圆下方"用它）。
    void add_round_gradient4(float x, float y, float w, float h, const float radii[4],
                             uint32_t top, uint32_t bottom);
    // 圆角描边：线宽向内生长（矩形外轮廓不变，尺寸不会因为加边框变大）。
    void add_round_border(float x, float y, float w, float h, float radius,
                          float thickness, uint32_t rgba);
    // 逐角半径的描边（标题栏"上圆下方"要它：四条边一起画，不留半圈缝）。
    void add_round_border4(float x, float y, float w, float h, const float radii[4],
                           float thickness, uint32_t rgba);
    // 圆角外阴影：spread 是视觉上的扩散半径，rgba 的 alpha 是中心的浓度。
    void add_shadow(float x, float y, float w, float h, float radius, float spread,
                    uint32_t rgba);
    void add_text(float x, float y, const char* text, float font_size, uint32_t rgba);
    void add_sprite(float x, float y, float w, float h, uint64_t texture,
                    float u0, float v0, float u1, float v1, uint32_t rgba);
    // 凸多边形填充。xy 是 2*count 个 float（x0,y0,x1,y1,...），按给定顺序连成
    // 闭合凸多边形——勾选标记与下拉箭头用它。少于 3 点直接忽略。
    void add_polygon(const float* xy, int count, uint32_t rgba);

    // 查询
    int                command_count() const { return count_; }
    const UiCommand&   command(int index) const { return commands_[index]; }
    const char*        text_at(uint32_t off) const;      // 保证 '\0' 结尾
    const float*       points_at(uint32_t off) const;    // Polygon 顶点池
    const UiClipRect&  clip_at(int index) const;         // index 1..clip_depth_
    int                clip_depth() const { return clip_depth_; }
    uint32_t           dropped() const { return dropped_; }  // 容量丢弃计数
    uint32_t           culled() const { return culled_; }    // 裁剪剔除计数

private:
    bool  emit(const UiCommand& c);       // 统一入口：满队返回 false
    bool  cull_active() const;            // 当前裁剪是否为空（剔除态）
    void  intersect(float x, float y, float w, float h,
                    float* ox, float* oy, float* ow, float* oh) const;

    UiCommand  commands_[kMaxCommands];
    UiClipRect clips_[kMaxClips];   // [0] 不用；有效区 1..clip_depth_
    char       text_pool_[kTextPoolSize];
    float      poly_pool_[kPolyPoolSize];   // 成对存 x/y

    int      count_        = 0;
    int      clip_depth_   = 0;
    int      text_used_    = 0;
    int      poly_used_    = 0;
    uint32_t dropped_      = 0;
    uint32_t culled_       = 0;
};

} // namespace gryce_ui
