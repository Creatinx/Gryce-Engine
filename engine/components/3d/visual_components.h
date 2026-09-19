#pragma once

#include "components/component.h"
#include "math/math.h"
#include "render/render2d.h"

#include <string>
#include <vector>

namespace gryce_engine::components {

// ---------------------------------------------------------------------------
// Animator — 动画状态机组件（数据层；AnimatorSystem 后续消费）
// 驱动 SkinnedMeshRenderer 的片段切换、混合与播放参数。
// ---------------------------------------------------------------------------
class Animator : public Component {
public:
    std::string clip_name;
    bool playing = true;
    bool loop = true;
    float speed = 1.0f;
    float time = 0.0f;
    float blend_duration = 0.2f;

    Animator() = default;
    const char* type() const override { return "Animator"; }

    void serialize(nlohmann::json& out) const override {
        out["clip_name"] = clip_name;
        out["playing"] = playing;
        out["loop"] = loop;
        out["speed"] = speed;
        out["time"] = time;
        out["blend_duration"] = blend_duration;
    }
    void deserialize(const nlohmann::json& in) override {
        clip_name = in.value("clip_name", "");
        playing = in.value("playing", true);
        loop = in.value("loop", true);
        speed = in.value("speed", 1.0f);
        time = in.value("time", 0.0f);
        blend_duration = in.value("blend_duration", 0.2f);
    }
};

// ---------------------------------------------------------------------------
// ParticleSystem3D — GPU 3D 粒子发射器（数据层；渲染器消费）
// ---------------------------------------------------------------------------
class ParticleSystem3D : public Component {
public:
    std::string texture_path;
    bool loop = true;
    bool play_on_awake = true;
    int max_particles = 256;
    float emission_rate = 10.0f;
    float lifetime_min = 1.0f;
    float lifetime_max = 2.0f;
    float speed_min = 1.0f;
    float speed_max = 3.0f;
    float start_size = 0.2f;
    float end_size = 0.05f;
    render::Color start_color = render::Color::white();
    render::Color end_color = render::Color(1.0f, 1.0f, 1.0f, 0.0f);
    bool additive = false;
    math::Vector3f emission_offset = math::Vector3f::zero();

    ParticleSystem3D() = default;
    const char* type() const override { return "ParticleSystem3D"; }

    void serialize(nlohmann::json& out) const override {
        out["texture_path"] = texture_path;
        out["loop"] = loop;
        out["play_on_awake"] = play_on_awake;
        out["max_particles"] = max_particles;
        out["emission_rate"] = emission_rate;
        out["lifetime_min"] = lifetime_min;
        out["lifetime_max"] = lifetime_max;
        out["speed_min"] = speed_min;
        out["speed_max"] = speed_max;
        out["start_size"] = start_size;
        out["end_size"] = end_size;
        out["start_color"] = { start_color.r, start_color.g, start_color.b, start_color.a };
        out["end_color"] = { end_color.r, end_color.g, end_color.b, end_color.a };
        out["additive"] = additive;
        out["emission_offset"] = { emission_offset.x, emission_offset.y, emission_offset.z };
    }
    void deserialize(const nlohmann::json& in) override {
        texture_path = in.value("texture_path", "");
        loop = in.value("loop", true);
        play_on_awake = in.value("play_on_awake", true);
        max_particles = in.value("max_particles", 256);
        emission_rate = in.value("emission_rate", 10.0f);
        lifetime_min = in.value("lifetime_min", 1.0f);
        lifetime_max = in.value("lifetime_max", 2.0f);
        speed_min = in.value("speed_min", 1.0f);
        speed_max = in.value("speed_max", 3.0f);
        start_size = in.value("start_size", 0.2f);
        end_size = in.value("end_size", 0.05f);
        auto sc = in.value("start_color", std::vector<float>{1, 1, 1, 1});
        if (sc.size() >= 4) start_color = render::Color(sc[0], sc[1], sc[2], sc[3]);
        auto ec = in.value("end_color", std::vector<float>{1, 1, 1, 0});
        if (ec.size() >= 4) end_color = render::Color(ec[0], ec[1], ec[2], ec[3]);
        additive = in.value("additive", false);
        auto eo = in.value("emission_offset", std::vector<float>{0, 0, 0});
        if (eo.size() >= 3) emission_offset = math::Vector3f(eo[0], eo[1], eo[2]);
    }
};

// ---------------------------------------------------------------------------
// TrailRenderer — 3D 拖尾渲染
// ---------------------------------------------------------------------------
class TrailRenderer : public Component {
public:
    float lifetime = 0.3f;
    float min_vertex_distance = 0.02f;
    float width = 0.1f;
    render::Color color = render::Color::white();
    bool autodestruct = false;

