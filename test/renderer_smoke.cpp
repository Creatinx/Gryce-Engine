// renderer_smoke.cpp — Gryce 渲染器双后端冒烟 / 演示程序
//
// 用法:
//   GryceRenderSmoke.exe [--api opengl|vulkan|dx12] [--project <dir>] [--scene <res:/...>]
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
//         按一下 Alt 切换"锁定/解锁"（解锁后指针可见、视角冻结，可去点菜单或
//         别的窗口；再按一下 Alt 恢复锁定），松开 Alt 不会自动恢复。
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
// 左右 Alt（GLFW_KEY_LEFT_ALT / RIGHT_ALT）：按一下切换"锁定/解锁"指针。
constexpr int kKeyLeftAlt = 342, kKeyRightAlt = 346;

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
    // 指针是否处于"锁定（GLFW_CURSOR_DISABLED）"态。每帧按下面条件重算：
    //   窗口有焦点 && 用户没主动解锁 → 锁定；否则放开（指针可见可移动）。
    bool cursor_locked = false;
    // 用户通过 Alt 主动切换出的"解锁"态：true=指针放开、视角冻结。
    // 是"按一下切换"，不是"按住"：松开 Alt 不会自动恢复锁定。
    bool user_unlocked = false;
    // 上一帧的 Alt 状态：只在"按下沿"触发一次切换，长按产生的键盘自动重复
    // 不会连续误触发。
    bool alt_was_held = false;
    // 上一帧的焦点状态：重新获得焦点时把 user_unlocked 复位成锁定。
    bool was_focused = false;

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

        // ---- 指针锁定策略 ----
        //  1) **只在窗口有焦点时锁定**：Alt-Tab 切走还锁着鼠标的话，指针被系统
        //     扣在这个窗口里，别的窗口点不到、也看不到指针。失焦即放开。
        //  2) **按一下 Alt 切换**（是"切换"，不是"按住"）：解锁后指针可见、视角
        //     冻结，可以自由去点菜单或别的窗口；再按一下 Alt 恢复锁定。松开 Alt
        //     不会自动恢复，所以"按住"语义下那套长按/自动重复的坑不存在。
        //  3) 重新获得焦点时回到锁定态，避免"点回游戏却还停在解锁态、鼠标转不了视角"。
        const bool alt_down = window.get_key(kKeyLeftAlt) || window.get_key(kKeyRightAlt);
        if (alt_down != alt_was_held) {
            // 原始按键边沿：只看"键有没有送到窗口、送了几次"，与锁定策略无关。
            // 排查"按一下 Alt 却切了两次/没切"时先看这一行（一次按下只应有一行 DOWN）。
            std::printf("[smoke] alt raw: %s (focused=%d)\n", alt_down ? "DOWN" : "UP",
                        window.has_focus() ? 1 : 0);
            std::fflush(stdout);
        }
        if (alt_down && !alt_was_held) {
            // 只在"按下沿"切换一次：长按产生的键盘自动重复不会连续误触发。
            user_unlocked = !user_unlocked;
            // ASCII 前缀 "alt toggle:" 便于脚本计数（test/tools/cursor_lock_stress.ps1）。
            std::printf("[smoke] alt toggle: %s (focused=%d)\n",
                        user_unlocked ? "UNLOCKED" : "LOCKED",
                        window.has_focus() ? 1 : 0);
            std::fflush(stdout);
        }
        alt_was_held = alt_down;

        const bool focused = window.has_focus();
        if (focused && !was_focused) user_unlocked = false;   // 重新进入窗口 → 默认锁定
        was_focused = focused;

        const bool want_lock = focused && !user_unlocked;
        if (want_lock != cursor_locked) {
            cursor_locked = want_lock;
            window.set_cursor_disabled(want_lock);
            // 状态变化时打一行日志：交互模式出问题时（"鼠标被别的窗口抢走后没回来"
            // 之类）能从日志一眼看出是哪一步没生效。
            // 用 ASCII 标记（LOCKED/UNLOCKED + reason=…）：中文说明在后，
            // 这样脚本也能按 ASCII 前缀校验状态机（test/tools/cursor_lock_probe.ps1）。
            std::printf("[smoke] cursor state: %s (reason=%s) 指针%s（%s）\n",
                        want_lock ? "LOCKED" : "UNLOCKED",
                        !focused ? "focus-lost" : (user_unlocked ? "alt" : "focus"),
                        want_lock ? "已锁定" : "已放开",
                        !focused ? "窗口失焦" : (user_unlocked ? "Alt 解锁" : "窗口获得焦点"));
            std::fflush(stdout);   // 交互模式被强杀时也要留下这行（重定向时是整块缓冲）
            // 刚锁上时把指针挪回中心并对齐基线，避免"放开期间指针在别处"
            // 造成解锁瞬间视角跳一下。
            mouse_ready = false;
        }
        if (!cursor_locked) {
            // 未锁定（失焦或用户解锁）：不读鼠标增量、不回中、不转视角。
            // 只把当前指针位置记成基线，重新锁定时不会跳。
            window.get_cursor_pos(last_mx, last_my);
            mouse_ready = true;
            // 位移仍然照常（解锁也能走位，和大多数游戏一致）。
            apply_movement(window, dt);
            return true;
        }

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
            // 刚锁定的这一帧先把指针拉回中心，再以中心为基线（丢弃这一帧的增量）。
            window.set_cursor_pos(cx, cy);
            apply_movement(window, dt);
            return true;
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

        if (apply_movement(window, dt)) moved = true;

        if (moved) {
            const GQuat q = to_quat();
            GEntity_SetLocalRotation(entity, &q);
        }
        return true;
    }

    // WASD/空格/Ctrl 位移（视角锁定与否都要能用：按住 Alt 只是放开指针，
    // 不该把走位也一起禁掉）。返回是否真的移动了。
    bool apply_movement(Window& window, float dt) {
        const float in_fwd = (window.get_key(kKeyW) ? 1.0f : 0.0f) -
                             (window.get_key(kKeyS) ? 1.0f : 0.0f);
        const float in_side = (window.get_key(kKeyD) ? 1.0f : 0.0f) -
                              (window.get_key(kKeyA) ? 1.0f : 0.0f);
        const float in_up = ((window.get_key(kKeySpace) || window.get_key(kKeyE)) ? 1.0f : 0.0f) -
                            ((window.get_key(kKeyLeftControl) || window.get_key(kKeyQ)) ? 1.0f : 0.0f);
        if (in_fwd == 0.0f && in_side == 0.0f && in_up == 0.0f) return false;

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
        if (len <= 1e-4f) return false;

        const float sprint = window.get_key(kKeyLeftShift) ? 2.5f : 1.0f;
        const float scale = speed * sprint * dt / len;
        position.x += move.x * scale;
        position.y += move.y * scale;
        position.z += move.z * scale;
        GEntity_SetLocalPosition(entity, &position);
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
    float ssr_env_fallback = 1.0f;      // 未命中回退到 IBL/探针的强度（1=替换）
    float ssr_resolution_scale = 1.0f;  // SSR 内部渲染分辨率缩放
    bool pcss_enabled = false;          // 接触硬化软阴影
    float pcss_light_size = 0.06f;
    float pcss_max_radius = 12.0f;
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
        // 配置里的值常被手改成带首尾空白（如 "vulkan "），会与后端名比对失败，这里去掉
        {
            const std::string& v = s.render_api;
            const size_t b = v.find_first_not_of(" \t\r\n");
            const size_t e = v.find_last_not_of(" \t\r\n");
            s.render_api = (b == std::string::npos) ? std::string{} : v.substr(b, e - b + 1);
        }
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
        s.ssr_env_fallback = j.value("ssr_env_fallback", s.ssr_env_fallback);
        s.ssr_resolution_scale = j.value("ssr_resolution_scale", s.ssr_resolution_scale);
        s.pcss_enabled = j.value("pcss_enabled", s.pcss_enabled);
        s.pcss_light_size = j.value("pcss_light_size", s.pcss_light_size);
        s.pcss_max_radius = j.value("pcss_max_radius", s.pcss_max_radius);
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
        "  --api <opengl|vulkan|dx12>  渲染后端（默认取 project.data 的 render_api）\n"
        "  --project <dir>        项目根（默认编译期内置的 demo_project）\n"
        "  --scene <res:/...>     覆盖起始场景\n"
        "  --frames <N>           渲染帧数后截图退出（默认 60）\n"
        "  --shot <png>           截图输出路径（默认 shot_<api>.png）\n"
        "  --w / --h <px>         窗口与视口尺寸（默认取 project.data）\n"
        "  --sync                 同步渲染模式（不启动渲染线程）\n"
        "  --no-vsync             关闭垂直同步（性能测量用，默认打开）\n"
        "  --resolution WxH       设置渲染分辨率（等价 --w W --h H）\n"
        "  --ssr-scale <0.25..1>  SSR 内部渲染分辨率缩放（0.5 约省 3/4 SSR 开销）\n"
        "  --ssr-env-fallback <0..1>  SSR 未命中回退 IBL/探针的强度（默认 1=替换）\n"
        "  --pcss / --no-pcss     接触硬化软阴影开关（默认取 project.data）\n"
        "  --debug-view <0..8>    调试视图：5=阴影因子 6=直接光 7=环境光 8=级联着色\n"
        "  --fps / --no-fps       左上角白色 FPS 叠加（默认开）\n"
        "  --fps-limit <N>        软件锁帧：帧率上限 N（0 = 不限制；与 --vsync 互补）\n"
        "  --no-debug-ui          不画调试 UI（左上角读数 + ImGui 调试窗口）\n"
        "  --bench [N]            开启 GPU 分段计时，跑 N 帧（默认 240）后打印每 pass 平均 GPU 时间\n"
        "  --ssr / --no-ssr       覆盖屏幕空间反射开关（A/B 对照用）\n"
        "  --ssao / --no-ssao     覆盖屏幕空间环境光遮蔽开关\n"
        "  --ssil / --no-ssil     覆盖屏幕空间间接光开关\n"
        "  --no-analyze           跳过截图统计\n"
        "  --no-gpu-cull          关闭 GPU 视锥剔除（A/B 对照用）\n"
        "  --no-occlusion         关闭 HiZ 遮挡剔除（A/B 对照用）\n"
        "  --msaa <N>             MSAA 采样数（前向 HDR pass；默认 4，--no-msaa 关闭）\n"
        "  --power-saver          强制开启省电模式（低分辨率 HDR + 跳过后处理）\n"
        "  --no-power-saver       强制关闭省电模式（A/B 对照用）\n"
        "  --list                 打印场景清单后退出\n"
        "  --simulate             进入 Play 模式驱动组件 on_update（粒子/动画），但不接管相机、不锁鼠标\n"
        "  --play                 交互模式：鼠标锁定转视角，WASD 移动，Esc 退出\n"
        "  --speed <m/s>          交互模式移动速度（默认 3.0）\n"
        "  --sensitivity <度/像素> 交互模式鼠标灵敏度（默认 0.12）\n"
        "  --play-frames <N>      交互模式运行 N 帧后自动退出（自检用）\n"
        "  --stress <N>           生成 N 个动态立方体的重负载场景（性能基准用）。\n"
        "  --instanced <N>        注入 1 个 InstancedMeshRenderer（N 个实例），用于\n"
        "                         验证 GPU 剔除（compute + 间接绘制）的 A/B 对照。\n"
        "                         与 --scene 互斥；生成后替换当前场景内容。\n"
        "  --stress-shadow        压力场景加载 N 个投影体（默认全开阴影投射）\n");
}

