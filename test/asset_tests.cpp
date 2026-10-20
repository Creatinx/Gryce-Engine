// ===========================================================================
// test/asset_tests.cpp
// 资产侧车文件（<asset>.meta）与导入设置的单元测试。
//
// 与 math/script 测试同构：被测源码（assets/asset_meta.cpp +
// assets/import_options.cpp + glog）直接编译进本可执行文件，不链接
// GryceCore.dll，也不碰 GPU / 窗口 —— CI 上毫秒级完成。
//
// 每个 suite 在 CMake 里注册成一条独立 CTest：
//   ctest 任一条失败 -> 进程返回非零 -> CI 判定不通过。
// 用 --suite <name> 可单独跑某个 suite，--list 列出全部。
// ===========================================================================

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "assets/asset_meta.h"
#include "assets/import_options.h"

using gryce_engine::assets::AssetMeta;
using gryce_engine::assets::ImportOptions;
using gryce_engine::assets::MeshData;
using gryce_engine::assets::MeshVertex;

namespace fs = std::filesystem;

// ===========================================================================
// 迷你断言框架（无第三方依赖，与 math_tests.cpp 同风格）
// ===========================================================================
namespace {

struct Case {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)()) {
        registry().push_back(Case{suite, name, fn});
    }
};

int g_checks = 0;
int g_checks_failed = 0;
int g_case_checks = 0;
int g_case_failed = 0;

void fail_at(const char* file, int line, const std::string& msg) {
    ++g_case_failed;
    ++g_checks_failed;
    std::printf("      FAIL %s:%d  %s\n", file, line, msg.c_str());
    std::fflush(stdout);
}

} // namespace

#define TEST(suite, name)                                                            \
    static void suite##__##name##_impl();                                            \
    static ::Registrar suite##__##name##_reg(#suite, #name, &suite##__##name##_impl); \
    static void suite##__##name##_impl()

#define CHECK_TRUE(expr)                                                             \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        if (!(expr)) ::fail_at(__FILE__, __LINE__, "expected true: " #expr);          \
    } while (0)

#define CHECK_FALSE(expr)                                                            \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        if (expr) ::fail_at(__FILE__, __LINE__, "expected false: " #expr);            \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                               \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        const std::string a_ = (actual);                                             \
        const std::string b_ = (expected);                                           \
        if (a_ != b_) {                                                              \
            ::fail_at(__FILE__, __LINE__,                                            \
                      std::string(#actual) + " = \"" + a_ + "\", expected \"" + b_ + "\""); \
        }                                                                            \
    } while (0)

#define CHECK_NEAR(actual, expected, eps)                                            \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        const double a_ = static_cast<double>(actual);                                \
        const double b_ = static_cast<double>(expected);                              \
        if (!(std::fabs(a_ - b_) <= static_cast<double>(eps))) {                      \
            char buf_[256];                                                          \
            std::snprintf(buf_, sizeof(buf_), "%s => %.8g, expected %.8g", #actual,   \
                          a_, b_);                                                   \
            ::fail_at(__FILE__, __LINE__, buf_);                                     \
        }                                                                            \
    } while (0)

// ---------------------------------------------------------------------------
// 临时目录：每次用例一个独立子目录，用例结束由调用方清理（失败时留下便于排查）。
// ---------------------------------------------------------------------------
namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("gryce_asset_test_" + std::to_string(++counter) + "_" +
                std::to_string(reinterpret_cast<std::uintptr_t>(&counter)));
        std::error_code ec;
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string file(const std::string& name) const { return (path / name).string(); }
};

void write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

