// GryceEngine embedded game entry (GryceSPC template).
//
// Runs a standalone game on the embedded GryceEngineUtils::Renderer facade,
// which owns Window + RenderContext + RenderPipeline + UIManager, so the
// classic .uif + JS HUD system works out of the box. The ECS world is updated
// by the core (GCore_BeginFrame drives physics + the gameplay script VM), and
// rendered by a generic entity-submission loop that reads mesh/material data
// through the read-only C API.
//
//   Core init -> physics attach -> Renderer(+UIManager) -> HUD load -> play loop
#include "GryceCore/core_api.h"
#include "GryceCore/scene_api.h"
#include "GryceCore/script_api.h"
#include "GryceCore/entity_api.h"
#include "GryceCore/component_api.h"
#include "GryceCore/material_api.h"
#include "GrycePlatform/input_api.h"
#include "GrycePhysics/physics_api.h"

// GryceEngineUtils：嵌入式 Renderer / UIManager / .uif HUD 系统
#include "GryceEngineUtils/types.h"
#include "GryceEngineUtils/renderer.h"
#include "GryceEngineUtils/ui/ui.h"
// .uif 自定义 DSL：build_from_dsl_source 全流程入口（Lexer→Parser→Semantic→Optimizer→Builder）
#include "GryceEngineUtils/ui/uif_builder.h"
#include "GryceEngineUtils/ui/dsl/semantic_analyzer.h"
// 渲染内存抽象：LightData（Renderer::set_lights 用）
#include "render/storage_rd/light_storage.h"
#include "render/rendering_server.h"
// 数学（Matrix4f / Camera / 向量）
#include "math/math.h"
// 资源解析：打包产物（资源仅存 .gpkg）统一经 AssetManager 从磁盘/挂载包解析。
#include "assets/asset_manager.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#include <delayimp.h>
#endif

namespace {

typedef GryceEngineUtils::Renderer        GRenderer;
typedef GryceEngineUtils::ui::UIManager   GUIManager;
typedef GryceEngineUtils::ui::ScriptVM    GScriptVM;
typedef GryceEngineUtils::ui::EngineBridge GEngineBridge;
typedef gryce_engine::math::Vector3f      GVec;
typedef gryce_engine::math::Quaternionf   GQuat4;
typedef gryce_engine::math::Matrix4f      GMat;
typedef gryce_engine::math::Camera        GCamera;
typedef gryce_engine::render::LightType   GLightType;
typedef gryce_engine::render::LightData   GLight;

// ---------------------------------------------------------------------------
// 平台引导（DLL 搜索路径 / delay-load 兜底 / CRT 固定）。
// 与旧 GWindow 路径共用，保证打包后的独立 exe 能先定位 runtime/ 再启动。
// ---------------------------------------------------------------------------
#if defined(_WIN32)
void write_boot_log(const std::wstring& exe_dir, const std::string& line) {
    std::ofstream log(std::filesystem::path(exe_dir) / "gryce_boot.log", std::ios::app);
    if (log) log << line << "\n";
}

extern "C" FARPROC WINAPI GryceDelayLoadHook(unsigned event, PDelayLoadInfo info) {
    if (event != dliFailLoadLib || !info || !info->szDll) return nullptr;

    wchar_t exe_buf[MAX_PATH + 1] = {};
    std::wstring exe_dir;
    if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) > 0) {
        exe_dir = std::filesystem::path(exe_buf).parent_path().wstring();
    }
    const std::string msg = std::string("delay-load failed: ") + info->szDll;
    write_boot_log(exe_dir, msg);

    if (!exe_dir.empty()) {
        const int wide_len = MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, nullptr, 0);
        std::wstring wide(static_cast<size_t>(wide_len > 0 ? wide_len : 1), L'\0');
        if (wide_len > 0) {
            MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, wide.data(), wide_len);
            const std::wstring full = exe_dir + L"\\runtime\\" + wide;
            HMODULE h = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (h) return reinterpret_cast<FARPROC>(h);
        }
    }

    MessageBoxW(nullptr,
                (L"GryceGame failed to start:\n" +
                 std::wstring(L"missing runtime DLL: ") +
                 [&]() {
                     std::wstring w;
                     const int n = MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, nullptr, 0);
                     if (n > 0) {
                         w.resize(n - 1);
                         MultiByteToWideChar(CP_ACP, 0, info->szDll, -1, w.data(), n);
                     }
                     return w;
                 }() + L"\nSee gryce_boot.log next to the game.").c_str(),
                L"GryceGame", MB_OK | MB_ICONERROR);
    return nullptr;
}

