// renderer_smoke.cpp — Gryce 渲染器双后端冒烟 / 演示程序
//
// 用法:
//   GryceRenderSmoke.exe [--api opengl|vulkan] [--project <dir>] [--scene <res:/...>]
//                        [--frames N] [--shot <png>] [--w N] [--h N]
//                        [--sync] [--no-analyze] [--list]
//                        [--play] [--speed <m/s>] [--sensitivity <deg/px>]
//
// 行为与参考工程的 GryceGame 宿主一致：
//   1. 以 <project> 为项目根初始化 Core（res:/ 由此解析），载入主场景；
//   2. 读取 <project>/project.data 的运行时设置（HDR / 色调映射 / 曝光 / 阴影 /
//      环境光 / IBL），应用到渲染器，保证与参考工程样的画面配方；
//   3. 驱动 N 帧（渲染 3D 世界 + 2D 覆盖层），随后请求截图并多驱动几帧让
//      渲染线程真正写盘；
//   4. 用 stb_image 回读截图做统计，画面若是"纯清屏色"则判定渲染链路未生效。
//
// --play：进入交互模式 —— 鼠标锁定（光标隐藏、锁定在窗口内）用鼠标转视角，
//         WASD 前后左右移动、空格/左Ctrl 升降、Shift 加速、Esc 退出。
//         交互模式下 --frames 表示"第 N 帧截一次图"，不退出。
//
// 退出码: 0 = 正常；1 = 初始化失败；2 = 渲染结果为空（着色器/管线未生效）。
#include "api/render_api.h"
#include "api/core_api.h"
#include "api/scene_api.h"
#include "api/entity_api.h"
#include "api/component_api.h"
#include "platform/window.h"

#include <nlohmann/json.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using gryce_engine::platform::Window;
using gryce_engine::platform::WindowMode;
using gryce_engine::platform::WindowContextType;

namespace {

// ---------------------------------------------------------------------------
// 交互式相机（--play）：鼠标锁定 + 视角 + WASD
// ---------------------------------------------------------------------------
// GLFW 键码是固定值（GLFW_KEY_*），这里直接用常量，避免为了几个按键把 GLFW
// 头文件拉进宿主。
constexpr int kKeySpace = 32;
constexpr int kKeyA = 65, kKeyD = 68, kKeyE = 69, kKeyQ = 81, kKeyS = 83, kKeyW = 87;
constexpr int kKeyEscape = 256;
constexpr int kKeyLeftShift = 340, kKeyLeftControl = 341;

// 相机本地坐标约定：-Z 前、+X 右、+Y 上（与 math::Camera::update_vectors 一致）。
struct FpsCamera {
    GEntityHandle entity = 0;
    GVec3 position{};
    float yaw_deg = 0.0f;    // 绕世界 +Y
    float pitch_deg = 0.0f;  // 绕相机本地 +X
    float speed = 3.0f;      // m/s
    float sensitivity = 0.12f;  // 度/像素
    bool mouse_ready = false;
    double last_mx = 0.0;
    double last_my = 0.0;