bool is_lower_hex_32(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

// 造一个只有一个三角形的网格（顶点位置/法线/切线可辨认，便于验证变换）。
MeshData make_triangle() {
    MeshData mesh;
    mesh.name = "tri";
    for (int i = 0; i < 3; ++i) {
        MeshVertex v;
        v.position = gryce_engine::math::Vector3f(1.0f + static_cast<float>(i), 2.0f, 3.0f);
        v.normal = gryce_engine::math::Vector3f(0.0f, 1.0f, 0.0f);
        v.tangent = gryce_engine::math::Vector3f(1.0f, 0.0f, 0.0f);
        v.uv = gryce_engine::math::Vector2f(0.0f, 0.0f);
        v.color = gryce_engine::math::Vector3f::one();
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 1, 2};
    mesh.material.valid = true;
    mesh.material.roughness = 0.5f;
    return mesh;
}

} // namespace

// ===========================================================================
// suite: meta
// ===========================================================================
TEST(meta, path_suffix) {
    CHECK_EQ_STR(gryce_engine::assets::asset_meta_path("a/b/model.fbx"),
                 "a/b/model.fbx.meta");
    // 空路径不给侧车名，调用方据此早退
    CHECK_EQ_STR(gryce_engine::assets::asset_meta_path(""), "");
}

TEST(meta, guid_format_and_uniqueness) {
    std::vector<std::string> guids;
    for (int i = 0; i < 512; ++i) guids.push_back(gryce_engine::assets::generate_asset_guid());
    bool all_lower_hex = true;
    for (const auto& g : guids) all_lower_hex = all_lower_hex && is_lower_hex_32(g);
    CHECK_TRUE(all_lower_hex);

    bool unique = true;
    for (size_t i = 0; i < guids.size() && unique; ++i) {
        for (size_t j = i + 1; j < guids.size(); ++j) {
            if (guids[i] == guids[j]) { unique = false; break; }
        }
    }
    CHECK_TRUE(unique);
}

TEST(meta, create_when_missing) {
    TempDir dir;
    const std::string asset = dir.file("mesh.obj");
    write_text(asset, "v 0 0 0\n");

    // 首次：侧车不存在 -> 生成并写盘
    const std::string sidecar = gryce_engine::assets::asset_meta_path(asset);
    CHECK_FALSE(fs::exists(sidecar));

    AssetMeta meta;
    CHECK_TRUE(gryce_engine::assets::load_or_create_asset_meta(asset, meta));
    CHECK_TRUE(is_lower_hex_32(meta.guid));
    CHECK_TRUE(fs::exists(sidecar));
    CHECK_TRUE(meta.source_size > 0);
    CHECK_TRUE(meta.source_mtime_ns != 0);
    CHECK_EQ_STR(meta.importer, "mesh");

    // 二次：读到的是同一个 guid（不是重新生成）
    AssetMeta again;
    CHECK_TRUE(gryce_engine::assets::read_asset_meta(asset, again));
    CHECK_EQ_STR(again.guid, meta.guid);
}

TEST(meta, roundtrip_all_fields) {
    TempDir dir;
    const std::string asset = dir.file("scene.glb");
    write_text(asset, "glTF");

    AssetMeta src;
    src.guid = gryce_engine::assets::generate_asset_guid();
    src.source_path = "assets/scene.glb";
    src.importer = "mesh";
    src.scale = 0.01f;
    src.up_axis = 2;
    src.generate_lod = false;
    src.import_materials = false;
    src.roughness_scale = 0.75f;
    src.source_mtime_ns = 1234567890123LL;
    src.source_size = 4242;
    CHECK_TRUE(gryce_engine::assets::write_asset_meta(asset, src));

    AssetMeta dst;
    CHECK_TRUE(gryce_engine::assets::read_asset_meta(asset, dst));
    CHECK_EQ_STR(dst.guid, src.guid);
    CHECK_EQ_STR(dst.source_path, src.source_path);
    CHECK_EQ_STR(dst.importer, src.importer);
    CHECK_NEAR(dst.scale, 0.01f, 1e-6);
    CHECK_TRUE(dst.up_axis == 2);
    CHECK_FALSE(dst.generate_lod);
    CHECK_FALSE(dst.import_materials);
    CHECK_NEAR(dst.roughness_scale, 0.75f, 1e-6);
    CHECK_TRUE(dst.source_mtime_ns == 1234567890123LL);
    CHECK_TRUE(dst.source_size == 4242);
}

