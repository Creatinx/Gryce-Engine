#pragma once

#include "components/component.h"
#include "components/2d/component_2d.h"
#include "math/math.h"
#include "scene/entity.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace gryce_engine::components::d2 {

// ---------------------------------------------------------------------------
// AnimatedSprite2D — 序列帧动画精灵（图集/逐帧播放）
// ---------------------------------------------------------------------------
class AnimatedSprite2D : public Component2D {
public:
    std::string texture_path;
    bool playing = true;
    bool loop = true;
    float fps = 12.0f;
    int frame_count = 1;
    float frame_width = 0.0f;
    float frame_height = 0.0f;

    // 运行时（不序列化）
    int current_frame = 0;
    float time = 0.0f;

    AnimatedSprite2D() = default;
    const char* type() const override { return "AnimatedSprite2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void on_update(float dt) override {
        if (!playing || frame_count <= 0) return;
        time += dt;
        if (fps > 0.0f) {
            int frame = static_cast<int>(time * fps);
            if (loop) {
                current_frame = frame % frame_count;
            } else {
                current_frame = std::min(frame, frame_count - 1);
                if (current_frame == frame_count - 1) playing = false;
            }
        }
    }

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["texture_path"] = texture_path;
        out["playing"] = playing;
        out["loop"] = loop;
        out["fps"] = fps;
        out["frame_count"] = frame_count;
        out["frame_width"] = frame_width;
        out["frame_height"] = frame_height;
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        texture_path = in.value("texture_path", "");
        playing = in.value("playing", true);
        loop = in.value("loop", true);
        fps = in.value("fps", 12.0f);
        frame_count = in.value("frame_count", 1);
        frame_width = in.value("frame_width", 0.0f);
        frame_height = in.value("frame_height", 0.0f);
    }
};

// ---------------------------------------------------------------------------
// Skeleton2D — 2D 骨骼动画（Spine 风格；数据层）
// ---------------------------------------------------------------------------
class Skeleton2D : public Component2D {
public:
    std::string skeleton_path;
    std::string animation_name;
    bool playing = true;
    float speed = 1.0f;

    Skeleton2D() = default;
    const char* type() const override { return "Skeleton2D"; }
    void draw(render::IRenderer2D* /*renderer*/) override {}

    void serialize(nlohmann::json& out) const override {
        Component2D::serialize_base(out);
        out["skeleton_path"] = skeleton_path;
        out["animation_name"] = animation_name;
        out["playing"] = playing;
        out["speed"] = speed;
    }
    void deserialize(const nlohmann::json& in) override {
        Component2D::deserialize_base(in);
        skeleton_path = in.value("skeleton_path", "");
        animation_name = in.value("animation_name", "");
        playing = in.value("playing", true);
        speed = in.value("speed", 1.0f);
    }
};

// ---------------------------------------------------------------------------
// Path2D — 2D 路径曲线（供敌人巡逻/轨道移动）
// ---------------------------------------------------------------------------
class Path2D : public Component {
public:
    std::vector<math::Vector2f> points;
    bool closed = false;
    bool curve_smooth = true;

    Path2D() = default;
    const char* type() const override { return "Path2D"; }

    void serialize(nlohmann::json& out) const override {
        out["closed"] = closed;
        out["curve_smooth"] = curve_smooth;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : points) arr.push_back({ p.x, p.y });
        out["points"] = std::move(arr);
    }
    void deserialize(const nlohmann::json& in) override {
        closed = in.value("closed", false);
        curve_smooth = in.value("curve_smooth", true);
        points.clear();
        if (in.contains("points") && in["points"].is_array()) {
            for (const auto& item : in["points"]) {
                if (item.is_array() && item.size() >= 2) {
                    points.emplace_back(item[0].get<float>(), item[1].get<float>());
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// PathFollow2D — 路径跟随者（挂在 Path2D 实体下，沿路径移动）
// ---------------------------------------------------------------------------
class PathFollow2D : public Component {
public:
    float progress = 0.0f;   // 0..1
    bool loop = true;
    float speed = 0.1f;      // progress/秒
    bool rotate = false;
    float offset = 0.0f;

    PathFollow2D() = default;
    const char* type() const override { return "PathFollow2D"; }

    void on_update(float dt) override {
        if (!owner_ || !owner_->transform() || !owner_->parent()) return;
        auto* path = owner_->parent()->get_component<Path2D>();
        if (!path || path->points.size() < 2) return;

        progress += speed * dt;
        if (loop) {
            progress = std::fmod(progress + offset, 1.0f);
            if (progress < 0.0f) progress += 1.0f;
        } else {
            progress = std::clamp(progress, 0.0f, 1.0f);
        }

        const auto& pts = path->points;
        const int n = static_cast<int>(pts.size());
        float total = 0.0f;
        for (int i = 0; i + 1 < n; ++i) {
            total += (pts[i + 1] - pts[i]).length();
        }
        if (total <= 1e-6f) return;

        float target = total * progress;
        float walked = 0.0f;
        for (int i = 0; i + 1 < n; ++i) {
            const float seg = (pts[i + 1] - pts[i]).length();
            if (walked + seg >= target || i + 2 == n) {
                const float t = seg > 1e-6f ? (target - walked) / seg : 0.0f;
                const math::Vector2f p = pts[i].lerp(pts[i + 1], std::clamp(t, 0.0f, 1.0f));
                owner_->transform()->position.x = p.x;
                owner_->transform()->position.y = p.y;
                if (rotate) {
                    const math::Vector2f dir = (pts[i + 1] - pts[i]).normalized();
                    owner_->transform()->rotation =
                        math::Quaternionf::from_axis_angle(
                            math::Vector3f(0.0f, 0.0f, 1.0f), std::atan2(dir.y, dir.x));
                }
                return;
            }
            walked += seg;
        }
    }

    void serialize(nlohmann::json& out) const override {
        out["progress"] = progress;
        out["loop"] = loop;
        out["speed"] = speed;
        out["rotate"] = rotate;
        out["offset"] = offset;
    }
    void deserialize(const nlohmann::json& in) override {
        progress = in.value("progress", 0.0f);
        loop = in.value("loop", true);
        speed = in.value("speed", 0.1f);
        rotate = in.value("rotate", false);
        offset = in.value("offset", 0.0f);
    }
};

} // namespace gryce_engine::components::d2
