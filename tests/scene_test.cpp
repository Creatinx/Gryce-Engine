#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "components/component_factory.h"
#include "components/2d/basic_rect.h"
#include "components/2d/label.h"
#include "resources/project.h"
#include "resources/resource_path.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"
#include "scene/entity.h"

using namespace gryce_engine;

class SceneTest : public ::testing::Test {
protected:
    void SetUp() override {
        components::register_builtin_components();
        resources::Project::instance().set_root(std::string(GRYCE_TEST_PROJECT_ROOT) + "/tests/fixtures");
    }
};

TEST_F(SceneTest, ResourcePathResolve) {
    EXPECT_TRUE(resources::ResourcePath::is_resource_path("res:/scenes/main.gesc"));
    EXPECT_FALSE(resources::ResourcePath::is_resource_path("C:/scenes/main.gesc"));

    std::string resolved = resources::ResourcePath::resolve("res:/scenes/main.gesc", "D:/Project");
    EXPECT_EQ(resolved, "D:/Project/scenes/main.gesc");

    // 不以 res:/ 开头直接返回原路径
    EXPECT_EQ(resources::ResourcePath::resolve("local/file.txt", "/root"), "local/file.txt");
}

TEST_F(SceneTest, EntityHierarchy) {
    scene::Scene scene("test");
    scene::Entity* parent = scene.create_entity("Parent");

    // 通过 add_child 建立父子关系
    auto child_ptr = std::make_unique<scene::Entity>("Child");
    scene::Entity* child2 = parent->add_child(std::move(child_ptr));

    EXPECT_EQ(parent->children().size(), 1);
    EXPECT_EQ(child2->parent(), parent);
    EXPECT_EQ(parent->children()[0].get(), child2);
}

TEST_F(SceneTest, ComponentAttachAndSerialize) {
    scene::Scene scene("test");
    scene::Entity* entity = scene.create_entity("Rect");
    entity->transform()->position = math::Vector3f(10.0f, 20.0f, 0.0f);

    auto* rect = entity->add_component<components::d2::basic_rect::ColorRect>(100.0f, 50.0f, render::Color::red());
    ASSERT_NE(rect, nullptr);
    EXPECT_EQ(rect->type(), std::string("ColorRect"));
    EXPECT_EQ(rect->owner(), entity);

    nlohmann::json json;
    rect->serialize(json);
    EXPECT_FLOAT_EQ(json["width"].get<float>(), 100.0f);
    EXPECT_FLOAT_EQ(json["height"].get<float>(), 50.0f);
    EXPECT_EQ(json["color"].size(), 4);
}

TEST_F(SceneTest, SceneSerializerRoundTrip) {
    scene::Scene scene("demo");

    scene::Entity* bg = scene.create_entity("BG");
    bg->transform()->position = math::Vector3f(5.0f, 5.0f, 0.0f);
    bg->add_component<components::d2::basic_rect::ColorRect>(180.0f, 60.0f, render::Color::gray(0.1f));

    scene::Entity* label = scene.create_entity("Label");
    label->transform()->position = math::Vector3f(15.0f, 45.0f, 0.0f);
    label->add_component<components::d2::text::Label>("Hello Scene", 32.0f, render::Color::white());

    // 序列化
    nlohmann::json json = scene::SceneSerializer::serialize(scene);
    EXPECT_EQ(json["name"].get<std::string>(), "demo");
    EXPECT_EQ(json["entities"].size(), 2);

    // 反序列化
    auto loaded = scene::SceneSerializer::deserialize(json);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->name(), "demo");

    scene::Entity* loaded_label = loaded->find_entity_by_name("Label");
    ASSERT_NE(loaded_label, nullptr);

    auto* label_comp = loaded_label->get_component<components::d2::text::Label>();
    ASSERT_NE(label_comp, nullptr);
    EXPECT_EQ(label_comp->text, "Hello Scene");
    EXPECT_FLOAT_EQ(label_comp->font_size, 32.0f);

    auto* rect_comp = loaded->find_entity_by_name("BG")->get_component<components::d2::basic_rect::ColorRect>();
    ASSERT_NE(rect_comp, nullptr);
    EXPECT_FLOAT_EQ(rect_comp->width, 180.0f);
    EXPECT_FLOAT_EQ(rect_comp->height, 60.0f);
}

TEST_F(SceneTest, SaveAndLoadFile) {
    std::string path = "res:/scenes/test_scene.gesc";
    std::string resolved = resources::ResourcePath::resolve(path);

    // 清理旧文件
    std::filesystem::remove(resolved);
    std::filesystem::create_directories(std::filesystem::path(resolved).parent_path());

    scene::Scene scene("file_test");
    scene::Entity* e = scene.create_entity("FileEntity");
    e->transform()->position = math::Vector3f(1.0f, 2.0f, 3.0f);
    e->add_component<components::d2::basic_rect::ColorRect>(50.0f, 50.0f, render::Color::blue());

    EXPECT_TRUE(scene::SceneSerializer::save_to_file(scene, path));
    EXPECT_TRUE(std::filesystem::exists(resolved));

    auto loaded = scene::SceneSerializer::load_from_file(path);
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->name(), "file_test");

    scene::Entity* loaded_e = loaded->find_entity_by_name("FileEntity");
    ASSERT_NE(loaded_e, nullptr);
    EXPECT_EQ(loaded_e->transform()->position, math::Vector3f(1.0f, 2.0f, 3.0f));

    std::filesystem::remove(resolved);
}
