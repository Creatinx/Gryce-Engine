#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "assets/asset_manager.h"
#include "resources/gpack_bundle.h"
#include "assets/mesh_data.h"
#include "assets/obj_loader.h"
#ifdef GRYCE_HAS_ASSIMP
#include "assets/assimp_importer.h"
#endif
#include "resources/project.h"
#include "resources/resource_path.h"
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
    std::string path = resources::ResourcePath::resolve("res:/models/cube.obj");
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

TEST_F(AssetTest, AssetManagerLRUMemoryEviction) {
    assets::AssetManager::instance().clear();

    std::string path = "res:/models/cube.obj";
    assets::AssetManager::instance().set_max_cache_memory_mb(0.0001f); // ~100 bytes
    const assets::MeshData* mesh = assets::AssetManager::instance().load_mesh(path);
    ASSERT_NE(mesh, nullptr);

    // cube 占用几百字节，超过限制后应被驱逐
    EXPECT_FALSE(assets::AssetManager::instance().has_mesh(path));
    EXPECT_EQ(assets::AssetManager::instance().resident_count(), 0u);

    assets::AssetManager::instance().set_max_cache_memory_mb(0.0f);
    assets::AssetManager::instance().clear();
}

TEST_F(AssetTest, AssetManagerLRUCountEviction) {
    assets::AssetManager::instance().clear();

    std::string cube_path = "res:/models/cube.obj";
    std::string quad_path = "res:/models/quad.obj";

    assets::AssetManager::instance().set_max_cache_count(1);
    assets::AssetManager::instance().load_mesh(cube_path);
    assets::AssetManager::instance().load_mesh(quad_path);

    // 缓存上限为 1，最先加载的 cube 应被驱逐
    EXPECT_FALSE(assets::AssetManager::instance().has_mesh(cube_path));
    EXPECT_TRUE(assets::AssetManager::instance().has_mesh(quad_path));

    assets::AssetManager::instance().set_max_cache_count(0);
    assets::AssetManager::instance().clear();
}

TEST_F(AssetTest, AssetManagerLRURetainsExternalReference) {
    assets::AssetManager::instance().clear();

    std::string cube_path = "res:/models/cube.obj";
    std::string quad_path = "res:/models/quad.obj";

    auto cube_shared = assets::AssetManager::instance().load_mesh_shared(cube_path);
    ASSERT_NE(cube_shared, nullptr);

    assets::AssetManager::instance().set_max_cache_count(1);
    assets::AssetManager::instance().load_mesh(quad_path);

    // cube 被外部 shared_ptr 持有，缓存不应驱逐
    EXPECT_TRUE(assets::AssetManager::instance().has_mesh(cube_path));
    EXPECT_TRUE(assets::AssetManager::instance().has_mesh(quad_path));

    assets::AssetManager::instance().set_max_cache_count(0);
    assets::AssetManager::instance().clear();
}
