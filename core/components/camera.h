#pragma once

#include "components/component.h"
#include "render/render2d.h"

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Camera — 摄像机组件。
// 投影参数由本组件保存；视口位置/朝向由 owner Entity 的 Transform 决定。
// ---------------------------------------------------------------------------
class Camera : public Component {
public:
    // 垂直 FOV（度）
    float fov = 60.0f;
    // 裁剪平面
    float near_plane = 0.1f;
    float far_plane = 100.0f;

    // 是否为主摄像机；场景中优先级最高的主摄像机将被渲染管线使用。
    bool is_main = true;

    // 背景清除颜色（HDR 管线中作为 clear color 使用）
    render::Color background_color = render::Color(0.15f, 0.15f, 0.18f, 1.0f);

    Camera() = default;

    const char* type() const override { return "Camera"; }

    void serialize(nlohmann::json& out) const override {
        out["fov"] = fov;
        out["near_plane"] = near_plane;
        out["far_plane"] = far_plane;
        out["is_main"] = is_main;
        out["background_color"] = { background_color.r, background_color.g,
                                    background_color.b, background_color.a };
    }

    void deserialize(const nlohmann::json& in) override {
        fov = in.value("fov", 60.0f);
        near_plane = in.value("near_plane", 0.1f);
        far_plane = in.value("far_plane", 100.0f);
        is_main = in.value("is_main", true);
        auto bg = in.value("background_color", std::vector<float>{0.15f, 0.15f, 0.18f, 1.0f});
        if (bg.size() >= 4) {
            background_color = render::Color(bg[0], bg[1], bg[2], bg[3]);
        }
    }
};

} // namespace gryce_engine::components
