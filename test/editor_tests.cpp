// ===========================================================================
// test/editor_tests.cpp
// 编辑器前置能力的接口级测试：编辑命令载荷、撤销/重做、场景存档往返。
//
// 与 math/script/asset 三个测试不同，本测试**必须链接 GryceCore.dll** ——
// 撤销栈、命令消费、场景序列化都在 core 内部，没有可脱离的纯计算内核。
// 好在这些路径都不碰 GPU / 窗口：GCore_Init 只建 World 与场景，因此 CI 上
// 不需要显卡也不需要无头显示。
//
// 观测手段刻意选"场景 JSON"而不是实体计数：GEntity_GetCount 只统计有父节点
// 的实体（demo 场景会返回 0），把它当断言依据会得到假阳性。
//
// 每个 suite 注册成一条独立 CTest：任一条失败 -> 进程非零 -> CI 不通过。
//   ctest -R editor_ --suite edit|undo|scene
// ===========================================================================

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "api/core_api.h"
#include "api/edit_api.h"
#include "api/entity_api.h"
#include "api/scene_api.h"
#include "api/types.h"

namespace fs = std::filesystem;

// ===========================================================================
// 迷你断言框架（与其它测试同风格，无第三方依赖）
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

#define CHECK_INT(actual, expected)                                                  \
    do {                                                                             \
        ++::g_checks; ++::g_case_checks;                                             \
        const long long a_ = static_cast<long long>(actual);                          \
        const long long b_ = static_cast<long long>(expected);                        \
        if (a_ != b_) {                                                              \
            char buf_[256];                                                          \
            std::snprintf(buf_, sizeof(buf_), "%s => %lld, expected %lld", #actual,   \
                          a_, b_);                                                   \
            ::fail_at(__FILE__, __LINE__, buf_);                                     \
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

// ===========================================================================
// core 生命周期
// ===========================================================================
namespace {

std::string& blank_scene_path() {
    static std::string path;
    return path;
}

bool core_init() {
    static int state = 0;   // 0 = 未初始化，1 = 成功，-1 = 失败
    if (state != 0) return state > 0;

    GCoreInitDesc desc{};
    // GCore_Init 用 version 做 ABI 校验：必须等于结构体大小，不能填版本号。
    desc.version = static_cast<uint32_t>(sizeof(GCoreInitDesc));
    desc.project_root = GRYCE_TEST_PROJECT_DIR;
    desc.enable_reflection = false;
    GCore_SetAutoLoadMainScene(false);   // 测试自己决定加载哪个场景

    if (GCore_Init(&desc) != 0) {
        std::printf("      GCore_Init failed (project_root=%s)\n", GRYCE_TEST_PROJECT_DIR);
        state = -1;
        return false;
    }
    // 复位基线：GCore_Init 建出来的空场景先存一份，每个用例用它还原。
    // 刻意不用 GScene_New —— 那条路径会把新场景写进 res:/scenes/scene_3d.gesc，
    // 在测试里等于往工程目录写文件。
    const fs::path blank = fs::temp_directory_path() / "gryce_editor_blank.gesc";
    blank_scene_path() = blank.string();
    // 场景 API 的返回约定是 0 = 成功、-1 = 失败（与 GEdit_* 的 1/0 不同）
    if (GScene_Save(blank_scene_path().c_str()) != 0) {
        std::printf("      failed to save baseline scene to %s\n", blank_scene_path().c_str());
        state = -1;
        return false;
    }
    state = 1;
    return true;
}

// 推一条命令并让它被消费（process_command 在 BeginFrame 里跑）。
void pump_commands() {
    GCore_BeginFrame(1.0f / 60.0f);
    GCore_EndFrame();
}

// 回到空白场景并清空编辑历史（每个用例的起点）。
void reset_scene() {
    GScene_Load(blank_scene_path().c_str());
    GCore_ClearUndoHistory();
}

// 场景当前内容（存档 JSON）。用于把"撤销是否真的恢复了上一状态"变成字符串比较。
std::string scene_json(const std::string& tag) {
    const fs::path path = fs::temp_directory_path() / ("gryce_editor_test_" + tag + ".gesc");
    if (GScene_Save(path.string().c_str()) != 0) return std::string();
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::error_code ec;
    fs::remove(path, ec);
    return text;
}

// 造一个新实体并消费命令，返回其句柄（0 = 失败）。
GEntityHandle create_entity(const char* name) {
    if (!GEdit_CreateEntity(name, 0)) return 0;
    pump_commands();
    return GEntity_GetSelected();
}

} // namespace

// ===========================================================================
// suite: edit —— 命令载荷与参数校验
// ===========================================================================
TEST(edit, payload_layout) {
    // 命令载荷上限 256 字节（GCore_PushCommand 的拷贝缓冲），超了会被静默丢弃。
    CHECK_TRUE(sizeof(GCmdCreateEntity) <= 256);
    CHECK_TRUE(sizeof(GCmdRenameEntity) <= 256);
    CHECK_TRUE(sizeof(GCmdReparentEntity) <= 256);
    CHECK_TRUE(sizeof(GCmdSetTransform) <= 256);
    CHECK_TRUE(sizeof(GCmdSetProperty) <= 256);
    CHECK_TRUE(sizeof(GCmdAddComponent) <= 256);
    CHECK_TRUE(sizeof(GCmdRemoveComponent) <= 256);
    CHECK_TRUE(sizeof(GCmdSetMaterial) <= 256);
    CHECK_TRUE(sizeof(GCmdSetRenderTarget) <= 256);
    CHECK_TRUE(sizeof(GCmdGizmoManipulate) <= 256);
}

TEST(edit, invalid_arguments_rejected) {
    // 空名字 / 空句柄：入队前就拒绝，返回 0（而不是让 core 里出现半截状态）
    CHECK_INT(GEdit_CreateEntity(nullptr, 0), 0);
    CHECK_INT(GEdit_CreateEntity("", 0), 0);
    CHECK_INT(GEdit_RenameEntity(0, "x"), 0);
    CHECK_INT(GEdit_RenameEntity(1, nullptr), 0);
    CHECK_INT(GEdit_DestroyEntity(0), 0);
    CHECK_INT(GEdit_SetTransform(0, nullptr, nullptr, nullptr), 0);
    CHECK_INT(GEdit_SetProperty(0, 0, nullptr, nullptr, 0), 0);
    CHECK_INT(GEdit_AddComponent(0, nullptr), 0);
    CHECK_INT(GEdit_SetMaterial(0, "a.obj", 1.0f, 1.0f, 1.0f, 0.5f, 0.0f), 0);
    CHECK_INT(GEdit_SetRenderTargetSize(2, 64, 64), 0);   // 只有 0/1 两个目标
}

TEST(edit, create_select_rename) {
    if (!core_init()) { CHECK_TRUE(false); return; }
    reset_scene();

    CHECK_TRUE(GEdit_CreateEntity("Props", 0));
    pump_commands();
    // create 命令执行后会自动选中新实体（编辑器面板依赖这点）
    const GEntityHandle h = GEntity_GetSelected();
    CHECK_TRUE(h != 0);

    char buf[128] = {0};
    GEntity_GetName(h, buf, sizeof(buf));
    CHECK_TRUE(std::strcmp(buf, "Props") == 0);

    CHECK_TRUE(GEdit_RenameEntity(h, "Props2"));
    pump_commands();
    std::memset(buf, 0, sizeof(buf));
    const int len = GEntity_GetName(h, buf, sizeof(buf));
    // 注意：GEntity_GetName 返回的是名字长度，不是错误码
    CHECK_TRUE(len > 0);
    CHECK_TRUE(std::strcmp(buf, "Props2") == 0);
}

TEST(edit, transform_roundtrip) {
    if (!core_init()) { CHECK_TRUE(false); return; }
    reset_scene();

    const GEntityHandle h = create_entity("Mover");
    CHECK_TRUE(h != 0);

    GVec3 pos{1.0f, 2.0f, 3.0f};
    GQuat rot{0.0f, 0.0f, 0.0f, 1.0f};
    GVec3 scale{2.0f, 2.0f, 2.0f};
    CHECK_TRUE(GEdit_SetTransform(h, &pos, &rot, &scale));
    pump_commands();

    GVec3 got_pos{};
    GVec3 got_scale{};
    CHECK_INT(GEntity_GetLocalPosition(h, &got_pos), 0);
    CHECK_INT(GEntity_GetLocalScale(h, &got_scale), 0);
    CHECK_NEAR(got_pos.x, 1.0f, 1e-5);
    CHECK_NEAR(got_pos.y, 2.0f, 1e-5);
    CHECK_NEAR(got_pos.z, 3.0f, 1e-5);
    CHECK_NEAR(got_scale.x, 2.0f, 1e-5);

    // nullptr 分量表示"保持原值"：只改位置不该动缩放
    GVec3 only_pos{5.0f, 6.0f, 7.0f};
    CHECK_TRUE(GEdit_SetTransform(h, &only_pos, nullptr, nullptr));
    pump_commands();
    CHECK_INT(GEntity_GetLocalPosition(h, &got_pos), 0);
    CHECK_INT(GEntity_GetLocalScale(h, &got_scale), 0);
    CHECK_NEAR(got_pos.x, 5.0f, 1e-5);
    CHECK_NEAR(got_scale.x, 2.0f, 1e-5);
}

// ===========================================================================
// suite: undo —— 快照式撤销/重做
// ===========================================================================
TEST(undo, create_then_undo_redo) {
    if (!core_init()) { CHECK_TRUE(false); return; }
    reset_scene();

    CHECK_INT(GCore_GetUndoCount(), 0);
    CHECK_INT(GCore_Redo(), 0);          // 空栈：撤销/重做都不做事

    const std::string empty = scene_json("undo_empty");
    create_entity("Box");
    const std::string with_box = scene_json("undo_box");
    CHECK_TRUE(!with_box.empty());
    CHECK_TRUE(with_box != empty);
    CHECK_TRUE(GCore_GetUndoCount() >= 1);

    CHECK_INT(GCore_Undo(), 1);
    const std::string after_undo = scene_json("undo_after");
    CHECK_TRUE(after_undo == empty);     // 撤销必须逐字节回到创建前
    CHECK_INT(GCore_GetRedoCount(), 1);

    CHECK_INT(GCore_Redo(), 1);
    const std::string after_redo = scene_json("redo_after");
    CHECK_TRUE(after_redo == with_box);
    CHECK_TRUE(GCore_GetUndoCount() >= 1);
}

TEST(undo, grouped_edit_is_one_step) {
    if (!core_init()) { CHECK_TRUE(false); return; }
    reset_scene();

    const GEntityHandle h = create_entity("Drag");
    CHECK_TRUE(h != 0);
    const int base = GCore_GetUndoCount();

    // 一次 Gizmo 拖拽 = N 条 SetTransform 命令，但只应算一步
    GCore_BeginEdit("Drag");
    for (int i = 1; i <= 5; ++i) {
        GVec3 pos{static_cast<float>(i), 0.0f, 0.0f};
        CHECK_TRUE(GEdit_SetTransform(h, &pos, nullptr, nullptr));
        pump_commands();
    }
    GCore_EndEdit();

    CHECK_INT(GCore_GetUndoCount(), base + 1);

    // 撤销一次即回到拖拽开始前（x = 0）
    CHECK_INT(GCore_Undo(), 1);
    GVec3 pos{};
    CHECK_INT(GEntity_GetLocalPosition(h, &pos), 0);
    CHECK_NEAR(pos.x, 0.0f, 1e-5);
}

TEST(undo, limit_and_clear) {
    if (!core_init()) { CHECK_TRUE(false); return; }
    reset_scene();

    GCore_SetUndoLimit(3);
    for (int i = 0; i < 6; ++i) {
        GEdit_CreateEntity("Spam", 0);
        pump_commands();
    }
    CHECK_TRUE(GCore_GetUndoCount() <= 3);

    GCore_ClearUndoHistory();
    CHECK_INT(GCore_GetUndoCount(), 0);
    CHECK_INT(GCore_GetRedoCount(), 0);

    GCore_SetUndoLimit(64);   // 还原默认，避免影响后续用例
}

TEST(undo, load_scene_clears_history) {
    if (!core_init()) { CHECK_TRUE(false); return; }
    reset_scene();

    create_entity("BeforeLoad");
    CHECK_TRUE(GCore_GetUndoCount() >= 1);

    // 换文档必须清空历史：旧快照指向的实体在新场景里并不存在
    GScene_Load(blank_scene_path().c_str());
    CHECK_INT(GCore_GetUndoCount(), 0);
    CHECK_INT(GCore_GetRedoCount(), 0);
}

TEST(undo, disabled_limit) {
    if (!core_init()) { CHECK_TRUE(false); return; }
    reset_scene();

    GCore_SetUndoLimit(0);   // 0 = 不记录
    create_entity("NoHistory");
    CHECK_INT(GCore_GetUndoCount(), 0);
    GCore_SetUndoLimit(64);
}

// ===========================================================================
// suite: scene —— 存档往返
// ===========================================================================
TEST(scene, save_load_roundtrip) {
    if (!core_init()) { CHECK_TRUE(false); return; }
    reset_scene();

    const GEntityHandle a = create_entity("Alpha");
    CHECK_TRUE(a != 0);
    GVec3 pos{1.5f, -2.25f, 0.75f};
    GQuat rot{0.0f, 0.0f, 0.0f, 1.0f};
    GVec3 scale{1.0f, 1.0f, 1.0f};
    GEdit_SetTransform(a, &pos, &rot, &scale);
    pump_commands();
    create_entity("Beta");

    // 存 -> 新场景 -> 读 -> 再存：两次存档必须逐字节相同。
    // 这同时覆盖了"序列化丢字段"和"反序列化不还原"两类回归。
    const std::string first = scene_json("rt_first");
    CHECK_TRUE(!first.empty());

    const fs::path mid = fs::temp_directory_path() / "gryce_editor_rt_mid.gesc";
    CHECK_INT(GScene_Save(mid.string().c_str()), 0);

    // 回到空白场景再读回来。这里同样不用 GScene_New：它会把新场景写进
    // res:/scenes/scene_2d.gesc，等于让测试往工程目录写文件。
    CHECK_INT(GScene_Load(blank_scene_path().c_str()), 0);
    CHECK_INT(GScene_Load(mid.string().c_str()), 0);
    pump_commands();

    const std::string second = scene_json("rt_second");
    CHECK_TRUE(!second.empty());
    CHECK_TRUE(first == second);

    // 内容层面再确认一次：存档里必须出现实体名
    CHECK_TRUE(first.find("Alpha") != std::string::npos);
    CHECK_TRUE(first.find("Beta") != std::string::npos);

    std::error_code ec;
    fs::remove(mid, ec);
}

TEST(scene, load_missing_file_fails_cleanly) {
    if (!core_init()) { CHECK_TRUE(false); return; }
    reset_scene();
    // 不存在的路径返回 -1（场景 API：0 = 成功、-1 = 失败），且当前场景保持
    // 可用 —— 后续命令仍能正常入队并执行。
    CHECK_INT(GScene_Load("res:/scenes/definitely_not_here.gesc"), -1);
    CHECK_TRUE(GEdit_CreateEntity("AfterFailedLoad", 0));
    pump_commands();
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

    GCore_Shutdown();

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