// ---------------------------------------------------------------------------
// 压力场景生成器（--stress）
//
// 目的：demo 场景只有 9 实体 / 5 mesh，阴影、合批、剔除、CPU 遍历这些开销
// 全被稀释到噪声里，量不出真实优化空间。这里用 GEntity_ImportJson 批量灌入
// 实体 —— 它走的是与场景文件完全相同的 SceneSerializer::deserialize 路径，
// 因此生成的实体与手写 .gesc 在引擎里没有任何区别。
//
// 布局：一条沿 -Z 延伸的立方体"走廊"，每个立方体都投阴影（这是关键：
// 阴影 pass 的绘制次数 = 投影体数量，是 CPU 侧 O(N) 遍历 + GPU 侧几何提交的
// 双重压力源）。地面用一个大 quad 承接所有阴影，保证接收端像素覆盖满屏。
//
// 注意：必须在 GRender_Init 之后调用（材质/网格资源此时才可解析）。
// ---------------------------------------------------------------------------
struct StressReport {
    int entities = 0;   // 成功灌入的实体数
    int failed = 0;     // 导入失败的批次数
};

StressReport generate_stress_scene(int count) {
    StressReport rep;
    if (count <= 0) return rep;

    // 立方体边长与间距：间距略大于边长，避免互相穿模影响视觉判读，
    // 同时保证有大量重叠投影（阴影 pass 的 overdraw 压力）。
    const float kCubeSize = 1.0f;
    const float kSpacing = 1.6f;
    // 每行多少个：排成方阵，中心对齐原点
    const int side = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(count))));
    const float half = (side - 1) * 0.5f * kSpacing;

    // ---- 地面（承接阴影；单个大 quad，保证接收端满屏覆盖）----
    {
        const float ground_extent = std::max(40.0f, side * kSpacing + 20.0f);
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            R"({"version":2,"name":"StressGround","entities":[{
              "name":"StressGround","parent":null,"enabled":true,
              "transform":{"position":[0,0,0],"rotation":[0,0,0,1],
                           "scale":[%.3f,1.0,%.3f]},
              "components":[{"type":"MeshRenderer","enabled":true,
                "mesh_path":"res:/models/quad.obj","billboard":false,
                "depth_test":true,"material":{"name":"StressGroundMat",
                "preset_name":"Concrete"}}]}]})",
            static_cast<double>(ground_extent), static_cast<double>(ground_extent));
        if (GEntity_ImportJson(buf, 0) == 0) ++rep.failed;
    }

    // ---- N 个立方体，一次一个 JSON（批量拼字符串会在大 N 下爆内存）----
    // 交替使用几种材质预设，制造不同 roughness/metalic，避免被合批成一次
    // instanced draw —— 我们要的是"每个都是独立 draw call"的 CPU 压力。
    static const char* kPresets[] = {"Concrete", "Gold", "Chrome", "Wood"};
    int idx = 0;
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            if (idx >= count) break;
            const float px = static_cast<float>(x) * kSpacing - half;
            const float pz = static_cast<float>(z) * kSpacing - half;
            // 高度错落：让阴影层次可见，也避免完全规整的图案
            const float py = kCubeSize * 0.5f + 0.12f * static_cast<float>(idx % 5);
            // 各自的 Y 轴旋转：避免被实例化合批，同时让投影形状多样
            const float ang = static_cast<float>(idx) * 0.37f;
            const float half_ang = ang * 0.5f;
            const float qy = std::sin(half_ang);
            const float qw = std::cos(half_ang);
            const char* preset = kPresets[idx % 4];

            char buf[768];
            std::snprintf(buf, sizeof(buf),
                R"({"version":2,"name":"StressCube%d","entities":[{
                  "name":"StressCube%d","parent":null,"enabled":true,
                  "transform":{"position":[%.4f,%.4f,%.4f],
                               "rotation":[0,%.6f,0,%.6f],
                               "scale":[%.3f,%.3f,%.3f]},
                  "components":[{"type":"MeshRenderer","enabled":true,
                    "mesh_path":"res:/models/cube.obj","billboard":false,
                    "depth_test":true,"material":{"name":"StressMat%d",
                    "preset_name":"%s"}}]}]})",
                idx, idx,
                static_cast<double>(px), static_cast<double>(py), static_cast<double>(pz),
                static_cast<double>(qy), static_cast<double>(qw),
                static_cast<double>(kCubeSize), static_cast<double>(kCubeSize),
                static_cast<double>(kCubeSize),
                idx, preset);

            if (GEntity_ImportJson(buf, 0) != 0) {
                ++rep.entities;
            } else {
                ++rep.failed;
            }
            ++idx;
        }
        if (idx >= count) break;
    }
    return rep;
}