PfnDliHook __pfnDliFailureHook2 = GryceDelayLoadHook;
#endif

std::string argv0_override;

// 默认项目根：exe 所在目录。GryceGC 打包后 .gpkg 位于 exe 旁，res:/ 由此解析。
std::string default_project_root() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::filesystem::path p(buf);
        return p.parent_path().string();
    }
#endif
    std::filesystem::path p(argv0_override.empty() ? "." : argv0_override);
    return std::filesystem::absolute(p).parent_path().string();
}

// 让核心进入 Play 模式（驱动 ScriptSystem 运行 game.js）。
void enter_play_mode() {
    GCommand cmd{};
    cmd.type = ECMD_PLAY_MODE;
    cmd.seq = 0;
    GCore_PushCommand(&cmd);
}

// 读取文本资源（.uif / .js）。经 AssetManager 解析（磁盘优先，gpkg 内提取兜底），
// 使打包产物（无散文件）与开发模式（源目录散文件）都能正确定位。
std::string read_res_text(const std::string& res_path) {
    const std::string abs = gryce_engine::assets::AssetManager::instance().resolve_any(res_path);
    if (abs.empty()) return {};
    std::ifstream f(abs, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 读本地坐标属性（float 标量/数组）。entity 上 comp 名为 name 时会给出其 type hash。
// best-effort：读取失败回退默认值。
float read_comp_float(GEntityHandle e, uint64_t hash, const char* name, float def) {
    float v = def;
    if (GComponent_GetProperty(e, hash, name, &v, (int)sizeof(float)) == 0) return v;
    return def;
}

// GComponent_GetTypeNameAt 返回的是 demangle 后的 C++ RTTI 全限定名
//（如 "gryce_engine::components::MeshRenderer"）。这里提取最后的短名做匹配。
std::string short_type_name(const char* full) {
    std::string n = full ? full : "";
    // 注意：必须用 rfind 查找子串 "::"，不能用 find_last_of（后者把双冒号当
    // 字符集合，只会定位到最后一个 ':'，导致下面 p+2 多跳一个字符、砍掉短名
    // 的首字母，使 "MeshRenderer" 变成 "eshRenderer" 而匹配失败）。
    const auto p = n.rfind("::");
    return (p == std::string::npos) ? n : n.substr(p + 2);
}

// ---------------------------------------------------------------------------
// 通用世界提交：遍历 ECS 场景实体，读相机/灯光/网格，逐个 draw 到 Renderer。
// 纯粹通过只读 C API 完成，不依赖 Editor UI / 具体场景结构。
// ---------------------------------------------------------------------------
struct SceneResources {
    std::unordered_map<GEntityHandle, GryceEngineUtils::IMaterial*> entity_materials;
    std::unordered_map<std::string, GryceEngineUtils::IMesh*>       mesh_cache;
};

void render_world(GRenderer* r, SceneResources& rr, int viewport_w, int viewport_h) {
    int count = GEntity_GetCount();
    if (count < 0) count = 0;

    // 第一遍：收集相机与灯光。
    GEntityHandle cam_entity = 0;
    float cam_fov = 60.0f, cam_near = 0.1f, cam_far = 100.0f;
    std::vector<GLight> lights;

    for (int i = 0; i < count; ++i) {
        const GEntityHandle e = GEntity_GetAt(i);
        if (e <= 0) continue;

        const int cc = GComponent_GetCount(e);
        for (int c = 0; c < cc; ++c) {
            char cname[64] = {};
            uint64_t chash = 0;
            if (GComponent_GetTypeHashAt(e, c, &chash) != 0) continue;
            // 成功时返回类型名长度（>0），仅负值表示失败——不可用 != 0 判失败。
            if (GComponent_GetTypeNameAt(e, c, cname, (int)sizeof(cname)) < 0) continue;
            const std::string cshort = short_type_name(cname);

            if (cshort == "Camera" && cam_entity == 0) {
                cam_entity = e;
                cam_fov   = read_comp_float(e, chash, "fov", 60.0f);
                cam_near  = read_comp_float(e, chash, "near_plane", 0.1f);
                cam_far   = read_comp_float(e, chash, "far_plane", 400.0f);
            } else if (cshort == "Light") {
                GLight l;
                l.type = GLightType::Directional;
                l.color = GVec::one();
                l.intensity = 1.0f;
                l.direction = GVec(0.0f, -1.0f, 0.0f);
                l.range = 10.0f;
                l.position = GVec::zero();

                int itype = (int)GLightType::Directional;
                if (GComponent_GetProperty(e, chash, "light_type", &itype, (int)sizeof(int)) == 0)
                    l.type = (GLightType)itype;
                float col[3] = {1, 1, 1};
                if (GComponent_GetProperty(e, chash, "color", col, (int)sizeof(col)) == 0)
                    l.color = GVec(col[0], col[1], col[2]);
                l.intensity = read_comp_float(e, chash, "intensity", 1.0f);
                float dir[3] = {0, -1, 0};
                if (GComponent_GetProperty(e, chash, "direction", dir, (int)sizeof(dir)) == 0)
                    l.direction = GVec(dir[0], dir[1], dir[2]);
                l.range = read_comp_float(e, chash, "range", 10.0f);
                lights.push_back(l);
            }
        }
    }

    // 无主相机 → 用一个朝下的默认相机；无灯 → 补一个方向光。
    if (cam_entity == 0) {
        // 使用第一个实体作为近似相机位（或全零兜底）。
        cam_entity = count > 0 ? GEntity_GetAt(0) : 0;
    }
    // 相机位姿：本地坐标 + 朝向 → 看向前方目标点。
    // 注意：场景实体均为根级实体（parent==null），因此本地变换即世界变换。
    // 不可用 GEntity_GetWorld* —— 它们当前是未实现的桩（返回 -1）。
    {
        GVec3 pos; GQuat rot;
        GVec eye(0.0f, 3.6f, -7.5f);   // 兜底：跑酷场景相机
        if (cam_entity != 0 &&
            GEntity_GetLocalPosition(cam_entity, &pos) == 0) {
            eye = GVec(pos.x, pos.y, pos.z);
        }
        GVec forward(0.0f, 0.0f, -1.0f); // 引擎默认前方向
        if (cam_entity != 0 &&
            GEntity_GetLocalRotation(cam_entity, &rot) == 0) {
            forward = GQuat4(rot.x, rot.y, rot.z, rot.w)
                          .rotate_vector(GVec(0.0f, 0.0f, -1.0f)).normalized();
        }

        GMat view = GMat::look_at(eye, eye + forward, GVec(0.0f, 1.0f, 0.0f));
        const float aspect = (viewport_h > 0) ? (float)viewport_w / (float)viewport_h : 16.0f / 9.0f;
        GMat proj  = GMat::perspective(gryce_engine::math::to_radians(cam_fov), aspect, cam_near, cam_far);
        r->set_camera(eye, proj * view);
    }

    if (lights.empty()) {
        GLight sun;
        sun.type = GLightType::Directional;
        sun.color = GVec(1.0f, 1.0f, 1.0f);
        sun.intensity = 1.5f;
        sun.direction = GVec(-0.4f, -1.0f, -0.3f);
        lights.push_back(sun);
    }
    r->set_lights(lights.empty() ? nullptr : lights.data(), (int)lights.size());
    r->set_ambient(GVec(0.22f, 0.22f, 0.24f));

    // 第二遍：提交所有带 MeshRenderer 的实体。
    static int dbg_last = -1;
    if (count != dbg_last) {
        dbg_last = count;
        std::printf("[game] DBG render_world entity-count=%d\n", count);
    }
    for (int i = 0; i < count; ++i) {
        const GEntityHandle e = GEntity_GetAt(i);
        if (e <= 0) continue;

        const int cc = GComponent_GetCount(e);
        for (int c = 0; c < cc; ++c) {
            char cname[64] = {};
            uint64_t chash = 0;
            if (GComponent_GetTypeHashAt(e, c, &chash) != 0) continue;
            if (GComponent_GetTypeNameAt(e, c, cname, (int)sizeof(cname)) < 0) continue;
            const std::string cshort2 = short_type_name(cname);
            if (cshort2 != "MeshRenderer") continue;

            // 网格路径
            char path[512] = {};
            if (GComponent_MeshGetPath(e, path, (int)sizeof(path)) != 0) continue;

            auto mit = rr.mesh_cache.find(path);
            if (mit == rr.mesh_cache.end()) {
                GryceEngineUtils::IMesh* m = r->load_mesh(path);
                rr.mesh_cache[path] = m;
                mit = rr.mesh_cache.find(path);
            }
            GryceEngineUtils::IMesh* mesh = mit->second;
            if (!mesh) continue;

            // 材质（按实体缓存一个，避免每帧重建）
            auto& mat = rr.entity_materials[e];
            if (!mat) {
                mat = r->create_material();
                if (mat) {
                    float cr, cg, cb, rough, metal;
                    if (GComponent_MeshGetMaterial(e, &cr, &cg, &cb, &rough, &metal) != 0) {
                        cr = cg = cb = 1.0f; rough = 0.5f; metal = 0.0f;
                    }
                    mat->set_albedo(GVec(cr, cg, cb));
                    mat->set_roughness(rough);
                    mat->set_metallic(metal);
                }
            }

            // 位姿 TRS（本地变换；根级实体本地==世界）
            GVec3 pos; GQuat rot; GVec3 scl;
            GVec p(0, 0, 0), s(1, 1, 1);
            if (GEntity_GetLocalPosition(e, &pos) == 0) p = GVec(pos.x, pos.y, pos.z);
            if (GEntity_GetLocalScale(e, &scl) == 0)   s = GVec(scl.x, scl.y, scl.z);
            GMat q = GMat::identity();
            if (GEntity_GetLocalRotation(e, &rot) == 0)
                q = GMat::from_quaternion(GQuat4(rot.x, rot.y, rot.z, rot.w));
            const GMat model = GMat::translate(p) * q * GMat::scale(s);

            r->draw(mesh, mat, model);
        }
    }
}

// 把 Renderer 窗口的键盘状态镜像到 Core 输入，差分后推命令，使 engine.input 可用。
// 应在 begin_frame() 之后、GCore_BeginFrame 之前调用。
void mirror_input_to_core(GRenderer* r) {
    // 覆盖 GLFW 键区间（Space=32 到 F25≈348）
    for (int k = 32; k < 349; ++k) {
        GInput_InjectKey(k, r->key_held(k) ? GINPUT_ACTION_PRESS : GINPUT_ACTION_RELEASE);
    }
    GInput_SyncToCore();
}

// 读取 res:/ui/hud.uif（自定义 DSL 声明）并构建控件树；成功则设为 UIManager 根。
void load_hud(GRenderer* r, GUIManager* ui) {
    const std::string text = read_res_text("res:/ui/hud.uif");
    if (text.empty()) {
        std::fprintf(stderr, "[game] HUD .uif not found / empty\n");
        return;
    }
    // 全流程入口：DSL 源码 -> Lexer -> Parser -> SemanticAnalyzer -> ASTOptimizer -> UIBuilder
    std::vector<GryceEngineUtils::ui::dsl::SemanticError> sem_errors;
    GryceEngineUtils::ui::UIBuildResult build =
        GryceEngineUtils::ui::UIWidgetBuilder::build_from_dsl_source(text, &sem_errors);
    if (!build.success || !build.root) {
        std::fprintf(stderr, "[game] HUD DSL build failed: %s\n", build.error_message.c_str());
        for (const auto& e : sem_errors) {
            std::fprintf(stderr, "[game]   DSL: %s\n", e.toString().c_str());
        }
        return;
    }
    ui->set_root(build.root);
    std::printf("[game] HUD loaded: %d widgets\n", build.widget_count);
}

// 把 res:/ui/hud.js 求值进 UI ScriptVM，定义每帧驱动的 hudUpdate()。
void load_hud_script() {
    std::string code = read_res_text("res:/ui/hud.js");
    if (code.empty()) {
        std::fprintf(stderr, "[game] hud.js not found / empty\n");
        return;
    }
    GScriptVM* vm = GEngineBridge::vm();
    if (!vm) {
        std::fprintf(stderr, "[game] UI ScriptVM unavailable\n");
        return;
    }
    GryceEngineUtils::ui::ScriptResult sr = vm->eval(code, "hud.js");
    if (!sr.success) {
        std::fprintf(stderr, "[game] hud.js eval failed: %s\n", sr.error_msg.c_str());
    } else {
        std::printf("[game] hud.js loaded\n");
    }
}

} // namespace

int main(int argc, char* argv[]) {
#if defined(_WIN32)
    // 与旧模板一致：让 delay-load 的 runtime DLL 可从 runtime/ 解析。
    wchar_t exe_buf[MAX_PATH + 1] = {};
    const DWORD exe_len = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    if (exe_len > 0 && exe_len < MAX_PATH) {
        const std::filesystem::path exe_dir = std::filesystem::path(exe_buf).parent_path();
        const std::filesystem::path runtime_dir = exe_dir / "runtime";
        write_boot_log(exe_dir.wstring(), "gryce_boot: exe_dir=" + exe_dir.string());

        SetDllDirectoryW(runtime_dir.c_str());
    }
#endif

    argv0_override = argv[0];
    std::string project = default_project_root();
    const char* scene = "res:/scenes/main.gesc";
    bool scene_override = false;
    float auto_close_seconds = 0.0f;
    int width = 1280;
    int height = 720;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) project = argv[++i];
        else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) { scene = argv[++i]; scene_override = true; }
        else if (std::strcmp(argv[i], "--auto-close") == 0 && i + 1 < argc) {
            auto_close_seconds = static_cast<float>(std::atof(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--w") == 0 && i + 1 < argc) width = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--h") == 0 && i + 1 < argc) height = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::printf(
                "GryceGame (GryceSPC 游戏模板)\n"
                "  --project <dir>   项目根（默认 exe 目录）\n"
                "  --scene <res:...> 覆盖起始场景\n"
                "  --auto-close <s>  运行 s 秒后自动退出（CI/测试用）\n"
                "  --w/--h <px>      窗口尺寸\n");
            return 0;
        }
    }

    // 1. 核心初始化（载入项目主场景）。
    GCoreInitDesc core_desc{};
    core_desc.version = sizeof(GCoreInitDesc);
    core_desc.project_root = project.c_str();
    core_desc.enable_reflection = true;
    GCore_SetAutoLoadMainScene(!scene_override);
    if (GCore_Init(&core_desc) != 0) {
        std::fprintf(stderr, "[game] GCore_Init failed\n");
        return 1;
    }

    // 2. 物理。
    void* world = GCore_GetInternalWorldPtr();
    if (world && GPhysics_Init(GPHYSICS_BACKEND_JOLT) == 0) {
        GPhysics_AttachSystems(world);
    }

    // 3. 嵌入式 Renderer（自持窗口 + 渲染管线 + 渲染线程）。
    GryceEngineUtils::RendererConfig cfg;
    cfg.title = "Gryce Parkour";
    cfg.width = width;
    cfg.height = height;
    cfg.api = GryceEngineUtils::RenderAPI::OpenGL;
    cfg.hdr = true;
    GRenderer* r = GRenderer::create(cfg);
    if (!r) {
        std::fprintf(stderr, "[game] Renderer::create failed\n");
        GPhysics_Shutdown();
        GCore_Shutdown();
        return 1;
    }

    // 4. UIManager + HUD。
    GUIManager* ui = GUIManager::create(r);
    if (ui) {
        r->set_ui_manager(ui);
        load_hud(r, ui);        // .uif 控件树
        load_hud_script();      // hud.js → hudUpdate()
    } else {
        std::fprintf(stderr, "[game] UIManager::create failed; running without HUD\n");
    }

    // 5. 玩法桥接运行时（可选，engine.game.* 依赖注入）。
    if (world && ui) {
        GryceEngineUtils::GameRuntime rt;
        rt.world = reinterpret_cast<GryceEngineUtils::ecs::World*>(world);
        rt.renderer = r;
        GEngineBridge::set_game_runtime(rt);
    }

    // 6. 场景加载 + 进入 Play。
    if (scene_override && GScene_Load(scene) != 0) {
        std::fprintf(stderr, "[game] failed to load scene %s\n", scene);
    }
    enter_play_mode();

    // 7. 主循环。
    SceneResources rr;
    float auto_close_timer = 0.0f;
    while (r->is_running()) {
        r->begin_frame();               // poll 事件 + 转发输入给 UIManager

        float dt = static_cast<float>(r->delta_time());
        if (dt < 0.0f || dt > 0.05f) dt = 0.016f;

        mirror_input_to_core(r);        // Renderer 键盘 → Core（engine.input）

        GCore_BeginFrame(dt);           // physics + ScriptSystem（运行 game.js）

        render_world(r, rr, width, height); // 提交 3D 场景

        if (ui) {
            ui->update(dt);
            GScriptVM* vm = GEngineBridge::vm();
            if (vm) vm->call_function("hudUpdate"); // HUD 读取 engine.state 刷新控件
        }

        if (auto_close_seconds > 0.0f) {
            auto_close_timer += dt;
            if (auto_close_timer >= auto_close_seconds) {
                std::printf("[game] auto-close after %.1f seconds\n", auto_close_seconds);
                break;
            }
        }

        r->end_frame();                 // clear -> 3D -> ui->render() -> present
    }

    // 8. 清理（顺序与 Renderer::destroy 一致）。
    if (r) r->destroy();
    GPhysics_Shutdown();
    GCore_Shutdown();
    return 0;
}