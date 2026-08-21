#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "render/export.h"
#include "render/rhi_handle.h"

namespace gryce_engine::render {

class RenderContext;

// 回调点类型（与 Godot 一致）
enum class CompositorEffectCallbackType : uint8_t {
    POST_OPAQUE = 0,        // 不透明渲染后
    PRE_TRANSPARENT,        // 透明渲染前
    POST_TRANSPARENT,       // 透明渲染后
    POST_SKY,               // 天空盒渲染后
    POST_PROCESSING,        // 后处理前
    MAX
};

// ---------------------------------------------------------------------------
// CompositorEffect — 用户自定义后处理效果
// 参考 Godot 的 CompositorEffect 系统，允许用户插入自定义渲染回调。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API CompositorEffect {
public:
    CompositorEffect() = default;
    virtual ~CompositorEffect() = default;

    // 回调类型
    CompositorEffectCallbackType callback_type() const { return callback_type_; }
    void set_callback_type(CompositorEffectCallbackType type) { callback_type_ = type; }

    // 效果名称
    const std::string& name() const { return name_; }
    void set_name(const std::string& name) { name_ = name; }

    // 是否启用
    bool enabled() const { return enabled_; }
    void set_enabled(bool enabled) { enabled_ = enabled; }

    // 渲染回调（子类实现）
    // ctx: 渲染上下文
    // color_tex/depth_tex: 当前帧缓冲的输入/输出
    virtual void render(RenderContext* ctx,
                        RHITextureHandle color_tex,
                        RHITextureHandle depth_tex,
                        int viewport_w, int viewport_h) = 0;

private:
    CompositorEffectCallbackType callback_type_ = CompositorEffectCallbackType::POST_PROCESSING;
    std::string name_ = "CompositorEffect";
    bool enabled_ = true;
};

// ---------------------------------------------------------------------------
// CompositorEffectManager — 效果管理器
// 管理所有注册的 CompositorEffect，按回调类型分发渲染。
// ---------------------------------------------------------------------------
class GRYCE_RENDERER_API CompositorEffectManager {
public:
    CompositorEffectManager() = default;
    ~CompositorEffectManager() = default;

    // 注册效果
    void add_effect(std::unique_ptr<CompositorEffect> effect);

    // 移除效果
    void remove_effect(const std::string& name);

    // 获取效果列表
    size_t effect_count() const { return effects_.size(); }
    CompositorEffect* get_effect(size_t index) const;
    CompositorEffect* get_effect_by_name(const std::string& name) const;

    // 执行指定回调类型的所有效果
    void execute_callbacks(CompositorEffectCallbackType type,
                           RenderContext* ctx,
                           RHITextureHandle color_tex,
                           RHITextureHandle depth_tex,
                           int viewport_w, int viewport_h);

private:
    std::vector<std::unique_ptr<CompositorEffect>> effects_;
};

} // namespace gryce_engine::render