    TrailRenderer() = default;
    const char* type() const override { return "TrailRenderer"; }

    void serialize(nlohmann::json& out) const override {
        out["lifetime"] = lifetime;
        out["min_vertex_distance"] = min_vertex_distance;
        out["width"] = width;
        out["color"] = { color.r, color.g, color.b, color.a };
        out["autodestruct"] = autodestruct;
    }
    void deserialize(const nlohmann::json& in) override {
        lifetime = in.value("lifetime", 0.3f);
        min_vertex_distance = in.value("min_vertex_distance", 0.02f);
        width = in.value("width", 0.1f);
        auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
        autodestruct = in.value("autodestruct", false);
    }
};

// ---------------------------------------------------------------------------
// LineRenderer3D — 3D 线段/折线渲染（技能指示、调试绘制）
// ---------------------------------------------------------------------------
class LineRenderer3D : public Component {
public:
    std::vector<math::Vector3f> points;
    bool loop = false;
    float width = 0.05f;
    render::Color color = render::Color::white();

    LineRenderer3D() = default;
    const char* type() const override { return "LineRenderer3D"; }

    void serialize(nlohmann::json& out) const override {
        out["loop"] = loop;
        out["width"] = width;
        out["color"] = { color.r, color.g, color.b, color.a };
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : points) arr.push_back({ p.x, p.y, p.z });
        out["points"] = std::move(arr);
    }
    void deserialize(const nlohmann::json& in) override {
        loop = in.value("loop", false);
        width = in.value("width", 0.05f);
        auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
        points.clear();
        if (in.contains("points") && in["points"].is_array()) {
            for (const auto& item : in["points"]) {
                if (item.is_array() && item.size() >= 3) {
                    points.emplace_back(item[0].get<float>(), item[1].get<float>(), item[2].get<float>());
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Decal — 贴花投影（弹孔、血迹、地面标识）
// ---------------------------------------------------------------------------
class Decal : public Component {
public:
    std::string texture_path;
    math::Vector3f size = math::Vector3f::one();
    float opacity = 1.0f;
    bool fade_edges = true;

    Decal() = default;
    const char* type() const override { return "Decal"; }

    void serialize(nlohmann::json& out) const override {
        out["texture_path"] = texture_path;
        out["size"] = { size.x, size.y, size.z };
        out["opacity"] = opacity;
        out["fade_edges"] = fade_edges;
    }
    void deserialize(const nlohmann::json& in) override {
        texture_path = in.value("texture_path", "");
        auto s = in.value("size", std::vector<float>{1, 1, 1});
        if (s.size() >= 3) size = math::Vector3f(s[0], s[1], s[2]);
        opacity = in.value("opacity", 1.0f);
        fade_edges = in.value("fade_edges", true);
    }
};

// ---------------------------------------------------------------------------
// Billboard — 广告牌（血条、公告、草；始终面向相机）
// ---------------------------------------------------------------------------
class Billboard : public Component {
public:
    std::string texture_path;
    bool lock_x_axis = true;
    math::Vector2f size = math::Vector2f::one();
    float opacity = 1.0f;
    bool shaded = false;

    Billboard() = default;
    const char* type() const override { return "Billboard"; }

    void serialize(nlohmann::json& out) const override {
        out["texture_path"] = texture_path;
        out["lock_x_axis"] = lock_x_axis;
        out["size"] = { size.x, size.y };
        out["opacity"] = opacity;
        out["shaded"] = shaded;
    }
    void deserialize(const nlohmann::json& in) override {
        texture_path = in.value("texture_path", "");
        lock_x_axis = in.value("lock_x_axis", true);
        auto s = in.value("size", std::vector<float>{1, 1});
        if (s.size() >= 2) size = math::Vector2f(s[0], s[1]);
        opacity = in.value("opacity", 1.0f);
        shaded = in.value("shaded", false);
    }
};

// ---------------------------------------------------------------------------
// TextMesh3D — 3D 世界空间文本
// ---------------------------------------------------------------------------
class TextMesh3D : public Component {
public:
    std::string text;
    std::string font_path;
    float font_size = 1.0f;
    float pixel_height = 0.1f;
    render::Color color = render::Color::white();
    bool double_sided = true;

    TextMesh3D() = default;
    const char* type() const override { return "TextMesh3D"; }

    void serialize(nlohmann::json& out) const override {
        out["text"] = text;
        out["font_path"] = font_path;
        out["font_size"] = font_size;
        out["pixel_height"] = pixel_height;
        out["color"] = { color.r, color.g, color.b, color.a };
        out["double_sided"] = double_sided;
    }
    void deserialize(const nlohmann::json& in) override {
        text = in.value("text", "");
        font_path = in.value("font_path", "");
        font_size = in.value("font_size", 1.0f);
        pixel_height = in.value("pixel_height", 0.1f);
        auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
        double_sided = in.value("double_sided", true);
    }
};

// ---------------------------------------------------------------------------
// Skybox3D — 3D 天空盒组件（接入现有 IBL 环境贴图）
// ---------------------------------------------------------------------------
class Skybox3D : public Component {
public:
    std::string texture_path;
    std::string environment_path;
    float exposure = 1.0f;
    bool visible = true;

    Skybox3D() = default;
    const char* type() const override { return "Skybox3D"; }

    void serialize(nlohmann::json& out) const override {
        out["texture_path"] = texture_path;
        out["environment_path"] = environment_path;
        out["exposure"] = exposure;
        out["visible"] = visible;
    }
    void deserialize(const nlohmann::json& in) override {
        texture_path = in.value("texture_path", "");
        environment_path = in.value("environment_path", "");
        exposure = in.value("exposure", 1.0f);
        visible = in.value("visible", true);
    }
};

// ---------------------------------------------------------------------------
// ReflectionProbe — 反射探针（静态反射/IBL 支持）
// ---------------------------------------------------------------------------
class ReflectionProbe : public Component {
public:
    int resolution = 256;
    math::Vector3f box_extents = math::Vector3f(10.0f, 10.0f, 10.0f);
    float intensity = 1.0f;
    bool realtime = false;

    ReflectionProbe() = default;
    const char* type() const override { return "ReflectionProbe"; }

    void serialize(nlohmann::json& out) const override {
        out["resolution"] = resolution;
        out["box_extents"] = { box_extents.x, box_extents.y, box_extents.z };
        out["intensity"] = intensity;
        out["realtime"] = realtime;
    }
    void deserialize(const nlohmann::json& in) override {
        resolution = in.value("resolution", 256);
        auto b = in.value("box_extents", std::vector<float>{10, 10, 10});
        if (b.size() >= 3) box_extents = math::Vector3f(b[0], b[1], b[2]);
        intensity = in.value("intensity", 1.0f);
        realtime = in.value("realtime", false);
    }
};

// ---------------------------------------------------------------------------
// LightProbeGroup — 光照探针组（动态物体间接光照插值）
// ---------------------------------------------------------------------------
class LightProbeGroup : public Component {
public:
    int grid_x = 2;
    int grid_y = 2;
    int grid_z = 2;
    math::Vector3f size = math::Vector3f(10.0f, 10.0f, 10.0f);
    float intensity = 1.0f;

    LightProbeGroup() = default;
    const char* type() const override { return "LightProbeGroup"; }

    void serialize(nlohmann::json& out) const override {
        out["grid_x"] = grid_x;
        out["grid_y"] = grid_y;
        out["grid_z"] = grid_z;
        out["size"] = { size.x, size.y, size.z };
        out["intensity"] = intensity;
    }
    void deserialize(const nlohmann::json& in) override {
        grid_x = in.value("grid_x", 2);
        grid_y = in.value("grid_y", 2);
        grid_z = in.value("grid_z", 2);
        auto s = in.value("size", std::vector<float>{10, 10, 10});
        if (s.size() >= 3) size = math::Vector3f(s[0], s[1], s[2]);
        intensity = in.value("intensity", 1.0f);
    }
};

// ---------------------------------------------------------------------------
// FogVolume — 体积雾区域
// ---------------------------------------------------------------------------
class FogVolume : public Component {
public:
    render::Color color = render::Color(0.7f, 0.8f, 0.9f, 1.0f);
    float density = 0.05f;
    float height_falloff = 1.0f;
    math::Vector3f size = math::Vector3f(5.0f, 5.0f, 5.0f);
    bool volumetric = true;

    FogVolume() = default;
    const char* type() const override { return "FogVolume"; }

    void serialize(nlohmann::json& out) const override {
        out["color"] = { color.r, color.g, color.b, color.a };
        out["density"] = density;
        out["height_falloff"] = height_falloff;
        out["size"] = { size.x, size.y, size.z };
        out["volumetric"] = volumetric;
    }
    void deserialize(const nlohmann::json& in) override {
        auto c = in.value("color", std::vector<float>{0.7f, 0.8f, 0.9f, 1.0f});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
        density = in.value("density", 0.05f);
        height_falloff = in.value("height_falloff", 1.0f);
        auto s = in.value("size", std::vector<float>{5, 5, 5});
        if (s.size() >= 3) size = math::Vector3f(s[0], s[1], s[2]);
        volumetric = in.value("volumetric", true);
    }
};

// ---------------------------------------------------------------------------
// VolumetricLight — 体积光柱 / God Rays
// ---------------------------------------------------------------------------
class VolumetricLight : public Component {
public:
    float intensity = 1.0f;
    float range = 10.0f;
    render::Color color = render::Color::white();
    int steps = 16;
    float jitter = 0.1f;

    VolumetricLight() = default;
    const char* type() const override { return "VolumetricLight"; }

    void serialize(nlohmann::json& out) const override {
        out["intensity"] = intensity;
        out["range"] = range;
        out["color"] = { color.r, color.g, color.b, color.a };
        out["steps"] = steps;
        out["jitter"] = jitter;
    }
    void deserialize(const nlohmann::json& in) override {
        intensity = in.value("intensity", 1.0f);
        range = in.value("range", 10.0f);
        auto c = in.value("color", std::vector<float>{1, 1, 1, 1});
        if (c.size() >= 4) color = render::Color(c[0], c[1], c[2], c[3]);
        steps = in.value("steps", 16);
        jitter = in.value("jitter", 0.1f);
    }
};

// ---------------------------------------------------------------------------
// LODGroup — 多级细节切换（远近距离替换模型）
// ---------------------------------------------------------------------------
class LODGroup : public Component {
public:
    std::vector<std::string> mesh_paths;
    std::vector<float> screen_size_thresholds;
    float transition_duration = 0.1f;
    int active_lod = 0;

    LODGroup() = default;
    const char* type() const override { return "LODGroup"; }

    void serialize(nlohmann::json& out) const override {
        out["mesh_paths"] = mesh_paths;
        out["screen_size_thresholds"] = screen_size_thresholds;
        out["transition_duration"] = transition_duration;
        out["active_lod"] = active_lod;
    }
    void deserialize(const nlohmann::json& in) override {
        if (in.contains("mesh_paths") && in["mesh_paths"].is_array()) {
            mesh_paths = in["mesh_paths"].get<std::vector<std::string>>();
        }
        if (in.contains("screen_size_thresholds") && in["screen_size_thresholds"].is_array()) {
            screen_size_thresholds = in["screen_size_thresholds"].get<std::vector<float>>();
        }
        transition_duration = in.value("transition_duration", 0.1f);
        active_lod = in.value("active_lod", 0);
    }
};

// ---------------------------------------------------------------------------
// InstancedMeshRenderer — GPU 实例化渲染（植被/碎石/人群）
// ---------------------------------------------------------------------------
class InstancedMeshRenderer : public Component {
public:
    std::string mesh_path;
    std::string material_path;
    int instance_count = 1;
    float spacing = 1.0f;
    int seed = 0;

    InstancedMeshRenderer() = default;
    const char* type() const override { return "InstancedMeshRenderer"; }

    void serialize(nlohmann::json& out) const override {
        out["mesh_path"] = mesh_path;
        out["material_path"] = material_path;
        out["instance_count"] = instance_count;
        out["spacing"] = spacing;
        out["seed"] = seed;
    }
    void deserialize(const nlohmann::json& in) override {
        mesh_path = in.value("mesh_path", "");
        material_path = in.value("material_path", "");
        instance_count = in.value("instance_count", 1);
        spacing = in.value("spacing", 1.0f);
        seed = in.value("seed", 0);
    }
};

} // namespace gryce_engine::components
