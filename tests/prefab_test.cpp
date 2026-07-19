// tests/prefab_test.cpp
// Prefab 完整工作流测试：保存/实例化/覆盖/嵌套/序列化引用/还原/循环检测
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "components/component_factory.h"
#include "components/node3d.h"
#include "components/prefab_instance.h"
#include "components/transform.h"
#include "resources/project.h"
#include "scene/prefab.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"

using namespace gryce_engine;

namespace {

// 构建一棵模板树：root(Node3D) + child("Child", Node3D, pos=(1,2,3))
std::unique_ptr<scene::Entity> make_template_tree() {
    auto root = std::make_unique<scene::Entity>("TplRoot");
    root->add_component<components::Node3D>();
    root->transform()->position = math::Vector3f(0.0f, 0.0f, 0.0f);

    auto child = std::make_unique<scene::Entity>("Child");
    child->add_component<components::Node3D>();
    child->transform()->position = math::Vector3f(1.0f, 2.0f, 3.0f);
    root->add_child(std::move(child));

    return root;
}

} // namespace

class PrefabTest : public ::testing::Test {
protected:
    void SetUp() override {
        components::register_builtin_components();

        // 每个用例使用独立的临时资源目录
        temp_root_ = std::filesystem::temp_directory_path() /
                     ("gryce_prefab_test_" + std::to_string(
                         std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(temp_root_);
        resources::Project::instance().set_root(temp_root_.string());
        scene::Prefab::clear_cache();
    }

    void TearDown() override {
        scene::Prefab::clear_cache();
        std::error_code ec;
        std::filesystem::remove_all(temp_root_, ec);
    }

    // 保存一个模板 prefab 并返回 res:/ 路径
    std::string save_template(const std::string& file_name) {
        auto tpl = make_template_tree();
        const std::string res_path = "res:/" + file_name;
        EXPECT_TRUE(scene::Prefab::save(tpl.get(), res_path));
        return res_path;
    }

    std::filesystem::path temp_root_;
};

// 保存 + 实例化基本流程
TEST_F(PrefabTest, SaveAndInstantiate) {
    const std::string path = save_template("basic.geprefab");

    scene::Scene scene("Test");
    scene::Entity* inst = scene::Prefab::instantiate(&scene, path);
    ASSERT_NE(inst, nullptr);
    EXPECT_EQ(inst->name(), "TplRoot");
    ASSERT_EQ(inst->children().size(), 1u);
    EXPECT_EQ(inst->children()[0]->name(), "Child");
    EXPECT_FLOAT_EQ(inst->children()[0]->transform()->position.y, 2.0f);

    // 实例标记组件
    auto* pi = scene::Prefab::get_instance(inst);
    ASSERT_NE(pi, nullptr);
    EXPECT_EQ(pi->prefab_path, path);
    // root + child = 2 个成员映射
    EXPECT_EQ(pi->members.size(), 2u);
    // 子实体带模板标记，根本身不带
    EXPECT_TRUE(inst->prefab_template_uuid().empty());
    EXPECT_FALSE(inst->children()[0]->prefab_template_uuid().empty());
}

// 覆盖参数：根 transform / 改名 / 子实体覆盖 / 新增组件
TEST_F(PrefabTest, InstantiateWithOverrides) {
    const std::string path = save_template("ov.geprefab");

    nlohmann::json overrides;
    overrides["name"] = "Renamed";
    overrides["transform"] = {{"position", {10.0f, 0.0f, 0.0f}}};
    overrides["entities"]["Child"]["transform"] = {{"scale", {2.0f, 2.0f, 2.0f}}};
    overrides["entities"]["Child"]["name"] = "ChildRenamed";
    overrides["components"]["Node3D"] = {{"visible", false}};

    scene::Scene scene("Test");
    scene::Entity* inst = scene::Prefab::instantiate(&scene, path, overrides);
    ASSERT_NE(inst, nullptr);
    EXPECT_EQ(inst->name(), "Renamed");
    EXPECT_FLOAT_EQ(inst->transform()->position.x, 10.0f);

    auto* node3d = inst->get_component<components::Node3D>();
    ASSERT_NE(node3d, nullptr);
    EXPECT_FALSE(node3d->visible);

    ASSERT_EQ(inst->children().size(), 1u);
    EXPECT_EQ(inst->children()[0]->name(), "ChildRenamed");
    EXPECT_FLOAT_EQ(inst->children()[0]->transform()->scale.x, 2.0f);
}

// remove 覆盖：移除模板成员
TEST_F(PrefabTest, OverrideRemoveMember) {
    const std::string path = save_template("rm.geprefab");

    nlohmann::json overrides;
    overrides["remove"] = {"Child"};

    scene::Scene scene("Test");
    scene::Entity* inst = scene::Prefab::instantiate(&scene, path, overrides);
    ASSERT_NE(inst, nullptr);
    EXPECT_TRUE(inst->children().empty());
}

// 场景序列化：实例写成紧凑引用，加载后重新展开且 UUID 稳定
TEST_F(PrefabTest, SceneRoundTripKeepsPrefabReference) {
    const std::string path = save_template("rt.geprefab");

    scene::Scene scene("RoundTrip");
    nlohmann::json overrides;
    overrides["transform"] = {{"position", {5.0f, 6.0f, 7.0f}}};
    scene::Entity* inst = scene::Prefab::instantiate(&scene, path, overrides);
    ASSERT_NE(inst, nullptr);
    const std::string inst_uuid = inst->uuid().str();
    const std::string child_uuid = inst->children()[0]->uuid().str();

    // 运行时添加的子实体（非模板成员）
    auto added = std::make_unique<scene::Entity>("RuntimeAdded");
    inst->add_child(std::move(added));

    // 序列化
    nlohmann::json saved = scene::SceneSerializer::serialize(scene);
    const auto& entities = saved["entities"];
    ASSERT_EQ(entities.size(), 1u); // 只有实例根一个节点（模板成员不展开）
    EXPECT_EQ(entities[0].value("prefab", ""), path);
    EXPECT_TRUE(entities[0].contains("overrides"));
    EXPECT_TRUE(entities[0].contains("members"));
    ASSERT_EQ(entities[0]["children"].size(), 1u);
    EXPECT_EQ(entities[0]["children"][0].value("name", ""), "RuntimeAdded");

    // 反序列化
    auto loaded = scene::SceneSerializer::deserialize(saved);
    ASSERT_NE(loaded, nullptr);
    scene::Entity* reinst = loaded->find_entity_by_uuid(scene::UUID(inst_uuid));
    ASSERT_NE(reinst, nullptr);
    EXPECT_EQ(reinst->name(), "TplRoot");

    // 覆盖被重新应用
    EXPECT_FLOAT_EQ(reinst->transform()->position.x, 5.0f);

    // 成员 UUID 稳定
    ASSERT_FALSE(reinst->children().empty());
    scene::Entity* re_child = loaded->find_entity_by_uuid(scene::UUID(child_uuid));
    ASSERT_NE(re_child, nullptr);
    EXPECT_EQ(re_child->name(), "Child");

    // 运行时添加的子实体保留
    scene::Entity* re_added = nullptr;
    reinst->foreach([&](scene::Entity* e) {
        if (e->name() == "RuntimeAdded") re_added = e;
    });
    ASSERT_NE(re_added, nullptr);

    // 实例标记存在
    EXPECT_NE(scene::Prefab::get_instance(reinst), nullptr);
}

// 嵌套预制体：B 包含 A 的实例；修改 A 后重新实例化 B 自动传播
TEST_F(PrefabTest, NestedPrefabPropagatesChanges) {
    const std::string path_a = save_template("inner.geprefab");

    // 构建 B：包含 A 的实例
    {
        scene::Scene scene_b("B");
        scene::Entity* inst_a = scene::Prefab::instantiate(&scene_b, path_a);
        ASSERT_NE(inst_a, nullptr);
        // B 的根是 A 的实例；保存为 outer.geprefab
        ASSERT_TRUE(scene::Prefab::save(inst_a, "res:/outer.geprefab"));
    }

    scene::Prefab::clear_cache();

    // 实例化 B，检查嵌套展开
    scene::Scene scene("Test");
    scene::Entity* inst_b = scene::Prefab::instantiate(&scene, "res:/outer.geprefab");
    ASSERT_NE(inst_b, nullptr);
    EXPECT_NE(scene::Prefab::get_instance(inst_b), nullptr); // B 的实例标记
    // 子树里有 Child（来自 A 的模板）
    scene::Entity* child = nullptr;
    inst_b->foreach([&](scene::Entity* e) {
        if (e->name() == "Child") child = e;
    });
    ASSERT_NE(child, nullptr);
    EXPECT_FLOAT_EQ(child->transform()->position.y, 2.0f);

    // 修改 A（子实体位置 y: 2 -> 99），清缓存后重新实例化 B
    {
        auto tpl = make_template_tree();
        tpl->children()[0]->transform()->position.y = 99.0f;
        ASSERT_TRUE(scene::Prefab::save(tpl.get(), path_a));
    }
    scene::Prefab::clear_cache();

    scene::Scene scene2("Test2");
    scene::Entity* inst_b2 = scene::Prefab::instantiate(&scene2, "res:/outer.geprefab");
    ASSERT_NE(inst_b2, nullptr);
    scene::Entity* child2 = nullptr;
    inst_b2->foreach([&](scene::Entity* e) {
        if (e->name() == "Child") child2 = e;
    });
    ASSERT_NE(child2, nullptr);
    EXPECT_FLOAT_EQ(child2->transform()->position.y, 99.0f);
}

// revert：还原模板状态，保留运行时添加的子实体，根 UUID 不变
TEST_F(PrefabTest, RevertRestoresTemplate) {
    const std::string path = save_template("rev.geprefab");

    scene::Scene scene("Test");
    scene::Entity* inst = scene::Prefab::instantiate(&scene, path);
    ASSERT_NE(inst, nullptr);
    const std::string root_uuid = inst->uuid().str();

    // 运行时修改：移动根、删除模板子实体、添加运行时子实体
    inst->transform()->position = math::Vector3f(42.0f, 0.0f, 0.0f);
    inst->remove_child(inst->children()[0].get());
    ASSERT_TRUE(inst->children().empty());
    inst->add_child(std::make_unique<scene::Entity>("RuntimeChild"));

    ASSERT_TRUE(scene::Prefab::revert(inst));

    // 根 UUID 不变，位置还原，模板子实体恢复
    EXPECT_EQ(inst->uuid().str(), root_uuid);
    EXPECT_FLOAT_EQ(inst->transform()->position.x, 0.0f);
    bool has_tpl_child = false;
    bool has_runtime_child = false;
    for (const auto& c : inst->children()) {
        if (c->name() == "Child") has_tpl_child = true;
        if (c->name() == "RuntimeChild") has_runtime_child = true;
    }
    EXPECT_TRUE(has_tpl_child);
    EXPECT_TRUE(has_runtime_child);
}

// 循环引用检测：A 引用 B，B 引用 A -> 实例化失败但不崩溃
TEST_F(PrefabTest, CyclicReferenceDetected) {
    // 手写两个互相引用的 prefab 文件
    auto write_prefab_ref = [this](const std::string& file, const std::string& ref,
                                   const std::string& uuid_str) {
        nlohmann::json j;
        j["version"] = 1;
        j["name"] = file;
        nlohmann::json e;
        e["name"] = "InstRoot";
        e["uuid"] = uuid_str;
        e["parent"] = nullptr;
        e["prefab"] = "res:/" + ref;
        e["overrides"] = nlohmann::json::object();
        e["members"] = nlohmann::json::object();
        e["children"] = nlohmann::json::array();
        j["entities"] = {e};
        std::ofstream(temp_root_ / file) << j.dump(2);
    };
    write_prefab_ref("cyc_a.geprefab", "cyc_b.geprefab", "aaaaaaaa-0000-0000-0000-000000000001");
    write_prefab_ref("cyc_b.geprefab", "cyc_a.geprefab", "bbbbbbbb-0000-0000-0000-000000000002");

    scene::Scene scene("Test");
    scene::Entity* inst = scene::Prefab::instantiate(&scene, "res:/cyc_a.geprefab");
    EXPECT_EQ(inst, nullptr);
}

// 多根预制体：自动包一层容器实体
TEST_F(PrefabTest, MultiRootPrefabWrapped) {
    // 手写一个两根实体的 prefab
    nlohmann::json j;
    j["version"] = 1;
    j["name"] = "Multi";
    nlohmann::json e1, e2;
    e1["name"] = "RootA";
    e1["uuid"] = "cccccccc-0000-0000-0000-000000000001";
    e1["parent"] = nullptr;
    e1["transform"] = {{"position", {0, 0, 0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}};
    e1["components"] = nlohmann::json::array();
    e2["name"] = "RootB";
    e2["uuid"] = "cccccccc-0000-0000-0000-000000000002";
    e2["parent"] = nullptr;
    e2["transform"] = {{"position", {1, 0, 0}}, {"rotation", {0, 0, 0, 1}}, {"scale", {1, 1, 1}}};
    e2["components"] = nlohmann::json::array();
    j["entities"] = {e1, e2};
    std::ofstream(temp_root_ / "multi.geprefab") << j.dump(2);

    scene::Scene scene("Test");
    scene::Entity* inst = scene::Prefab::instantiate(&scene, "res:/multi.geprefab");
    ASSERT_NE(inst, nullptr);
    EXPECT_EQ(inst->name(), "multi"); // 容器实体以文件名命名
    ASSERT_EQ(inst->children().size(), 2u);
    EXPECT_NE(scene::Prefab::get_instance(inst), nullptr);
}