// ---------------------------------------------------------------------------
// 实例化场景注入器（--instanced N）
//
// 为什么需要它：GPU 剔除（compute 视锥剔除 + HiZ + 间接绘制）只在
// **InstancedMeshRenderer 且实例数 ≥ 32** 时才启用（见
// RenderPipeline::collect_instanced 的 k_min_gpu_cull_instances）。而 demo 场景
// 用 MeshRenderer、--stress 也刻意用 MeshRenderer 打散合批 —— 于是这条路径在
// 现有场景里**一次都跑不到**，"开关能编译、功能是死代码"。
//
// 这里灌一个 InstancedMeshRenderer 实体（走与 .gesc 完全相同的
// SceneSerializer 路径），实例按 seed 确定性散布成一片，用来做
// 「--no-gpu-cull A/B」的正确性对照：两条路径的画面应当**完全一致**。
// ---------------------------------------------------------------------------
bool spawn_instanced_scene(int count) {
    if (count <= 0) return false;
    char buf[768];
    std::snprintf(buf, sizeof(buf),
        R"({"version":2,"name":"InstancedField","entities":[{
          "name":"InstancedField","parent":null,"enabled":true,
          "transform":{"position":[0,1.0,0],"rotation":[0,0,0,1],
                       "scale":[1.0,1.0,1.0]},
          "components":[{"type":"InstancedMeshRenderer","enabled":true,
            "mesh_path":"res:/models/cube.obj","material_path":"",
            "instance_count":%d,"spacing":1.4,"seed":7,
            "material":{"name":"InstancedMat","preset_name":"Gold"}}]}]})",
        count);
    return GEntity_ImportJson(buf, 0) != 0;
}

} // namespace

