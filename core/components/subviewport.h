#pragma once

#include <string>

#include "export.h"
#include "components/component.h"
#include "render/rhi_handle.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// SubViewport — 运行时 3D→2D 视口（Godot SubViewport 平替，MVP）
// 每帧把场景从指定相机渲染到离屏纹理，并把结果注入同名 Sprite2D 实体，
// 实现"3D 画面显示在 2D 场景里"（小地图、监控屏、传送门等）。
//
// 当前限制：
// - 视口输出（离屏 FBO + tonemap 到纹理）目前仅 OpenGL 后端可用；
// - 渲染线程运行时无法从主线程创建 GL 资源，因此仅在同步模式（无渲染线程）
//   下生效；多 SubViewport 场景取第一个启用的组件。
// ---------------------------------------------------------------------------
class GRYCE_API SubViewport : public Component {
public:
    int width = 256;
    int height = 256;
    // 使用哪个相机实体（空 = 场景第一个启用的 Camera）
    std::string camera_name;
    // 把渲染结果注入同名 Sprite2D 实体（空 = 不注入，仅经 texture_handle 访问）
    std::string sprite_entity_name;

    // 运行时离屏纹理句柄（每帧由 SubViewportSystem 更新，不序列化）
    render::RHITextureHandle texture_handle;

    SubViewport() = default;

    const char* type() const override { return "SubViewport"; }

    void serialize(nlohmann::json& out) const override {
        out["enabled"] = enabled;
        out["width"] = width;
        out["height"] = height;
        out["camera_name"] = camera_name;
        out["sprite_entity_name"] = sprite_entity_name;
    }

    void deserialize(const nlohmann::json& in) override {
        enabled = in.value("enabled", true);
        width = in.value("width", 256);
        height = in.value("height", 256);
        camera_name = in.value("camera_name", "");
        sprite_entity_name = in.value("sprite_entity_name", "");
    }
};

} // namespace gryce_engine::components
