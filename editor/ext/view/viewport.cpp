// ===========================================================================
// editor/ext/view/viewport.cpp — 视口状态实现
// ===========================================================================

#include "view/viewport.h"

#include "api/edit_api.h"
#include "api/render_api.h"
#include "api/viewport_api.h"

#include "common/json_writer.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace gryce::editor::view {

namespace {

// 显示模式只有设置没有查询，ext 自己记一份。
// 初始值与引擎默认一致（Shaded），否则面板上会显示空白。
char g_display_mode[32] = "Shaded";

// 请求的视口尺寸。renderer 实际尺寸可能因窗口限制不同，每帧对一次账。
int g_requested_w = 0;
int g_requested_h = 0;

EditorCamera g_camera;

// 不引 strings.h / _stricmp：这两个在 MinGW 与 MSVC 上名字不同，
// 而这里只需要比较两个短模式名。
bool equals_ci(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

struct Vec3 {
    float x, y, z;
};

Vec3 sub(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 add(Vec3 a, Vec3 b) { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 mul(Vec3 a, float s) { return Vec3{a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 normalize(Vec3 v) {
    const float len = std::sqrt(dot(v, v));
    return len > 1e-6f ? Vec3{v.x / len, v.y / len, v.z / len} : Vec3{0.0f, 0.0f, 0.0f};
}

// v' = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v)
// 与 main.js 的 rotate() 是同一个公式：脚本侧算相机基向量、ext 侧算拾取射线，
// 两边必须给出同一个方向。
Vec3 rotate_by_quat(const float q[4], Vec3 v) {
    const float cx = q[1] * v.z - q[2] * v.y + q[3] * v.x;
    const float cy = q[2] * v.x - q[0] * v.z + q[3] * v.y;
    const float cz = q[0] * v.y - q[1] * v.x + q[3] * v.z;
    return Vec3{
        v.x + 2.0f * (q[1] * cz - q[2] * cy),
        v.y + 2.0f * (q[2] * cx - q[0] * cz),
        v.z + 2.0f * (q[0] * cy - q[1] * cx),
    };
}

constexpr float kDeg2Rad = 3.14159265358979323846f / 180.0f;

} // namespace

void reset() {
    std::strncpy(g_display_mode, "Shaded", sizeof(g_display_mode) - 1);
    g_display_mode[sizeof(g_display_mode) - 1] = '\0';
    g_requested_w = 0;
    g_requested_h = 0;
    g_camera = EditorCamera{};
}

int enable_output(int enabled) {
    const bool want = enabled != 0;
    if (GRender_IsViewportOutputEnabled() == want) return 1;
    // 运行期切换会触发一次管线热重载（tonemap 的 render pass 与目标 FBO 绑定），
    // 有一两帧卡顿。编辑器在 on_start 里一次性打开，之后不该再动。
    GRender_SetViewportOutput(want);
    return 1;
}

int set_size(int w, int h) {
    // 上界 8192 是多数桌面 GPU 的纹理上限下沿；超过必然创建失败，
    // 提前拒绝比让管线在 resize 里静默失败好查。
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;
    if (w == g_requested_w && h == g_requested_h) return 1;

    g_requested_w = w;
    g_requested_h = h;

    // 走编辑命令面：视口尺寸变更会重建 HDR 链，必须在渲染线程的节奏里做，
    // 不能在 JS 的 on_update 里直接改。
    GEdit_SetRenderTargetSize(0, w, h);
    GEdit_SetRenderTargetSize(1, w, h);

    // GameView 与 SceneView 共用一个内部分辨率（管线明确要求一致，不一致会
    // 拒绝并记日志），所以两个目标一起设，避免只改一个导致半边拉伸。
    GViewport_SetSize(w, h);
    GGameView_SetSize(w, h);
    return 1;
}

int set_screen_origin(float x, float y) {
    // Gizmo 画在 ImGui 的绝对坐标系里，必须知道视口贴在窗口的哪个位置，
    // 否则会画在"窗口左上角"（旧行为：视口铺满窗口）而点不中也画不对。
    if (!std::isfinite(x) || !std::isfinite(y)) return 0;
    GViewport_SetScreenOrigin(x, y);
    return 1;
}

void set_editor_camera(float x, float y, float z, float yaw_deg, float pitch_deg,
                       float fov_deg) {
    // NaN / Inf 会让相机的视图矩阵整个失效，画面直接空白，而日志里什么都看不到。
    // 视口相机每帧都从脚本传进来，这里必须自己守一道。
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(yaw_deg) || !std::isfinite(pitch_deg) || !std::isfinite(fov_deg)) {
        return;
    }
    // fov 夹在 (1, 179)：0 度会让投影矩阵退化，179 以上同样。
    const float fov = fov_deg < 1.0f ? 1.0f : (fov_deg > 179.0f ? 179.0f : fov_deg);
    const float pitch = pitch_deg < -89.0f ? -89.0f : (pitch_deg > 89.0f ? 89.0f : pitch_deg);

    // 朝向只经四元数传给引擎：先绕 Y 转 yaw、再绕局部 X 转 pitch。
    // 这条路径与 main.js 的 view_quat 是同一个乘法顺序，也与 screen_ray 用的
    // 是同一份数据 —— 之前用 yaw/pitch 直传时，引擎与 ext 各有一套正负号，
    // 表现为"画面是水平的、射线却在低头"，点哪都选不中。
    const float hy = yaw_deg * 0.5f * kDeg2Rad;
    const float hp = pitch * 0.5f * kDeg2Rad;
    const float cy = std::cos(hy), sy = std::sin(hy);
    const float cp = std::cos(hp), sp = std::sin(hp);

    // q = qy ⊗ qp，其中 qy = (0, sy, 0, cy)、qp = (sp, 0, 0, cp)。
    float q[4];
    q[0] = cy * sp;    // x
    q[1] = sy * cp;    // y
    q[2] = -sy * sp;   // z
    q[3] = cy * cp;    // w
    // 归一化：两个单位四元数相乘本应仍是单位四元数，浮点误差会累积。
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 1e-6f) {
        for (int i = 0; i < 4; ++i) q[i] /= len;
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }

    GViewport_SetEditorTransform(x, y, z, q[0], q[1], q[2], q[3], fov, 0.05f, 5000.0f);

    // 存一份给 screen_ray 用：引擎只写不读，而拾取必须用视口相机的朝向，
    // 不能用场景里被编辑的 Camera 实体（那是内容，朝向由用户随意摆）。
    g_camera.pos[0] = x;
    g_camera.pos[1] = y;
    g_camera.pos[2] = z;
    g_camera.quat[0] = q[0];
    g_camera.quat[1] = q[1];
    g_camera.quat[2] = q[2];
    g_camera.quat[3] = q[3];
    g_camera.yaw = yaw_deg;
    g_camera.pitch = pitch;
    g_camera.fov = fov;
    g_camera.valid = true;
}

const EditorCamera& editor_camera() { return g_camera; }

int screen_ray(float sx, float sy, GVec3* out_origin, GVec3* out_dir) {
    if (!g_camera.valid || !out_origin || !out_dir) return 0;
    if (g_requested_w <= 0 || g_requested_h <= 0) return 0;

    // 用相机四元数把默认朝向 (-Z) 转到世界空间 —— 与引擎渲染用的是同一个
    // 四元数，所以"看到的方向"与"算出的射线"天然一致，不需要任何约定换算。
    const Vec3 z_axis{0.0f, 0.0f, -1.0f};
    const Vec3 forward = rotate_by_quat(g_camera.quat, z_axis);
    const Vec3 world_up{0.0f, 1.0f, 0.0f};
    Vec3 right = normalize(cross(forward, world_up));
    if (dot(right, right) < 0.5f) {
        // 正俯视/正仰视时 forward 与 world_up 平行，叉积退化。此时"右"取 +X。
        right = Vec3{1.0f, 0.0f, 0.0f};
    }
    const Vec3 up = normalize(cross(right, forward));

    const float aspect = static_cast<float>(g_requested_w) / static_cast<float>(g_requested_h);
    const float tan_half = std::tan(g_camera.fov * 0.5f * kDeg2Rad);
    const float nx = (2.0f * sx / static_cast<float>(g_requested_w) - 1.0f) * tan_half * aspect;
    const float ny = (1.0f - 2.0f * sy / static_cast<float>(g_requested_h)) * tan_half;

    const Vec3 dir = normalize(add(add(forward, mul(right, nx)), mul(up, ny)));
    out_origin->x = g_camera.pos[0];
    out_origin->y = g_camera.pos[1];
    out_origin->z = g_camera.pos[2];
    out_dir->x = dir.x;
    out_dir->y = dir.y;
    out_dir->z = dir.z;
    return 1;
}

int set_display_mode(const char* mode) {
    if (!mode || !mode[0]) return 0;
    if (!display_mode_supported(mode)) return 0;

    // 统一成引擎认识的两种写法。引擎内部按字符串比较 "Wireframe"，
    // 大小写不同会静默不生效，所以这里先归一。
    const char* canonical = equals_ci(mode, "Wireframe") ? "Wireframe" : "Shaded";
    GRender_SetDisplayMode(canonical);
    std::strncpy(g_display_mode, canonical, sizeof(g_display_mode) - 1);
    g_display_mode[sizeof(g_display_mode) - 1] = '\0';
    return 1;
}

int display_mode_supported(const char* mode) {
    if (!mode || !mode[0]) return 0;
    // 非线框模式在所有后端都有意义（等于"正常渲染"），不必问后端。
    if (equals_ci(mode, "Shaded")) return 1;
    return GRender_IsDisplayModeSupported(mode) ? 1 : 0;
}

const char* display_mode() { return g_display_mode; }

GTextureHandle viewport_texture() { return GRender_GetViewportTexture(); }
GTextureHandle game_view_texture() { return GRender_GetGameViewTexture(); }

void tick() {
    // renderer 可能拒绝或夹住请求的尺寸（窗口比视口小时）。把实际值同步回来，
    // 否则面板按请求值布局，贴图会被拉伸——看起来像渲染坏了。
    int w = 0;
    int h = 0;
    if (GRender_GetViewportSize(&w, &h) == 0 && w > 0 && h > 0) {
        g_requested_w = w;
        g_requested_h = h;
    }
}

std::string info_json() {
    int vw = 0;
    int vh = 0;
    int gw = 0;
    int gh = 0;
    GRender_GetViewportSize(&vw, &vh);
    GRender_GetGameViewSize(&gw, &gh);

    Json j;
    j.begin_object();
    j.kv("width", vw);
    j.kv("height", vh);
    j.kv("gameWidth", gw);
    j.kv("gameHeight", gh);
    j.kv("output", GRender_IsViewportOutputEnabled());
    j.kv("uiBackend", GRender_GetUIBackend() == GRYCE_UI_BACKEND_BUILTIN ? "builtin" : "imgui");
    j.kv("gizmo", GRender_IsGizmoEnabled());
    j.kv("displayMode", g_display_mode);
    j.kv("wireframeSupported", GRender_IsDisplayModeSupported("Wireframe") != 0);
    j.kv("viewportTex", static_cast<unsigned long long>(
                            reinterpret_cast<std::uintptr_t>(GRender_GetViewportTexture())));
    j.kv("gameViewTex", static_cast<unsigned long long>(
                            reinterpret_cast<std::uintptr_t>(GRender_GetGameViewTexture())));
    j.end_object();
    return j.str();
}

} // namespace gryce::editor::view