int main(int argc, char** argv) {
    // 排障用：stdout 默认是块缓冲，进程崩溃（尤其 D3D12 侧访问违规）时
    // printf 的路标会整块丢掉，只剩 GLOG 的行 —— 无法定位崩溃发生在哪个阶段。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

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
    int msaa_override = -1;   // -1 = 默认 4x；0 = 关闭；N = 指定采样数
    bool play_mode = false;
    // --simulate：只把引擎切进 Play 模式（World::update 生效，组件 on_update 跑起来），
    // 但不把相机交给 FpsCamera，也不锁鼠标。用于脚本/CI 验证粒子、动画这类依赖
    // on_update 的效果：不切 Play 模式它们全程静止，截出来的图里根本没有粒子。
    bool simulate_mode = false;
    // --move：每帧平移主相机（不接管输入），用于复现"相机移动"路径下的开销。
    // 反射探针在相机位移时会高频重烘全场景，静止相机的基准测不到这部分。
    bool move_cam = false;
    // --cam-pos / --cam-look：覆盖主相机位置与朝向（不接管输入），用于复现
    // "面向粒子/背向粒子"这类视角相关的开销差异。
    bool cam_override = false;
    GVec3 cam_pos_override{};
    GVec3 cam_look_override{};
    int bench_frames = 0;   // >0：开启 GPU 分段计时，跑满 N 帧后打印每 pass 平均 GPU 时间
    float play_speed = 3.0f;
    float play_sensitivity = 0.12f;
    int play_frames = 0;   // >0：交互模式跑满 N 帧自动退出（脚本/自检用）
    // --stress：生成 N 个动态立方体的重负载场景，用于把 demo 轻场景里
    // 被稀释的阴影/合批/剔除/CPU 遍历开销暴露出来。0 = 不生成。
    int stress_count = 0;
    bool stress_shadow = false;   // 保留：投影体默认全开，此项仅用于日志标注
    int instanced_count = 0;      // >0：注入一个 InstancedMeshRenderer（GPU 剔除验证用）
    bool frames_explicit = false;
    bool vsync = true;   // 默认开垂直同步；--no-vsync 用于性能测量
    float ssr_env_fallback_override = -1.0f;
    float ssr_scale_override = -1.0f;
    int debug_view_override = -1;
    int pcss_override = -1;
    bool fps_overlay = true;   // 左上角白色 FPS，默认开
    int  fps_limit = -1;       // --fps-limit N（<0 表示不设置，保持默认 0 = 不限速）
    bool debug_ui_enabled = true;   // --no-debug-ui 关闭调试 UI
    // -1 = 不干预（用引擎默认）；0/1 = 强制关/开。用于 A/B 对照优化开销。
    int gpu_cull_override = -1;
    int occlusion_override = -1;
    int power_saver_override = -1;   // -1=跟随 auto 判定；1=强制开；0=强制关
    int ssr_debug_override = 0;  // --ssr-debug：SSR 调试视图

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto need_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "[smoke] %s 缺少参数\n", flag);
                std::exit(1);
            }
            return argv[++i];
        };
        auto need_vec3 = [&](const char* flag, GVec3& out) {
            if (i + 3 >= argc) {
                std::fprintf(stderr, "[smoke] %s 需要 3 个数值\n", flag);
                std::exit(1);
            }
            out.x = static_cast<float>(std::atof(argv[++i]));
            out.y = static_cast<float>(std::atof(argv[++i]));
            out.z = static_cast<float>(std::atof(argv[++i]));
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
        else if (std::strcmp(a, "--vsync") == 0) vsync = true;
        else if (std::strcmp(a, "--no-vsync") == 0) vsync = false;
        // SSR：未命中回退强度 / 内部分辨率缩放
        else if (std::strcmp(a, "--ssr-env-fallback") == 0 ||
                 std::strcmp(a, "--ssr-fallback") == 0) {
            ssr_env_fallback_override = static_cast<float>(std::atof(need_value(a)));
        }
        else if (std::strcmp(a, "--ssr-scale") == 0) {
            ssr_scale_override = static_cast<float>(std::atof(need_value(a)));
        }
        else if (std::strcmp(a, "--debug-view") == 0 || std::strcmp(a, "--debug") == 0) {
            debug_view_override = std::atoi(need_value(a));
        }
        else if (std::strcmp(a, "--pcss") == 0) pcss_override = 1;
        else if (std::strcmp(a, "--no-pcss") == 0) pcss_override = 0;
        else if (std::strcmp(a, "--no-gpu-cull") == 0) gpu_cull_override = 0;
        else if (std::strcmp(a, "--no-occlusion") == 0) occlusion_override = 0;
        // 省电模式：forced 语义是"手动覆盖 auto 判定"，因此三态（默认 -1 不干预 /
        // 1 强制开 / 0 强制关）都映射到同一个 API。
        else if (std::strcmp(a, "--power-saver") == 0) power_saver_override = 1;
        else if (std::strcmp(a, "--no-power-saver") == 0) power_saver_override = 0;
        else if (std::strcmp(a, "--fps") == 0) fps_overlay = true;
        else if (std::strcmp(a, "--no-fps") == 0) fps_overlay = false;
        // --fps-limit N：软件锁帧（帧率上限），0 = 不限制。与 --vsync 互补。
        else if (std::strcmp(a, "--fps-limit") == 0) fps_limit = std::atoi(need_value(a));
        // --no-debug-ui：完全不画调试 UI（关掉左上角读数与 ImGui 调试窗口），
        // 用于"纯画面"截图比对（UI 会让两张图必然不同）。
        else if (std::strcmp(a, "--no-debug-ui") == 0) debug_ui_enabled = false;
        else if (std::strcmp(a, "--ssr-debug") == 0) ssr_debug_override = std::atoi(need_value(a));
        else if (std::strcmp(a, "--resolution") == 0) {
            // --resolution 1920x1080 等价于 --w 1920 --h 1080
            const char* v = need_value(a);
            int rw = 0, rh = 0;
            if (std::sscanf(v, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
                width_override = rw;
                height_override = rh;
            } else {
                std::fprintf(stderr, "[smoke] --resolution 需要 WxH 格式（如 1920x1080）\n");
            }
        }
        // 屏幕空间效果覆盖（A/B 对照测试用）
        else if (std::strcmp(a, "--ssr") == 0) ssr_override = 1;
        else if (std::strcmp(a, "--no-ssr") == 0) ssr_override = 0;
        else if (std::strcmp(a, "--ssao") == 0) ssao_override = 1;
        else if (std::strcmp(a, "--no-ssao") == 0) ssao_override = 0;
        else if (std::strcmp(a, "--ssil") == 0) ssil_override = 1;
        else if (std::strcmp(a, "--no-ssil") == 0) ssil_override = 0;
        else if (std::strcmp(a, "--env") == 0) env_override = 1;
        else if (std::strcmp(a, "--no-env") == 0) env_override = 0;
        // MSAA：--msaa N / --no-msaa（A/B 对照用）。0 = 关闭。
        else if (std::strcmp(a, "--msaa") == 0) msaa_override = std::atoi(need_value(a));
        else if (std::strcmp(a, "--no-msaa") == 0) msaa_override = 0;
        else if (std::strcmp(a, "--simulate") == 0) simulate_mode = true;
        else if (std::strcmp(a, "--move") == 0) move_cam = true;
        else if (std::strcmp(a, "--cam-pos") == 0) {
            need_vec3(a, cam_pos_override);
            cam_override = true;
        }
        else if (std::strcmp(a, "--cam-look") == 0) {
            need_vec3(a, cam_look_override);
            cam_override = true;
        }
        else if (std::strcmp(a, "--play") == 0 || std::strcmp(a, "--interactive") == 0 ||
                 std::strcmp(a, "-i") == 0) play_mode = true;
        else if (std::strcmp(a, "--speed") == 0) play_speed = static_cast<float>(std::atof(need_value(a)));
        else if (std::strcmp(a, "--sensitivity") == 0) {
            play_sensitivity = static_cast<float>(std::atof(need_value(a)));
        }
        else if (std::strcmp(a, "--play-frames") == 0) play_frames = std::atoi(need_value(a));
        else if (std::strcmp(a, "--stress") == 0) stress_count = std::atoi(need_value(a));
        else if (std::strcmp(a, "--instanced") == 0) instanced_count = std::atoi(need_value(a));
        else if (std::strcmp(a, "--stress-shadow") == 0) stress_shadow = true;
        else if (std::strcmp(a, "--bench") == 0) {
            // 可选帧数：--bench 200；不跟数字时用默认 240 帧
            if (i + 1 < argc && argv[i + 1][0] != '-') bench_frames = std::atoi(argv[++i]);
            else bench_frames = 240;
        }
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
    else if (api_arg == "dx12") api = GRYCE_RENDER_API_DX12;
    else if (api_arg != "opengl" && !api_arg.empty()) {
        std::fprintf(stderr, "[smoke] 未知后端 '%s'（用 opengl、vulkan 或 dx12）\n", api_arg.c_str());
        return 1;
    }
    const bool use_vulkan = (api == GRYCE_RENDER_API_VULKAN);
    const bool use_dx12   = (api == GRYCE_RENDER_API_DX12);
    const bool use_opengl = (api == GRYCE_RENDER_API_OPENGL);
    const char* backend_name =
        use_vulkan ? "vulkan" : use_dx12 ? "dx12" : "opengl";
    const int width = width_override > 0 ? width_override : cfg.width;
    const int height = height_override > 0 ? height_override : cfg.height;
    if (shot_path.empty()) {
        shot_path = use_vulkan ? "shot_vulkan.png"
                  : use_dx12   ? "shot_dx12.png"
                               : "shot_opengl.png";
    }

    std::printf("[smoke] backend=%s frames=%d viewport=%dx%d sync=%d\n",
                backend_name, frames, width, height, sync_mode ? 1 : 0);

    // 2) 平台 + Core（载入 <project>/project.data 与主场景）
    if (!Window::init_sdk()) {
        std::fprintf(stderr, "[smoke] failed to init GLFW SDK\n");
        return 1;
    }

    const WindowContextType ctx =
        use_opengl ? WindowContextType::OpenGL : WindowContextType::NoApi;
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
    GRender_SetVSync(vsync);
    // 调试 UI 也属于"init 前生效"：引擎在 GRender_Init 里决定要不要接管 ImGui，
    // --no-debug-ui 必须在那之前告知，否则 ImGui 照旧会初始化（只是不画面板）。
    if (!debug_ui_enabled) GRender_SetDebugUI(false);

    GRenderInitDesc desc{};
    desc.version = sizeof(GRenderInitDesc);
    desc.native_window = window.native_handle();
    desc.api = api;
    desc.viewport_w = width;
    desc.viewport_h = height;
    desc.sync_mode = sync_mode;
    // MSAA 必须在 GRender_Init 之前下发：采样数决定 init 时创建的 HDR 目标
    // 形态（多样本颜色附件 + resolve 目标）。--no-msaa → 1（= 关闭）。
    if (msaa_override >= 0) {
        const int msaa = msaa_override > 0 ? msaa_override : 1;
        GRender_SetMSAASamples(msaa);
        std::printf("[smoke] msaa = %d%s\n", msaa, msaa == 1 ? " (off)" : "x");
    }
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
    GRender_SetSSREnvFallback(cfg.ssr_env_fallback);
    GRender_SetSSRResolutionScale(cfg.ssr_resolution_scale);
    if (ssr_env_fallback_override >= 0.0f) {
        cfg.ssr_env_fallback = ssr_env_fallback_override;
        GRender_SetSSREnvFallback(ssr_env_fallback_override);
    }
    if (ssr_scale_override > 0.0f) {
        cfg.ssr_resolution_scale = ssr_scale_override;
        GRender_SetSSRResolutionScale(ssr_scale_override);
    }
    if (debug_view_override >= 0) GRender_SetDebugView(debug_view_override);
    if (pcss_override >= 0) cfg.pcss_enabled = pcss_override != 0;
    GRender_SetPCSSEnabled(cfg.pcss_enabled);
    GRender_SetPCSSParams(cfg.pcss_light_size, cfg.pcss_max_radius, 1.0f);
    // 左上角白色 FPS（默认开；--no-fps 关闭）
    GRender_SetFPSOverlay(fps_overlay);
    if (!debug_ui_enabled) GRender_SetDebugUI(false);
    if (fps_limit >= 0) {
        GRender_SetFrameRateLimit(fps_limit);
        std::printf("[smoke] fps limit = %d%s\n", fps_limit,
                    fps_limit == 0 ? " (unlimited)" : "");
    }
    if (gpu_cull_override >= 0) GRender_SetGPUCullingEnabled(gpu_cull_override != 0);
    if (occlusion_override >= 0) GRender_SetOcclusionCullingEnabled(occlusion_override != 0);
    if (power_saver_override >= 0) GRender_SetPowerSaverForced(power_saver_override != 0);
    if (ssr_debug_override > 0) GRender_SetSSRDebugView(ssr_debug_override);
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

    // ---- 压力场景注入（在统计与渲染之前）----
    // 必须在 GRender_Init 之后：ImportJson 内部按材质名解析资源，
    // 渲染器未初始化时材质/网格拿不到 GPU 句柄。
    if (stress_count > 0) {
        const StressReport rep = generate_stress_scene(stress_count);
        std::printf("[smoke] --stress %d: 注入 %d 个实体（失败 %d）%s\n",
                    stress_count, rep.entities, rep.failed,
                    stress_shadow ? " [shadow-casters]" : "");
    }
    if (instanced_count > 0) {
        const bool ok = spawn_instanced_scene(instanced_count);
        std::printf("[smoke] --instanced %d: %s\n", instanced_count,
                    ok ? "实例化实体已注入（GPU 剔除路径应被启用）" : "注入失败");
    }

    const SceneStats scene = scan_scene();
    std::printf("[smoke] scene: entities=%d cameras=%d lights=%d meshes=%d\n",
                scene.entities, scene.cameras, scene.lights, scene.meshes);
    std::printf("[smoke] renderer initialized (%s)\n", backend_name);

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
            // 光标锁定不在这里做：FpsCamera::update 每帧按"窗口有焦点 && 用户没主动
            // 解锁"重算（失焦自动放开，重新获得焦点自动恢复），单一来源，避免两处状态打架。
            window.focus_window();
            std::printf("[smoke] --play: 鼠标=转视角（仅窗口有焦点时锁定；"
                        "按一下 Alt 切换锁定/解锁）  WASD=移动  "
                        "空格/左Ctrl=升降  Shift=加速  Esc=退出  (speed=%.1f m/s sens=%.2f°/px)\n",
                        static_cast<double>(play_speed),
                        static_cast<double>(play_sensitivity));
        } else {
            std::fprintf(stderr, "[smoke] --play: 场景中没有 Camera 实体，退回普通模式\n");
            play_mode = false;
        }
    }
    if ((move_cam || cam_override) && !play_mode) {
        if (!play_cam.init_from_entity(play_speed, play_sensitivity)) {
            std::fprintf(stderr, "[smoke] --move/--cam-pos: 场景中没有 Camera 实体，忽略\n");
            move_cam = false;
            cam_override = false;
        }
    }
    if (cam_override && play_cam.entity != 0) {
        GVec3 d{cam_look_override.x - cam_pos_override.x,
                cam_look_override.y - cam_pos_override.y,
                cam_look_override.z - cam_pos_override.z};
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len > 1e-6f) { d.x /= len; d.y /= len; d.z /= len; }
        constexpr float k_rad2deg = 57.29577951308232f;
        play_cam.position = cam_pos_override;
        play_cam.pitch_deg = std::asin(std::max(-1.0f, std::min(1.0f, d.y))) * k_rad2deg;
        play_cam.yaw_deg = std::atan2(-d.x, -d.z) * k_rad2deg;
        GEntity_SetLocalPosition(play_cam.entity, &cam_pos_override);
        GQuat q = play_cam.to_quat();
        GEntity_SetLocalRotation(play_cam.entity, &q);
        std::printf("[smoke] 相机已覆盖: pos=(%.2f,%.2f,%.2f) look=(%.2f,%.2f,%.2f)\n",
                    static_cast<double>(cam_pos_override.x), static_cast<double>(cam_pos_override.y),
                    static_cast<double>(cam_pos_override.z), static_cast<double>(cam_look_override.x),
                    static_cast<double>(cam_look_override.y), static_cast<double>(cam_look_override.z));
    }
    // 进入引擎 Play 模式：--play 只负责相机控制，不会自动切换播放态；
    // 不切换的话 World::update 不跑，组件的 on_update（动画/物理/粒子）全程静止。
    if (play_mode || simulate_mode) {
        GCommand play_cmd{};
        play_cmd.type = ECMD_PLAY_MODE;
        if (GCore_PushCommand(&play_cmd) != 0) {
            std::fprintf(stderr, "[smoke] 进入 Play 模式失败\n");
        } else {
            std::printf("[smoke] 已进入 Play 模式（组件 on_update 生效）\n");
        }
    }

    // 交互模式默认一直跑到用户退出；只有显式给了 --frames N 才在第 N 帧截图。
    const bool shot_wanted = play_mode ? (frames_explicit && frames > 0) : true;

    // --bench：开启 GPU 分段计时，跑满指定帧数后打印每 pass 的 GPU 时间。
    if (bench_frames > 0) {
        frames = bench_frames;
        GRender_SetGPUProfiling(1);
        std::printf("[smoke] --bench: GPU 分段计时已开启（%d 帧后打印；"
                    "建议同时加 --no-vsync 让帧循环不被垂直同步拖慢）\n", bench_frames);
    }

    // 5) 帧循环：渲染 -> 请求截图 -> 再驱动若干帧让渲染线程写盘
    int rendered = 0;
    int frames_after_shot = -1;
    bool shot_requested = false;
    auto play_clock = std::chrono::steady_clock::now();
    float move_phase = 0.0f;
    // 逐帧墙钟耗时（含 GPU 等待）：用来暴露周期性卡顿——平均值正常但存在
    // 规律性尖峰时，只看 pass 平均耗时是发现不了的。
    std::vector<float> bench_frame_ms;
    if (bench_frames > 0) bench_frame_ms.reserve(static_cast<size_t>(bench_frames) + 8);
    // 0=world update 1=render begin 2=RenderWorld 3=RenderGizmo 4=EndFrame 5=Core EndFrame
    double bench_phase_ms[6] = {};
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
        if (move_cam && play_cam.entity != 0) {
            // 每帧位移约 0.05m：超过探针 0.02m 的单帧漂移阈值，会稳定触发
            // "相机移动"分支（每 2 帧一次全场景 6 面重烘）。
            move_phase += 0.35f;
            GVec3 p = play_cam.position;
            p.x += 0.15f * std::sin(move_phase);
            GEntity_SetLocalPosition(play_cam.entity, &p);
        }
        const auto t0 = std::chrono::steady_clock::now();
        GCore_BeginFrame(dt);
        const auto t1 = std::chrono::steady_clock::now();
        GRender_BeginFrame();
        const auto t2 = std::chrono::steady_clock::now();
        GRender_RenderWorld();
        const auto t3 = std::chrono::steady_clock::now();
        GRender_RenderGizmo();
        const auto t4 = std::chrono::steady_clock::now();
        GRender_EndFrame();
        const auto t5 = std::chrono::steady_clock::now();
        GCore_EndFrame();
        const auto bench_frame_end = std::chrono::steady_clock::now();
        if (bench_frames > 0) {
            auto ms = [](auto a, auto b) {
                return std::chrono::duration<float, std::milli>(b - a).count();
            };
            bench_phase_ms[0] += ms(t0, t1);   // world update（ECS/粒子/动画 on_update）
            bench_phase_ms[1] += ms(t1, t2);   // render begin
            bench_phase_ms[2] += ms(t2, t3);   // RenderWorld（构建命令）
            bench_phase_ms[3] += ms(t3, t4);   // RenderGizmo
            bench_phase_ms[4] += ms(t4, t5);   // EndFrame（提交 + 等 GPU）
            bench_phase_ms[5] += ms(t5, bench_frame_end);  // Core EndFrame
            bench_frame_ms.push_back(ms(t0, bench_frame_end));
        }
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
    if (bench_frames > 0) {
        // 统计在帧槽复用时结算（有 2 帧延迟），这里已跑满 bench_frames，直接打印。
        GRender_DumpGPUProfiling();
        GRender_SetGPUProfiling(0);
    }
    if (bench_frames > 0 && bench_frame_ms.size() > 40) {
        // 丢弃前 30 帧预热（着色器编译、探针首抓、缓冲首次分配）
        constexpr size_t k_warmup = 30;
        std::vector<float> s(bench_frame_ms.begin() + static_cast<ptrdiff_t>(k_warmup),
                             bench_frame_ms.end());
        std::sort(s.begin(), s.end());
        double sum = 0.0;
        for (float v : s) sum += v;
        const float avg = static_cast<float>(sum / static_cast<double>(s.size()));
        const float p50 = s[s.size() / 2];
        const float p99 = s[static_cast<size_t>(static_cast<double>(s.size() - 1) * 0.99)];
        const float mx = s.back();
        size_t spikes = 0;
        for (float v : s) if (v > p50 * 2.0f) ++spikes;
        std::printf("[cpu] 帧耗时 样本=%zu  平均=%.2fms(%.1f FPS)  中位=%.2fms  "
                    "p99=%.2fms  最大=%.2fms  抖动倍数=%.1fx\n",
                    s.size(), avg, avg > 0.0f ? 1000.0f / avg : 0.0f, p50, p99, mx,
                    p50 > 0.0f ? mx / p50 : 0.0f);
        std::printf("[cpu] 尖峰(>2x中位)=%zu 帧 (%.1f%%)\n", spikes,
                    s.empty() ? 0.0 : 100.0 * static_cast<double>(spikes) / static_cast<double>(s.size()));
        {
            const double n = static_cast<double>(bench_frame_ms.size());
            static const char* k_names[6] = {"world update", "render begin", "RenderWorld",
                                             "RenderGizmo", "EndFrame(提交+等GPU)", "Core EndFrame"};
            std::printf("[cpu] 分相平均:");
            for (int i = 0; i < 6; ++i) {
                std::printf(" %s=%.2fms", k_names[i], bench_phase_ms[i] / n);
            }
            std::printf("\n");
        }
        // 最慢的 5 帧及其序号：用于区分"周期性尖峰"（序号间隔固定）与
        // "收尾一次性开销"（序号集中在末尾，如截图编码/写盘）。
        std::vector<size_t> order(s.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::partial_sort(order.begin(), order.begin() + std::min<size_t>(5, order.size()),
                          order.end(),
                          [&](size_t a, size_t b) { return bench_frame_ms[k_warmup + a] > bench_frame_ms[k_warmup + b]; });
        std::printf("[cpu] 最慢 5 帧(帧序号:耗时ms):");
        for (size_t i = 0; i < std::min<size_t>(5, order.size()); ++i) {
            std::printf(" #%zu:%.1f", k_warmup + order[i], bench_frame_ms[k_warmup + order[i]]);
        }
        std::printf("\n");
        // 每 16 帧为一组打印组内最大耗时，周期性重抓（探针每 16 帧全场景 6 面）
        // 会表现为固定的第 16 帧凸起。
        std::printf("[cpu] 前 64 帧耗时(ms):");
        const size_t show = std::min<size_t>(s.size(), 64);
        for (size_t i = 0; i < show; ++i) {
            std::printf(" %s%.1f", (i % 16 == 15) ? "|" : "", bench_frame_ms[k_warmup + i]);
        }
        std::printf("\n");
    }
    // ★ 退出顺序：**先拆场景（World），再拆渲染器**。
    //
    // MeshRenderer::~MeshRenderer 会调 material->destroy_gpu(ctx_) /
    // ctx_->destroy_mesh()，而 ctx_ 是缓存的裸指针（mesh_renderer.h:68，
    // 只在首次上传时赋值，渲染器关闭时不会被清空）。若先 GRender_Shutdown
    // 再 GCore_Shutdown，这些析构就是对已释放 RenderContext 的 use-after-free。
    //
    // GL/Vulkan 之所以一直没暴露：那块堆内存被释放后内容恰好还像样，
    // running_ 读出来仍是 false，走到"同步销毁"分支糊弄过去了。
    // D3D12 的关闭路径分配更多，堆块被 0xFEEEFEEE 填充，
    // running_ 读成 true → 走 enqueue_destroy → 对垃圾 mutex 加锁 → SIGSEGV。
    // 帧率/帧时间读数：锁帧、VSync 这类"锁帧技术"的效果就靠这两个数验证
    // （--fps-limit 30 时这里应该稳定在 30 左右）。
    // 必须在 GRender_Shutdown() **之前**取 —— 渲染器销毁后统计就清零了。
    const float final_fps = GRender_GetFPS();
    const float final_frame_ms = GRender_GetFrameTimeMs();
    const int   final_limit = GRender_GetFrameRateLimit();

    GCore_Shutdown();
    GRender_Shutdown();
    Window::shutdown_sdk();

    std::printf("[smoke] done, rendered=%d frames, shot=%s\n", rendered, shot_path.c_str());
    std::printf("[smoke] fps=%.1f frame=%.2f ms (limit=%d vsync=%d)\n",
                static_cast<double>(final_fps), static_cast<double>(final_frame_ms),
                final_limit, vsync ? 1 : 0);

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
