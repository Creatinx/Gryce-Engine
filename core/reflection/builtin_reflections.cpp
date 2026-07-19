// 组件反射集中注册（M1-E1 首批）
//
// 与 component_factory.cpp 的集中注册风格一致：组件头/实现零改动。
// 只注册编辑器有意义的公有值字段；跳过 GPU 句柄、unique_ptr、alive_token、
// enum（本轮 FieldType 无 enum）等运行时/暂不支持字段。

#include "reflection/reflection.h"

#include "components/component.h"
#include "components/transform.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/audio_source.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/character_controller_3d.h"
#include "components/2d/sprite_2d.h"

using namespace gryce_engine::components;

// 基类：enabled 对所有组件经继承链可见
GRYCE_REFLECT_CLASS(Component, )
    GRYCE_REFLECT_FIELD(enabled)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Transform, Component)
    GRYCE_REFLECT_FIELD(position)
    GRYCE_REFLECT_FIELD(rotation)
    GRYCE_REFLECT_FIELD(scale)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(MeshRenderer, Component)
    GRYCE_REFLECT_FIELD(mesh_path)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(SkinnedMeshRenderer, Component)
    GRYCE_REFLECT_FIELD(model_path)
    GRYCE_REFLECT_FIELD(clip_name)
    GRYCE_REFLECT_FIELD(playing)
    GRYCE_REFLECT_FIELD(loop)
    GRYCE_REFLECT_FIELD_RANGE(speed, 0.0f, 4.0f)
    GRYCE_REFLECT_FIELD_RO(time)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Camera, Component)
    GRYCE_REFLECT_FIELD_RANGE(fov, 1.0f, 179.0f)
    GRYCE_REFLECT_FIELD_RANGE(near_plane, 0.001f, 100.0f)
    GRYCE_REFLECT_FIELD(far_plane)
    GRYCE_REFLECT_FIELD(is_main)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Light, Component)
    GRYCE_REFLECT_FIELD(color)
    GRYCE_REFLECT_FIELD_RANGE(intensity, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD(direction)
    GRYCE_REFLECT_FIELD_RANGE(range, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(spot_angle, 1.0f, 179.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(AudioSource, Component)
    GRYCE_REFLECT_FIELD(clip_path)
    GRYCE_REFLECT_FIELD_RANGE(volume, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD_RANGE(pitch, 0.1f, 4.0f)
    GRYCE_REFLECT_FIELD(loop)
    GRYCE_REFLECT_FIELD(play_on_awake)
    GRYCE_REFLECT_FIELD(is_3d)
    GRYCE_REFLECT_FIELD_RANGE(min_distance, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(max_distance, 0.0f, 10000.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(RigidBody, Component)
    GRYCE_REFLECT_FIELD_RANGE(mass, 0.001f, 10000.0f)
    GRYCE_REFLECT_FIELD(use_gravity)
    GRYCE_REFLECT_FIELD(is_kinematic)
    GRYCE_REFLECT_FIELD(velocity)
    GRYCE_REFLECT_FIELD(angular_velocity)
    GRYCE_REFLECT_FIELD_RANGE(linear_damping, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD_RANGE(angular_damping, 0.0f, 1.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(BoxCollider, Component)
    GRYCE_REFLECT_FIELD(size)
    GRYCE_REFLECT_FIELD(center)
    GRYCE_REFLECT_FIELD(is_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(CharacterController3D, Component)
    GRYCE_REFLECT_FIELD_RANGE(speed, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(jump_force, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD(ground_check_offset)
    GRYCE_REFLECT_FIELD_RANGE(ground_check_distance, 0.0f, 10.0f)
    GRYCE_REFLECT_FIELD_RANGE(ground_check_radius, 0.0f, 10.0f)
    GRYCE_REFLECT_FIELD(fixed_rotation)
    GRYCE_REFLECT_FIELD_RANGE(slope_limit_degrees, 0.0f, 89.0f)
    GRYCE_REFLECT_FIELD_RANGE(step_height, 0.0f, 5.0f)
GRYCE_REFLECT_END()

// Sprite2D 的 C++ 直接基类是 Component2D（本轮未注册其字段，
// 继承链到 Component2D 即停，自身字段不受影响）。
// 宏作用域名依赖 Class token，嵌套命名空间类先引入短名再注册。
using gryce_engine::components::d2::sprite::Sprite2D;

GRYCE_REFLECT_CLASS(Sprite2D, Component2D)
    GRYCE_REFLECT_FIELD(texture_path)
    GRYCE_REFLECT_FIELD(normal_map_path)
    GRYCE_REFLECT_FIELD_RANGE(width, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(height, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(lit)
    GRYCE_REFLECT_FIELD(cast_shadow)
GRYCE_REFLECT_END()

namespace gryce_engine::reflection {

// 锚点：本 TU 被链接后，上述静态注册对象才会执行。
// 由 components::register_builtin_components() 调用。
void register_builtin_reflections() {
    // 触碰单例，语义上标记注册入口；真正的注册由本 TU 静态初始化完成
    (void)Registry::instance().type_count();
}

} // namespace gryce_engine::reflection