    static void normalize(GQuat& q) {
        const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (len > 1e-6f) {
            q.x /= len; q.y /= len; q.z /= len; q.w /= len;
        }
    }
    static GQuat axis_angle(float ax, float ay, float az, float radians) {
        const float h = radians * 0.5f;
        const float s = std::sin(h);
        GQuat q{ax * s, ay * s, az * s, std::cos(h)};
        normalize(q);
        return q;
    }
    static GQuat mul(const GQuat& a, const GQuat& b) {
        GQuat r{
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
        normalize(r);
        return r;
    }
    static void rotate(const GQuat& q, float vx, float vy, float vz, GVec3& out) {
        // v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
        const float cx = q.y * vz - q.z * vy + q.w * vx;
        const float cy = q.z * vx - q.x * vz + q.w * vy;
        const float cz = q.x * vy - q.y * vx + q.w * vz;
        out.x = vx + 2.0f * (q.y * cz - q.z * cy);
        out.y = vy + 2.0f * (q.z * cx - q.x * cz);
        out.z = vz + 2.0f * (q.x * cy - q.y * cx);
    }

    // yaw/pitch → 四元数：先本地俯仰、再世界偏航（标准 FPS 组合）
    GQuat to_quat() const {
        const GQuat qy = axis_angle(0.0f, 1.0f, 0.0f,
                                    static_cast<float>(yaw_deg * 3.14159265358979 / 180.0));
        const GQuat qp = axis_angle(1.0f, 0.0f, 0.0f,
                                    static_cast<float>(pitch_deg * 3.14159265358979 / 180.0));
        return mul(qy, qp);
    }
    // 从实体当前旋转反解 yaw/pitch：forward 满足 (-cosφ sinψ, sinφ, -cosφ cosψ)
    bool init_from_entity(float speed_value, float sensitivity_value) {
        entity = find_camera_entity();
        if (entity == 0) return false;
        speed = speed_value;
        sensitivity = sensitivity_value;

        GQuat q{};
        GVec3 p{};
        if (GEntity_GetLocalRotation(entity, &q) != 0) return false;
        if (GEntity_GetLocalPosition(entity, &p) != 0) return false;
        position = p;

        GVec3 f{};
        rotate(q, 0.0f, 0.0f, -1.0f, f);
        const float fy = std::max(-1.0f, std::min(1.0f, f.y));
        pitch_deg = static_cast<float>(std::asin(fy) * 180.0 / 3.14159265358979);
        yaw_deg = static_cast<float>(std::atan2(-f.x, -f.z) * 180.0 / 3.14159265358979);
        return true;
    }

    static GEntityHandle find_camera_entity() {
        const int count = GEntity_GetCount();
        GEntityHandle fallback = 0;
        for (int i = 0; i < count; ++i) {
            const GEntityHandle e = GEntity_GetAt(i);
            if (e <= 0) continue;
            char name[128] = {};
            GEntity_GetName(e, name, static_cast<int>(sizeof(name)));
            const int comp_count = GComponent_GetCount(e);
            for (int c = 0; c < comp_count; ++c) {
                char type_name[128] = {};
                if (GComponent_GetTypeNameAt(e, c, type_name,
                                             static_cast<int>(sizeof(type_name))) < 0) {
                    continue;
                }
                std::string tn = type_name;
                const auto pos = tn.rfind("::");
                if (pos != std::string::npos) tn = tn.substr(pos + 2);
                if (tn != "Camera") continue;
                if (fallback == 0) fallback = e;
                if (std::strcmp(name, "MainCamera") == 0) return e;
            }
        }
        return fallback;
    }

    // 每帧：鼠标转视角 + WASD 位移。返回 false 表示用户按 Esc 要求退出。
    bool update(Window& window, float dt) {
        if (entity == 0) return true;

        if (window.get_key(kKeyEscape)) return false;

        int w = 0, h = 0;
        window.get_size(w, h);
        const double cx = w > 0 ? w * 0.5 : 0.0;
        const double cy = h > 0 ? h * 0.5 : 0.0;

        double mx = 0.0, my = 0.0;
        window.get_cursor_pos(mx, my);
        if (!mouse_ready) {
            last_mx = cx;
            last_my = cy;
            mouse_ready = true;
        }
        const double dx = mx - last_mx;
        const double dy = my - last_my;
        // 立即回中：锁定模式下光标会被系统不断推走，回中可以避免越界与漂移
        window.set_cursor_pos(cx, cy);
        last_mx = cx;
        last_my = cy;

        bool moved = false;
        if (dx != 0.0 || dy != 0.0) {
            yaw_deg -= static_cast<float>(dx) * sensitivity;    // 右移 → 右转
            pitch_deg -= static_cast<float>(dy) * sensitivity;  // 上移 → 抬头
            pitch_deg = std::max(-89.0f, std::min(89.0f, pitch_deg));
            moved = true;
        }

        const float in_fwd = (window.get_key(kKeyW) ? 1.0f : 0.0f) -
                             (window.get_key(kKeyS) ? 1.0f : 0.0f);
        const float in_side = (window.get_key(kKeyD) ? 1.0f : 0.0f) -
                              (window.get_key(kKeyA) ? 1.0f : 0.0f);
        const float in_up = ((window.get_key(kKeySpace) || window.get_key(kKeyE)) ? 1.0f : 0.0f) -
                            ((window.get_key(kKeyLeftControl) || window.get_key(kKeyQ)) ? 1.0f : 0.0f);

        if (in_fwd != 0.0f || in_side != 0.0f || in_up != 0.0f) {
            const GQuat q = to_quat();
            GVec3 f{};
            rotate(q, 0.0f, 0.0f, -1.0f, f);
            // 水平前向（WASD 只在地面平面内移动，抬头不影响前进方向）
            const float flat = std::sqrt(f.x * f.x + f.z * f.z);
            const float fx = flat > 1e-4f ? f.x / flat : 0.0f;
            const float fz = flat > 1e-4f ? f.z / flat : 0.0f;
            // right = normalize(forward × worldUp) = (-f.z, 0, f.x)
            GVec3 move{};
            move.x = fx * in_fwd - fz * in_side;
            move.y = in_up;
            move.z = fz * in_fwd + fx * in_side;
            const float len = std::sqrt(move.x * move.x + move.y * move.y + move.z * move.z);
            if (len > 1e-4f) {
                const float sprint = window.get_key(kKeyLeftShift) ? 2.5f : 1.0f;
                const float scale = speed * sprint * dt / len;
                position.x += move.x * scale;
                position.y += move.y * scale;
                position.z += move.z * scale;
                GEntity_SetLocalPosition(entity, &position);
            }
            moved = true;
        }

        if (moved) {
            const GQuat q = to_quat();
            GEntity_SetLocalRotation(entity, &q);
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// project.data — 与参考工程一致的运行时设置（缺失时用引擎默认值）
// ---------------------------------------------------------------------------
struct ProjectSettings {
    std::string title = "Gryce Renderer";
    int width = 1280;
    int height = 720;
    std::string render_api = "opengl";
    std::string main_scene = "res:/scenes/main.gesc";
    bool hdr = true;
    int tone_map_mode = 1;
    float exposure = 1.0f;
    bool shadow_enabled = true;
    int shadow_map_size = 2048;
    float ambient[3] = {0.2f, 0.22f, 0.26f};
    float ibl_intensity = 1.0f;
    bool default_environment = false;
    bool ssao_enabled = false;
    bool ssr_enabled = false;
    bool ssil_enabled = false;
    float ssr_max_steps = 64.0f;
    float ssr_max_roughness = 0.6f;
    float ssr_thickness = 0.1f;
    float ssr_bilateral = 0.5f;
    bool loaded = false;
};

ProjectSettings load_project_settings(const std::string& project_root) {
    ProjectSettings s;
    const std::filesystem::path path =
        std::filesystem::path(project_root) / "project.data";
    std::ifstream in(path);
    if (!in) return s;
    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("name") && j["name"].is_string()) s.title = j["name"].get<std::string>();
        if (j.contains("window") && j["window"].is_object()) {
            s.width = j["window"].value("width", s.width);
            s.height = j["window"].value("height", s.height);
            if (j["window"].contains("title") && j["window"]["title"].is_string()) {
                s.title = j["window"]["title"].get<std::string>();
            }
        }
        s.render_api = j.value("render_api", s.render_api);
        s.main_scene = j.value("main_scene", j.value("entry_scene", s.main_scene));
        s.hdr = j.value("hdr", s.hdr);
        s.tone_map_mode = j.value("tone_map_mode", s.tone_map_mode);
        s.exposure = j.value("exposure", s.exposure);
        s.shadow_enabled = j.value("shadow_enabled", s.shadow_enabled);
        s.shadow_map_size = j.value("shadow_map_size", s.shadow_map_size);
        s.ambient[0] = j.value("ambient_r", s.ambient[0]);
        s.ambient[1] = j.value("ambient_g", s.ambient[1]);
        s.ambient[2] = j.value("ambient_b", s.ambient[2]);
        s.ibl_intensity = j.value("ibl_intensity", s.ibl_intensity);
        s.default_environment = j.value("default_environment", s.default_environment);
        s.ssao_enabled = j.value("ssao_enabled", s.ssao_enabled);
        s.ssr_enabled = j.value("ssr_enabled", s.ssr_enabled);
        s.ssil_enabled = j.value("ssil_enabled", s.ssil_enabled);
        s.ssr_max_steps = j.value("ssr_max_steps", s.ssr_max_steps);
        s.ssr_max_roughness = j.value("ssr_max_roughness", s.ssr_max_roughness);
        s.ssr_thickness = j.value("ssr_thickness", s.ssr_thickness);
        s.ssr_bilateral = j.value("ssr_bilateral_filter", s.ssr_bilateral);
        s.loaded = true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[smoke] project.data parse failed: %s\n", e.what());
    }
    return s;
}

const char* short_type_name(const char* full) {
    static thread_local std::string buf;
    std::string n = full ? full : "";
    const auto p = n.rfind("::");
    buf = (p == std::string::npos) ? n : n.substr(p + 2);
    return buf.c_str();
}

// 场景清单：确认主场景真的把相机/灯光/网格载进来了
struct SceneStats {
    int entities = 0;
    int cameras = 0;
    int lights = 0;
    int meshes = 0;
};

SceneStats scan_scene() {
    SceneStats st;
    const int count = GEntity_GetCount();
    if (count <= 0) return st;
    st.entities = count;
    for (int i = 0; i < count; ++i) {
        const GEntityHandle e = GEntity_GetAt(i);
        if (e <= 0) continue;
        const int cc = GComponent_GetCount(e);
        for (int c = 0; c < cc; ++c) {
            char name[128] = {};
            if (GComponent_GetTypeNameAt(e, c, name, (int)sizeof(name)) < 0) continue;
            const std::string t = short_type_name(name);
            if (t == "Camera") ++st.cameras;
            else if (t == "Light") ++st.lights;
            else if (t == "MeshRenderer") ++st.meshes;
        }
    }
    return st;
}

// 截图统计：判定"真的画出东西了"而不是一片清屏色。
struct ImageStats {
    bool ok = false;
    int width = 0;
    int height = 0;
    double mean[3] = {0.0, 0.0, 0.0};
    double non_dominant_ratio = 0.0;   // 与主色调相差较远的像素占比
    int unique_bins = 0;
};

ImageStats analyze_image(const std::string& path) {
    ImageStats st;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 3);
    if (!pixels) return st;
    st.ok = true;
    st.width = w;
    st.height = h;

    const long long total = static_cast<long long>(w) * h;
    long long sum[3] = {0, 0, 0};
    std::unordered_map<unsigned int, long long> histogram;
    histogram.reserve(4096);
    for (long long i = 0; i < total; ++i) {
        const unsigned char* px = pixels + i * 3;
        sum[0] += px[0];
        sum[1] += px[1];
        sum[2] += px[2];
        const unsigned int bin =
            (static_cast<unsigned int>(px[0] >> 3) << 10) |
            (static_cast<unsigned int>(px[1] >> 3) << 5) |
            static_cast<unsigned int>(px[2] >> 3);
        ++histogram[bin];
    }
    stbi_image_free(pixels);

    for (int c = 0; c < 3; ++c) st.mean[c] = static_cast<double>(sum[c]) / total;
    long long dominant = 0;
    for (const auto& [bin, n] : histogram) {
        (void)bin;
        if (n > dominant) dominant = n;
    }
    st.unique_bins = static_cast<int>(histogram.size());
    st.non_dominant_ratio = 1.0 - static_cast<double>(dominant) / total;
    return st;
}

void print_usage() {
    std::printf(
        "GryceRenderSmoke — Gryce 渲染器双后端冒烟/演示程序\n"
        "  --api <opengl|vulkan>  渲染后端（默认取 project.data 的 render_api）\n"
        "  --project <dir>        项目根（默认编译期内置的 demo_project）\n"
        "  --scene <res:/...>     覆盖起始场景\n"
        "  --frames <N>           渲染帧数后截图退出（默认 60）\n"
        "  --shot <png>           截图输出路径（默认 shot_<api>.png）\n"
        "  --w / --h <px>         窗口与视口尺寸（默认取 project.data）\n"
        "  --sync                 同步渲染模式（不启动渲染线程）\n"
        "  --ssr / --no-ssr       覆盖屏幕空间反射开关（A/B 对照用）\n"
        "  --ssao / --no-ssao     覆盖屏幕空间环境光遮蔽开关\n"
        "  --ssil / --no-ssil     覆盖屏幕空间间接光开关\n"
        "  --no-analyze           跳过截图统计\n"
        "  --list                 打印场景清单后退出\n"
        "  --play                 交互模式：鼠标锁定转视角，WASD 移动，Esc 退出\n"
        "  --speed <m/s>          交互模式移动速度（默认 3.0）\n"
        "  --sensitivity <度/像素> 交互模式鼠标灵敏度（默认 0.12）\n"
        "  --play-frames <N>      交互模式运行 N 帧后自动退出（自检用）\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string project_root = GRYCE_DEMO_PROJECT_ROOT;
    std::string scene_override;
    int frames = 60;
    int shot_delay = 4;             // 请求截图后再驱动的帧数（等渲染线程写盘）
    std::string shot_path;
    std::string api_arg;
    int width_override = 0;
    int height_override = 0;
    bool sync_mode = false;
    bool analyze = true;
    bool list_only = false;
    int ssr_override = -1;    // -1 = 跟随 project.data
    int ssao_override = -1;
    int ssil_override = -1;
    int env_override = -1;
    bool play_mode = false;
    float play_speed = 3.0f;
    float play_sensitivity = 0.12f;
    int play_frames = 0;   // >0：交互模式跑满 N 帧自动退出（脚本/自检用）
    bool frames_explicit = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[smoke] %s 缺少参数\n", flag);
                std::exit(1);
            }
            return argv[++i];
        };
        if (std::strcmp(a, "--api") == 0) api_arg = need_value(a);
        else if (std::strcmp(a, "--project") == 0) project_root = need_value(a);
        else if (std::strcmp(a, "--scene") == 0) scene_override = need_value(a);
        else if (std::strcmp(a, "--frames") == 0) {
            frames = std::atoi(need_value(a));
            frames_explicit = true;
        }
        else if (std::strcmp(a, "--shot") == 0) shot_path = need_value(a);
        else if (std::strcmp(a, "--w") == 0) width_override = std::atoi(need_value(a));
        else if (std::strcmp(a, "--h") == 0) height_override = std::atoi(need_value(a));
        else if (std::strcmp(a, "--sync") == 0) sync_mode = true;
        // 屏幕空间效果覆盖（A/B 对照测试用）
        else if (std::strcmp(a, "--ssr") == 0) ssr_override = 1;
        else if (std::strcmp(a, "--no-ssr") == 0) ssr_override = 0;
        else if (std::strcmp(a, "--ssao") == 0) ssao_override = 1;
        else if (std::strcmp(a, "--no-ssao") == 0) ssao_override = 0;
        else if (std::strcmp(a, "--ssil") == 0) ssil_override = 1;
        else if (std::strcmp(a, "--no-ssil") == 0) ssil_override = 0;
        else if (std::strcmp(a, "--env") == 0) env_override = 1;
        else if (std::strcmp(a, "--no-env") == 0) env_override = 0;
        else if (std::strcmp(a, "--play") == 0 || std::strcmp(a, "--interactive") == 0 ||
                 std::strcmp(a, "-i") == 0) play_mode = true;
        else if (std::strcmp(a, "--speed") == 0) play_speed = static_cast<float>(std::atof(need_value(a)));
        else if (std::strcmp(a, "--sensitivity") == 0) {
            play_sensitivity = static_cast<float>(std::atof(need_value(a)));
        }
        else if (std::strcmp(a, "--play-frames") == 0) play_frames = std::atoi(need_value(a));
        else if (std::strcmp(a, "--no-analyze") == 0) analyze = false;
        else if (std::strcmp(a, "--list") == 0) list_only = true;
        else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "[smoke] 未知参数: %s\n", a);
            print_usage();
            return 1;
        }
    }

    // 1) 项目设置
    ProjectSettings cfg = load_project_settings(project_root);
    if (cfg.loaded) {
        std::printf("[smoke] project '%s' (hdr=%d tone_map=%d exposure=%.2f shadow=%d/%d "
                    "ambient=%.2f/%.2f/%.2f ibl=%.2f)\n",
                    project_root.c_str(), cfg.hdr ? 1 : 0, cfg.tone_map_mode,
                    static_cast<double>(cfg.exposure), cfg.shadow_enabled ? 1 : 0,
                    cfg.shadow_map_size, static_cast<double>(cfg.ambient[0]),
                    static_cast<double>(cfg.ambient[1]), static_cast<double>(cfg.ambient[2]),
                    static_cast<double>(cfg.ibl_intensity));
    } else {
        std::printf("[smoke] no project.data at '%s' — using engine defaults\n",
                    project_root.c_str());
    }

    if (api_arg.empty()) api_arg = cfg.render_api;
    GRenderAPI api = GRYCE_RENDER_API_OPENGL;
    if (api_arg == "vulkan") api = GRYCE_RENDER_API_VULKAN;
    else if (api_arg != "opengl" && !api_arg.empty()) {
        std::fprintf(stderr, "[smoke] 未知后端 '%s'（用 opengl 或 vulkan）\n", api_arg.c_str());
        return 1;
    }
    const bool use_vulkan = (api == GRYCE_RENDER_API_VULKAN);
    const int width = width_override > 0 ? width_override : cfg.width;
    const int height = height_override > 0 ? height_override : cfg.height;
    if (shot_path.empty()) {
        shot_path = use_vulkan ? "shot_vulkan.png" : "shot_opengl.png";
    }

    std::printf("[smoke] backend=%s frames=%d viewport=%dx%d sync=%d\n",
                use_vulkan ? "vulkan" : "opengl", frames, width, height, sync_mode ? 1 : 0);

    // 2) 平台 + Core（载入 <project>/project.data 与主场景）
    if (!Window::init_sdk()) {
        std::fprintf(stderr, "[smoke] failed to init GLFW SDK\n");
        return 1;
    }

    const WindowContextType ctx =
        use_vulkan ? WindowContextType::NoApi : WindowContextType::OpenGL;
    Window window(cfg.title.c_str(), width, height, WindowMode::Windowed, ctx);
    if (!window.is_valid()) {
        std::fprintf(stderr, "[smoke] failed to create window\n");
        Window::shutdown_sdk();
        return 1;
    }

    GCoreInitDesc core_desc{};
    core_desc.version = sizeof(GCoreInitDesc);
    core_desc.project_root = project_root.c_str();
    core_desc.enable_reflection = true;
    GCore_SetAutoLoadMainScene(scene_override.empty());
    if (GCore_Init(&core_desc) != 0) {
        std::fprintf(stderr, "[smoke] GCore_Init failed\n");
        Window::shutdown_sdk();
        return 1;
    }
    if (!scene_override.empty() && GScene_Load(scene_override.c_str()) != 0) {
        std::fprintf(stderr, "[smoke] GScene_Load('%s') failed\n", scene_override.c_str());
    }

    // 3) 渲染器（Project Settings 里"重启生效"的项必须在 init 之前应用）
    GRender_SetShadowMapSize(cfg.shadow_map_size);
    GRender_SetVSync(true);

    GRenderInitDesc desc{};
    desc.version = sizeof(GRenderInitDesc);
    desc.native_window = window.native_handle();
    desc.api = api;
    desc.viewport_w = width;
    desc.viewport_h = height;
    desc.sync_mode = sync_mode;
    if (GRender_Init(&desc) != 0) {
        std::fprintf(stderr, "[smoke] GRender_Init failed\n");
        GCore_Shutdown();
        Window::shutdown_sdk();
        return 1;
    }

    GRender_SetHDR(cfg.hdr);
    GRender_SetToneMapMode(cfg.tone_map_mode);
    GRender_SetExposure(cfg.exposure);
    GRender_SetShadowEnabled(cfg.shadow_enabled);
    GRender_SetAmbient(cfg.ambient[0], cfg.ambient[1], cfg.ambient[2]);
    GRender_SetIBLIntensity(cfg.ibl_intensity);
    if (env_override >= 0) cfg.default_environment = env_override != 0;
    GRender_SetDefaultEnvironment(cfg.default_environment);
    std::printf("[smoke] default_environment=%d\n", cfg.default_environment ? 1 : 0);

    // 屏幕空间效果：SSR/SSIL 依赖管线自动跑的 depth+normal 预通道；
    // 命令行覆盖优先于 project.data，方便做 A/B 对照。
    if (ssr_override >= 0) cfg.ssr_enabled = ssr_override != 0;
    if (ssao_override >= 0) cfg.ssao_enabled = ssao_override != 0;
    if (ssil_override >= 0) cfg.ssil_enabled = ssil_override != 0;
    GRender_SetSSRParams(cfg.ssr_max_steps, cfg.ssr_max_roughness, cfg.ssr_thickness,
                         cfg.ssr_bilateral);
    GRender_SetSSAO(cfg.ssao_enabled);
    GRender_SetSSR(cfg.ssr_enabled);
    GRender_SetSSIL(cfg.ssil_enabled);
    std::printf("[smoke] screen-space: ssao=%d ssr=%d ssil=%d "
                "(ssr steps=%.0f max_rough=%.2f thickness=%.3f bilateral=%.2f)\n",
                cfg.ssao_enabled ? 1 : 0, cfg.ssr_enabled ? 1 : 0, cfg.ssil_enabled ? 1 : 0,
                static_cast<double>(cfg.ssr_max_steps),
                static_cast<double>(cfg.ssr_max_roughness),
                static_cast<double>(cfg.ssr_thickness),
                static_cast<double>(cfg.ssr_bilateral));

    const SceneStats scene = scan_scene();
    std::printf("[smoke] scene: entities=%d cameras=%d lights=%d meshes=%d\n",
                scene.entities, scene.cameras, scene.lights, scene.meshes);
    std::printf("[smoke] renderer initialized (%s)\n", use_vulkan ? "Vulkan" : "OpenGL");

    if (list_only) {
        GRender_Shutdown();
        GCore_Shutdown();
        Window::shutdown_sdk();
        return 0;
    }

    // 4) 交互模式：锁定鼠标、找到主相机
    FpsCamera play_cam;
    if (play_mode) {
        if (play_cam.init_from_entity(play_speed, play_sensitivity)) {
            window.focus_window();
            window.set_cursor_disabled(true);
            int w = 0, h = 0;
            window.get_size(w, h);
            if (w > 0 && h > 0) window.set_cursor_pos(w * 0.5, h * 0.5);
            std::printf("[smoke] --play: 鼠标已锁定 | 鼠标=转视角  WASD=移动  "
                        "空格/左Ctrl=升降  Shift=加速  Esc=退出  (speed=%.1f m/s sens=%.2f°/px)\n",
                        static_cast<double>(play_speed),
                        static_cast<double>(play_sensitivity));
        } else {
            std::fprintf(stderr, "[smoke] --play: 场景中没有 Camera 实体，退回普通模式\n");
            play_mode = false;
        }
    }
    // 交互模式默认一直跑到用户退出；只有显式给了 --frames N 才在第 N 帧截图。
    const bool shot_wanted = play_mode ? (frames_explicit && frames > 0) : true;

    // 5) 帧循环：渲染 -> 请求截图 -> 再驱动若干帧让渲染线程写盘
    int rendered = 0;
    int frames_after_shot = -1;
    bool shot_requested = false;
    auto play_clock = std::chrono::steady_clock::now();
    while (!window.should_close()) {
        window.poll_events();

        // 交互模式的 dt 用真实时间（固定 1/60 会让移动速度随帧率漂移）
        float dt = 1.0f / 60.0f;
        if (play_mode) {
            const auto now = std::chrono::steady_clock::now();
            dt = std::chrono::duration<float>(now - play_clock).count();
            play_clock = now;
            dt = std::max(0.0005f, std::min(dt, 0.1f));
            if (!play_cam.update(window, dt)) break;   // Esc
        }
        GCore_BeginFrame(dt);

        GRender_BeginFrame();
        GRender_RenderWorld();
        GRender_RenderGizmo();
        GRender_EndFrame();

        GCore_EndFrame();
        ++rendered;

        if (!shot_requested && shot_wanted && rendered >= frames) {
            // 渲染线程下一帧才会真正执行截图命令，故请求后再驱动几帧。
            GRender_SaveScreenshot(shot_path.c_str());
            shot_requested = true;
            frames_after_shot = shot_delay;
        }
        // 交互模式截图后继续跑（用户自己决定什么时候退出）
        if (!play_mode && frames_after_shot >= 0 && frames_after_shot-- == 0) break;
        if (play_mode && play_frames > 0 && rendered >= play_frames) break;
    }

    if (play_mode) window.set_cursor_disabled(false);
    GRender_Shutdown();
    GCore_Shutdown();
    Window::shutdown_sdk();

    std::printf("[smoke] done, rendered=%d frames, shot=%s\n", rendered, shot_path.c_str());

    if (!shot_requested) return 0;
    if (!std::filesystem::exists(shot_path)) {
        std::fprintf(stderr, "[smoke] 截图未生成（渲染线程未写盘）\n");
        return 2;
    }
    if (!analyze) {
        std::printf("[smoke] screenshot ok: %s\n", shot_path.c_str());
        return 0;
    }

    const ImageStats img = analyze_image(shot_path);
    if (!img.ok) {
        std::fprintf(stderr, "[smoke] 截图无法读取: %s\n", shot_path.c_str());
        return 2;
    }
    std::printf("[smoke] image %dx%d mean=(%.1f,%.1f,%.1f) dominant-color 之外占 %.2f%% "
                "(颜色档位 %d)\n",
                img.width, img.height, img.mean[0], img.mean[1], img.mean[2],
                img.non_dominant_ratio * 100.0, img.unique_bins);

    // 场景里有网格却只画出单一色块（清屏色 + 极少量覆盖），说明 RenderPipeline
    // 没生效。实测：正常渲染覆盖 ≈62%，管线退化到只清屏时 ≈9%。
    if (scene.meshes > 0 && img.non_dominant_ratio < 0.20) {
        std::fprintf(stderr,
                     "[smoke] 场景有 %d 个 MeshRenderer，但画面只有 %.2f%% 非背景像素，"
                     "渲染管线未生效（着色器/网格上传/阴影？）\n",
                     scene.meshes, img.non_dominant_ratio * 100.0);
        return 2;
    }
    std::printf("[smoke] OK: 场景已渲染\n");
    return 0;
}