TEST(meta, missing_fields_use_defaults) {
    TempDir dir;
    const std::string asset = dir.file("partial.fbx");
    write_text(asset, "x");
    // 手写精简侧车：只给 guid
    write_text(gryce_engine::assets::asset_meta_path(asset), "{\"guid\":\"abc\"}");

    AssetMeta meta;
    CHECK_TRUE(gryce_engine::assets::read_asset_meta(asset, meta));
    CHECK_EQ_STR(meta.guid, "abc");
    CHECK_NEAR(meta.scale, 1.0f, 1e-6);
    CHECK_TRUE(meta.up_axis == 1);
    CHECK_TRUE(meta.import_materials);
    CHECK_TRUE(meta.generate_lod);
    CHECK_NEAR(meta.roughness_scale, 1.0f, 1e-6);
    CHECK_TRUE(meta.version == gryce_engine::assets::kAssetMetaVersion);
}

TEST(meta, missing_guid_is_filled) {
    TempDir dir;
    const std::string asset = dir.file("nogiud.obj");
    write_text(asset, "v 0 0 0\n");
    write_text(gryce_engine::assets::asset_meta_path(asset), "{\"scale\":2.0}");

    AssetMeta meta;
    CHECK_TRUE(gryce_engine::assets::read_asset_meta(asset, meta));
    // 缺 guid 时必须补一个，否则引用无法稳定
    CHECK_TRUE(is_lower_hex_32(meta.guid));
    CHECK_NEAR(meta.scale, 2.0f, 1e-6);
}

TEST(meta, malformed_json_is_rejected) {
    TempDir dir;
    const std::string asset = dir.file("bad.obj");
    write_text(asset, "v 0 0 0\n");
    write_text(gryce_engine::assets::asset_meta_path(asset), "{ this is not json ");

    AssetMeta meta;
    // 解析失败按"缺失"处理，不能抛异常也不能静默给出半截结果
    CHECK_FALSE(gryce_engine::assets::read_asset_meta(asset, meta));
}

TEST(meta, invalid_settings_fall_back_to_defaults) {
    TempDir dir;
    const std::string asset = dir.file("badsettings.obj");
    write_text(asset, "v 0 0 0\n");
    const std::string guid = gryce_engine::assets::generate_asset_guid();
    write_text(gryce_engine::assets::asset_meta_path(asset),
               "{\"guid\":\"" + guid + "\",\"scale\":0,\"up_axis\":9}");

    AssetMeta meta;
    CHECK_TRUE(gryce_engine::assets::read_asset_meta(asset, meta));
    // 非法设置回退默认值，但 guid 必须保留（identity 不能被设置项带坏）
    CHECK_NEAR(meta.scale, 1.0f, 1e-6);
    CHECK_TRUE(meta.up_axis == 1);
    CHECK_EQ_STR(meta.guid, guid);

    CHECK_FALSE(gryce_engine::assets::asset_meta_validate([] {
        AssetMeta m; m.scale = 0.0f; return m;
    }()));
    CHECK_FALSE(gryce_engine::assets::asset_meta_validate([] {
        AssetMeta m; m.up_axis = 3; return m;
    }()));
    CHECK_TRUE(gryce_engine::assets::asset_meta_validate(AssetMeta{}));
}

TEST(meta, stale_detection) {
    TempDir dir;
    const std::string asset = dir.file("moved.obj");
    write_text(asset, "v 0 0 0\n");

    AssetMeta meta;
    CHECK_TRUE(gryce_engine::assets::load_or_create_asset_meta(asset, meta));
    // 刚写完，指纹一致 -> 不过期
    CHECK_FALSE(gryce_engine::assets::asset_meta_is_stale(asset, meta));

    // 换掉源文件（大小变化，mtime 也可能变） -> 过期
    write_text(asset, "v 0 0 0\nv 1 1 1\nv 2 2 2\n");
    CHECK_TRUE(gryce_engine::assets::asset_meta_is_stale(asset, meta));

    // 源文件不存在时不报过期（资源包内资产 / 已删除的文件由上层判断）
    AssetMeta no_stamp;
    CHECK_FALSE(gryce_engine::assets::asset_meta_is_stale(dir.file("nope.obj"), no_stamp));
}

