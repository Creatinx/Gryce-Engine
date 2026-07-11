#include <gtest/gtest.h>

#include <filesystem>

#include "assets/asset_manager.h"
#include "assets/mesh_data.h"
#include "assets/obj_loader.h"
#ifdef GRYCE_HAS_ASSIMP
#include "assets/assimp_importer.h"
#endif
#include "resources/project.h"
#include "physics/physics_point.h"

using namespace gryce_engine;

class AssetTest : public ::testing::Test {
protected:
    void SetUp() override {
        resources::Project::instance().set_root(std::string(GRYCE_TEST_PROJECT_ROOT) + "/tests/fixtures");
    }
};

TEST_F(AssetTest, MeshDataToPhysicsPoints) {
    assets::MeshData mesh;
    mesh.vertices = {
        {math::Vector3f(0, 0, 0), math::Vector3f::up(), math::Vector3f::right(), math::Vector2f::zero(), math::Vector3f::one()},
        {math::Vector3f(1, 0, 0), math::Vector3f::up(), math::Vector3f::right(), math::Vector2f::zero(), math::Vector3f::one()},
        {math::Vector3f(0, 1, 0), math::Vector3f::up(), math::Vector3f::right(), math::Vector2f::zero(), math::Vector3f::one()},
    };

    auto points = mesh.to_physics_points(2.0f);
    EXPECT_EQ(points.size(), 3);
    EXPECT_EQ(points[0].position, math::Vector3f(0, 0, 0));
    EXPECT_FLOAT_EQ(points[0].mass, 2.0f);
    EXPECT_FALSE(points[0].pinned);
}

TEST_F(AssetTest, ObjLoaderLoadCubeObj) {
    std::string path = std::string(GRYCE_TEST_PROJECT_ROOT) + "/tests/fixtures/models/cube.obj";
    ASSERT_TRUE(std::filesystem::exists(path));

    assets::ObjLoader loader;
    auto meshes = loader.load(path);
    ASSERT_FALSE(meshes.empty());

    const auto& mesh = meshes[0];
    EXPECT_FALSE(mesh.vertices.empty());
    EXPECT_FALSE(mesh.indices.empty());
    EXPECT_EQ(mesh.indices.size() % 3, 0);
}

#ifdef GRYCE_HAS_ASSIMP
TEST_F(AssetTest, AssimpImporterLoadCubeObj) {
    std::string path = "res/models/cube.obj";
    ASSERT_TRUE(std::filesystem::exists(path));

    assets::AssimpImporter importer;
    auto meshes = importer.import(path);
    ASSERT_FALSE(meshes.empty());

    const auto& mesh = meshes[0];
    EXPECT_FALSE(mesh.vertices.empty());
    EXPECT_FALSE(mesh.indices.empty());
    EXPECT_EQ(mesh.indices.size() % 3, 0);
}
#endif

TEST_F(AssetTest, AssetManagerCacheMesh) {
    std::string path = "res:/models/cube.obj";

    EXPECT_FALSE(assets::AssetManager::instance().has_mesh(path));
    const assets::MeshData* mesh1 = assets::AssetManager::instance().load_mesh(path);
    ASSERT_NE(mesh1, nullptr);
    EXPECT_FALSE(mesh1->empty());

    const assets::MeshData* mesh2 = assets::AssetManager::instance().load_mesh(path);
    EXPECT_EQ(mesh1, mesh2);
    EXPECT_TRUE(assets::AssetManager::instance().has_mesh(path));

    assets::AssetManager::instance().clear();
    EXPECT_FALSE(assets::AssetManager::instance().has_mesh(path));
}
