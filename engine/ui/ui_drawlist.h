// engine/ui/ui_drawlist.h
// UI 自绘命令缓冲（引擎内置 UI 的第一块"手脚"）。
//
// 定位：脱离 Dear ImGui 的引擎内置 UI 渲染路径上，本文件是**模型层**——
// 只记录"画什么"（命令 + 裁剪栈 + 字符串池），不碰 GPU、不依赖渲染器，
// 因此可以无窗口单测。光栅化由后续的渲染桥把命令翻成 IRenderer2D 调用
// （draw_rect / draw_text / draw_sprite_region / set_scissor），三后端
// （D3D12 / Vulkan / GL）自动继承，不需要为 UI 写新 shader。
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
};

// ---------------------------------------------------------------------------
// UiCommand — 单条命令（POD，测试与渲染桥共用）
// clip 字段 = **执行该命令后**生效的裁剪栈深度（PushClip 记压入后的深度，
// PopClip 记弹出后的深度，绘制命令记当前深度；0 = 无裁剪）。渲染桥据此
// 切换 scissor，不在每条命令里存裁剪矩形，省 16 字节/条。
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

    void reset();

    // 裁剪栈
    void push_clip(float x, float y, float w, float h);
    void pop_clip();  // 空栈时无操作（不崩、不计数）

    // 绘制命令（剔除态/容量满时按上面的策略丢弃）
    void add_rect(float x, float y, float w, float h, uint32_t rgba);
    void add_text(float x, float y, const char* text, float font_size, uint32_t rgba);
    void add_sprite(float x, float y, float w, float h, uint64_t texture,
                    float u0, float v0, float u1, float v1, uint32_t rgba);

    // 查询
    int                command_count() const { return count_; }
    const UiCommand&   command(int index) const { return commands_[index]; }
    const char*        text_at(uint32_t off) const;      // 保证 '\0' 结尾
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

    int      count_        = 0;
    int      clip_depth_   = 0;
    int      text_used_    = 0;
    uint32_t dropped_      = 0;
    uint32_t culled_       = 0;
};

} // namespace gryce_ui
