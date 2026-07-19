#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "components/transform.h"
#include "components/mesh_renderer.h"
#include "components/light.h"
#include "components/camera.h"
#include "reflection/reflection.h"

using namespace gryce_engine;

namespace {

const reflection::FieldInfo* find_field(const std::string& type, const std::string& field) {
    auto fields = reflection::Registry::instance().all_fields(type);
    for (const auto* f : fields) {
        if (f->name == field) return f;
    }
    return nullptr;
}

// 测试用本地类型：验证宏对未内建类型的可用性 + 只读/范围标记
struct LocalWidget {
    float value = 1.5f;
    int count = 3;
    std::string label = "hello";
    math::Vector3f position = math::Vector3f::zero();
    bool locked = false;
};

} // namespace

GRYCE_REFLECT_CLASS(LocalWidget, )
    GRYCE_REFLECT_FIELD(value)
    GRYCE_REFLECT_FIELD_RANGE(count, 0.0f, 10.0f)
    GRYCE_REFLECT_FIELD(label)
    GRYCE_REFLECT_FIELD(position)
    GRYCE_REFLECT_FIELD_RO(locked)
GRYCE_REFLECT_END()

// ---------------------------------------------------------------------------
// 字段枚举
// ---------------------------------------------------------------------------

TEST(ReflectionTest, EnumeratesTransformFields) {
    auto fields = reflection::Registry::instance().all_fields("Transform");
    ASSERT_EQ(fields.size(), 4u); // enabled（基类）+ position/rotation/scale

    EXPECT_EQ(fields[0]->name, "enabled");          // 基类字段在前
    EXPECT_EQ(fields[0]->type, reflection::FieldType::Bool);
    EXPECT_EQ(fields[1]->name, "position");
    EXPECT_EQ(fields[1]->type, reflection::FieldType::Vector3f);
    EXPECT_EQ(fields[2]->name, "rotation");
    EXPECT_EQ(fields[2]->type, reflection::FieldType::Quaternionf);
    EXPECT_EQ(fields[3]->name, "scale");
}

TEST(ReflectionTest, UnregisteredTypeReturnsEmpty) {
    EXPECT_EQ(reflection::Registry::instance().find("NoSuchComponent"), nullptr);
    EXPECT_TRUE(reflection::Registry::instance().all_fields("NoSuchComponent").empty());
}

TEST(ReflectionTest, FieldMetadataRangeAndReadOnly) {
    const auto* speed = find_field("Camera", "fov");
    ASSERT_NE(speed, nullptr);
    EXPECT_TRUE(speed->has_range);
    EXPECT_FLOAT_EQ(speed->range_min, 1.0f);
    EXPECT_FLOAT_EQ(speed->range_max, 179.0f);
    EXPECT_FALSE(speed->read_only);

    const auto* time = find_field("SkinnedMeshRenderer", "time");
    ASSERT_NE(time, nullptr);
    EXPECT_TRUE(time->read_only);
}

// ---------------------------------------------------------------------------
// 类型擦除读写
// ---------------------------------------------------------------------------

TEST(ReflectionTest, ReadWriteVector3fField) {
    components::Transform t;
    const auto* f = find_field("Transform", "position");
    ASSERT_NE(f, nullptr);

    auto v = reflection::read_field<math::Vector3f>(&t, *f);
    EXPECT_FLOAT_EQ(v.x, 0.0f);

    ASSERT_TRUE(reflection::write_field(&t, *f, math::Vector3f(1.0f, 2.0f, 3.0f)));
    EXPECT_FLOAT_EQ(t.position.x, 1.0f);
    EXPECT_FLOAT_EQ(t.position.y, 2.0f);
    EXPECT_FLOAT_EQ(t.position.z, 3.0f);
}

TEST(ReflectionTest, ReadWriteStringField) {
    components::MeshRenderer mr;
    const auto* f = find_field("MeshRenderer", "mesh_path");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->type, reflection::FieldType::String);

    ASSERT_TRUE(reflection::write_field<std::string>(&mr, *f, "res:/models/cube.obj"));
    EXPECT_EQ(mr.mesh_path, "res:/models/cube.obj");
    EXPECT_EQ(reflection::read_field<std::string>(&mr, *f), "res:/models/cube.obj");
}

TEST(ReflectionTest, ReadWriteFloatField) {
    components::Light light;
    const auto* f = find_field("Light", "intensity");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->type, reflection::FieldType::Float);
    EXPECT_TRUE(f->has_range);

    ASSERT_TRUE(reflection::write_field(&light, *f, 3.5f));
    EXPECT_FLOAT_EQ(light.intensity, 3.5f);
    EXPECT_FLOAT_EQ(reflection::read_field<float>(&light, *f), 3.5f);
}

TEST(ReflectionTest, InheritedFieldReadWrite) {
    components::Light light;
    const auto* f = find_field("Light", "enabled"); // 基类 Component 字段
    ASSERT_NE(f, nullptr);

    EXPECT_TRUE(reflection::read_field<bool>(&light, *f));
    ASSERT_TRUE(reflection::write_field(&light, *f, false));
    EXPECT_FALSE(light.enabled);
}

TEST(ReflectionTest, ReadOnlyFieldRejectsWrite) {
    LocalWidget w;
    const auto* f = find_field("LocalWidget", "locked");
    ASSERT_NE(f, nullptr);
    EXPECT_TRUE(f->read_only);

    EXPECT_FALSE(reflection::write_field(&w, *f, true));
    EXPECT_FALSE(w.locked); // 未被修改
}

TEST(ReflectionTest, LocalTypeFieldsRegistered) {
    auto fields = reflection::Registry::instance().all_fields("LocalWidget");
    ASSERT_EQ(fields.size(), 5u);

    const auto* count = find_field("LocalWidget", "count");
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(count->type, reflection::FieldType::Int);
    EXPECT_TRUE(count->has_range);

    LocalWidget w;
    ASSERT_TRUE(reflection::write_field(&w, *count, 7));
    EXPECT_EQ(w.count, 7);
}
