#include "render/compositor_effect.h"
#include "render/render_context.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::render {

void CompositorEffectManager::add_effect(std::unique_ptr<CompositorEffect> effect) {
    if (!effect) return;
    const std::string& name = effect->name();
    // 检查是否已存在同名效果
    for (auto& e : effects_) {
        if (e->name() == name) {
            GLOG_WARN("CompositorEffectManager: effect '{}' already exists, replacing", name);
            e = std::move(effect);
            return;
        }
    }
    effects_.push_back(std::move(effect));
}

void CompositorEffectManager::remove_effect(const std::string& name) {
    auto it = std::remove_if(effects_.begin(), effects_.end(),
        [&name](const auto& e) { return e->name() == name; });
    effects_.erase(it, effects_.end());
}

CompositorEffect* CompositorEffectManager::get_effect(size_t index) const {
    return index < effects_.size() ? effects_[index].get() : nullptr;
}

CompositorEffect* CompositorEffectManager::get_effect_by_name(const std::string& name) const {
    for (const auto& e : effects_) {
        if (e->name() == name) return e.get();
    }
    return nullptr;
}

void CompositorEffectManager::execute_callbacks(CompositorEffectCallbackType type,
                                                 RenderContext* ctx,
                                                 RHITextureHandle color_tex,
                                                 RHITextureHandle depth_tex,
                                                 int viewport_w, int viewport_h) {
    for (const auto& effect : effects_) {
        if (!effect->enabled() || effect->callback_type() != type) continue;
        effect->render(ctx, color_tex, depth_tex, viewport_w, viewport_h);
    }
}

} // namespace gryce_engine::render