TEST(meta, read_only_write_fails_gracefully) {
    TempDir dir;
    // 源文件不存在：不写盘，但内存里的结果仍然可用（资源包内资产的正常路径）
    const std::string ghost = dir.file("missing_in_pack.fbx");
    AssetMeta meta;
    CHECK_TRUE(gryce_engine::assets::load_or_create_asset_meta(ghost, meta));
    CHECK_TRUE(is_lower_hex_32(meta.guid));
    CHECK_FALSE(fs::exists(gryce_engine::assets::asset_meta_path(ghost)));
}

// ===========================================================================
// suite: import_options
// ===========================================================================
TEST(import_options, default_is_noop) {
    std::vector<MeshData> meshes{make_triangle()};
    const MeshData before = meshes[0];
    gryce_engine::assets::apply_import_options(meshes, ImportOptions{});
    CHECK_NEAR(meshes[0].vertices[0].position.x, before.vertices[0].position.x, 1e-6);
    CHECK_NEAR(meshes[0].vertices[0].position.y, before.vertices[0].position.y, 1e-6);
    CHECK_NEAR(meshes[0].vertices[0].position.z, before.vertices[0].position.z, 1e-6);
    CHECK_NEAR(meshes[0].material.roughness, before.material.roughness, 1e-6);
    CHECK_TRUE(meshes[0].material.valid);
}

TEST(import_options, scale) {
    std::vector<MeshData> meshes{make_triangle()};
    ImportOptions opt;
    opt.global_scale = 0.01f;
    gryce_engine::assets::apply_import_options(meshes, opt);
    // 位置缩放，法线/切线方向不变
    CHECK_NEAR(meshes[0].vertices[0].position.x, 0.01f, 1e-6);
    CHECK_NEAR(meshes[0].vertices[0].position.y, 0.02f, 1e-6);
    CHECK_NEAR(meshes[0].vertices[0].position.z, 0.03f, 1e-6);
    CHECK_NEAR(meshes[0].vertices[0].normal.y, 1.0f, 1e-6);
    CHECK_NEAR(meshes[0].vertices[0].tangent.x, 1.0f, 1e-6);
}

TEST(import_options, z_up_conversion) {
    std::vector<MeshData> meshes{make_triangle()};
    ImportOptions opt;
    opt.up_axis = 2;   // Z-up -> Y-up：(x, y, z) -> (x, z, -y)
    gryce_engine::assets::apply_import_options(meshes, opt);

    const auto& v = meshes[0].vertices[0];
    CHECK_NEAR(v.position.x, 1.0f, 1e-6);
    CHECK_NEAR(v.position.y, 3.0f, 1e-6);
    CHECK_NEAR(v.position.z, -2.0f, 1e-6);
    // 法线 (0,1,0) -> (0,0,-1)
    CHECK_NEAR(v.normal.x, 0.0f, 1e-6);
    CHECK_NEAR(v.normal.y, 0.0f, 1e-6);
    CHECK_NEAR(v.normal.z, -1.0f, 1e-6);
    // 切线 (1,0,0) -> (1,0,0)（沿 X 轴旋转不变）
    CHECK_NEAR(v.tangent.x, 1.0f, 1e-6);
    CHECK_NEAR(v.tangent.y, 0.0f, 1e-6);
    CHECK_NEAR(v.tangent.z, 0.0f, 1e-6);
}

TEST(import_options, x_up_conversion) {
    std::vector<MeshData> meshes{make_triangle()};
    ImportOptions opt;
    opt.up_axis = 0;   // X-up -> Y-up：(x, y, z) -> (-y, x, z)
    gryce_engine::assets::apply_import_options(meshes, opt);

    const auto& v = meshes[0].vertices[0];
    CHECK_NEAR(v.position.x, -2.0f, 1e-6);
    CHECK_NEAR(v.position.y, 1.0f, 1e-6);
    CHECK_NEAR(v.position.z, 3.0f, 1e-6);
    CHECK_NEAR(v.normal.x, -1.0f, 1e-6);
    CHECK_NEAR(v.normal.y, 0.0f, 1e-6);
}

