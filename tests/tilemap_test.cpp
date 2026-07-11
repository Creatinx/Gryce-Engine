#include <gtest/gtest.h>

#include "components/2d/tilemap.h"
#include "resources/tileset.h"
#include "scene/scene.h"
#include "scene/entity.h"
#include "math/math.h"

using namespace gryce_engine;

TEST(Tilemap, BasicSetGetTile) {
    components::d2::tilemap::Tilemap tm(4, 3, 32.0f, 32.0f);
    EXPECT_EQ(tm.map_width, 4);
    EXPECT_EQ(tm.map_height, 3);
    EXPECT_EQ(tm.tiles.size(), 12u);

    tm.set_tile(0, 0, 1);
    tm.set_tile(3, 2, 5);

    EXPECT_EQ(tm.get_tile(0, 0), 1);
    EXPECT_EQ(tm.get_tile(3, 2), 5);
    EXPECT_EQ(tm.get_tile(1, 1), -1);

    // 越界返回 -1
    EXPECT_EQ(tm.get_tile(-1, 0), -1);
    EXPECT_EQ(tm.get_tile(4, 0), -1);
    EXPECT_EQ(tm.get_tile(0, 3), -1);
}

TEST(Tilemap, SerializeDeserialize) {
    components::d2::tilemap::Tilemap tm(2, 2, 16.0f, 16.0f);
    tm.tileset_path = "res:/tilesets/ground.json";
    tm.set_tile(0, 0, 0);
    tm.set_tile(1, 0, 1);
    tm.set_tile(0, 1, 1);
    tm.set_tile(1, 1, 2);
    tm.generate_colliders = true;
    tm.render_order = 5;

    nlohmann::json out;
    tm.serialize(out);

    components::d2::tilemap::Tilemap restored;
    restored.deserialize(out);

    EXPECT_EQ(restored.map_width, 2);
    EXPECT_EQ(restored.map_height, 2);
    EXPECT_EQ(restored.cell_width, 16.0f);
    EXPECT_EQ(restored.cell_height, 16.0f);
    EXPECT_EQ(restored.tileset_path, "res:/tilesets/ground.json");
    EXPECT_EQ(restored.generate_colliders, true);
    EXPECT_EQ(restored.render_order, 5);
    EXPECT_EQ(restored.get_tile(0, 0), 0);
    EXPECT_EQ(restored.get_tile(1, 1), 2);
}

TEST(Tilemap, EntityIntegration) {
    scene::Scene scene("test");
    scene::Entity* e = scene.create_entity("Level");
    auto* tm = e->add_component<components::d2::tilemap::Tilemap>(4, 1, 32.0f, 32.0f);
    tm->set_tile(0, 0, 0);
    tm->set_tile(1, 0, 1);
    tm->set_tile(2, 0, 1);
    tm->set_tile(3, 0, 0);

    EXPECT_EQ(e->get_component<components::d2::tilemap::Tilemap>()->get_tile(2, 0), 1);
}

TEST(Tileset, SerializeDeserialize) {
    resources::Tileset ts;
    ts.name = "ground";
    ts.texture_path = "res:/textures/tileset.png";
    ts.tile_width = 16;
    ts.tile_height = 16;
    ts.columns = 8;
    ts.margin = 1;
    ts.spacing = 1;
    ts.tile_count = 64;

    nlohmann::json out;
    ts.serialize(out);

    resources::Tileset restored;
    restored.deserialize(out);

    EXPECT_EQ(restored.name, "ground");
    EXPECT_EQ(restored.texture_path, "res:/textures/tileset.png");
    EXPECT_EQ(restored.tile_width, 16);
    EXPECT_EQ(restored.tile_height, 16);
    EXPECT_EQ(restored.columns, 8);
    EXPECT_EQ(restored.margin, 1);
    EXPECT_EQ(restored.spacing, 1);
    EXPECT_EQ(restored.tile_count, 64);
}