TEST(import_options, materials_off) {
    std::vector<MeshData> meshes{make_triangle()};
    ImportOptions opt;
    opt.import_materials = false;
    gryce_engine::assets::apply_import_options(meshes, opt);
    // valid=false 让 MeshRenderer 走组件默认材质（白模）
    CHECK_FALSE(meshes[0].material.valid);
}

TEST(import_options, roughness_scale_clamped) {
    std::vector<MeshData> meshes{make_triangle()};
    ImportOptions opt;
    opt.roughness_scale = 4.0f;   // 0.5 * 4 = 2.0 -> 钳到 1.0
    gryce_engine::assets::apply_import_options(meshes, opt);
    CHECK_NEAR(meshes[0].material.roughness, 1.0f, 1e-6);

    std::vector<MeshData> dark{make_triangle()};
    opt.roughness_scale = 0.5f;   // 0.5 * 0.5 = 0.25
    gryce_engine::assets::apply_import_options(dark, opt);
    CHECK_NEAR(dark[0].material.roughness, 0.25f, 1e-6);
}

TEST(import_options, invalid_scale_ignored) {
    std::vector<MeshData> meshes{make_triangle()};
    ImportOptions opt;
    opt.global_scale = 0.0f;   // 非法：不缩放，而不是缩成零体积
    gryce_engine::assets::apply_import_options(meshes, opt);
    CHECK_NEAR(meshes[0].vertices[0].position.x, 1.0f, 1e-6);
}

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char** argv) {
    std::vector<std::string> wanted;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list") {
            std::vector<std::string> suites;
            for (const Case& c : registry()) {
                bool seen = false;
                for (const auto& s : suites) seen = seen || (s == c.suite);
                if (!seen) suites.push_back(c.suite);
            }
            for (const auto& s : suites) std::printf("%s\n", s.c_str());
            return 0;
        }
        if (arg == "--suite" && i + 1 < argc) {
            wanted.push_back(argv[++i]);
            continue;
        }
        if (!arg.empty() && arg[0] != '-') wanted.push_back(arg);
    }

    std::vector<std::string> suites;
    for (const Case& c : registry()) {
        bool seen = false;
        for (const auto& s : suites) seen = seen || (s == c.suite);
        if (!seen) suites.push_back(c.suite);
    }

    int cases_total = 0;
    int cases_failed = 0;
    for (const std::string& s : suites) {
        if (!wanted.empty()) {
            bool sel = false;
            for (const std::string& w : wanted) {
                if (w == s) { sel = true; break; }
            }
            if (!sel) continue;
        }
        std::printf("[ SUITE ] %s\n", s.c_str());
        for (const Case& c : registry()) {
            if (c.suite != s) continue;
            g_case_checks = 0;
            g_case_failed = 0;
            c.fn();
            ++cases_total;
            if (g_case_failed > 0) {
                ++cases_failed;
                std::printf("  [FAIL] %s (%d/%d checks failed)\n",
                            c.name.c_str(), g_case_failed, g_case_checks);
            } else {
                std::printf("  [ OK  ] %s (%d checks)\n", c.name.c_str(), g_case_checks);
            }
        }
    }

    std::printf("\n--------------------------------------------------------------\n");
    std::printf("checks: %d total, %d passed, %d failed\n",
                g_checks, g_checks - g_checks_failed, g_checks_failed);
    std::printf("cases : %d total, %d passed, %d failed\n",
                cases_total, cases_total - cases_failed, cases_failed);
    if (cases_failed == 0 && g_checks_failed == 0) {
        std::printf("RESULT: PASS\n");
        return 0;
    }
    std::printf("RESULT: FAIL\n");
    return 1;
}
