// 组件反射集中注册（M1-E1）
//
// 与 component_factory.cpp 的集中注册风格一致：组件头/实现零改动。
// 只注册编辑器有意义的公有值字段；跳过 GPU 句柄、unique_ptr、alive_token、
// 容器/嵌套结构等暂不支持字段。

#include "reflection/reflection.h"

#include "components/component.h"
#include "components/transform.h"
#include "components/node2d.h"
#include "components/node3d.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh_renderer.h"
#include "components/terrain.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/audio_source.h"
#include "components/audio_listener.h"
#include "components/rigid_body.h"
#include "components/static_body.h"
#include "components/box_collider.h"
#include "components/sphere_collider.h"
#include "components/plane_collider.h"
#include "components/character_controller_3d.h"
#include "components/physics_body.h"
#include "components/physical_material.h"
#include "components/fragment_body.h"
#include "components/destructible_body.h"
#include "components/prefab_instance.h"
#include "components/rigid_body_2d.h"
#include "components/static_body_2d.h"
#include "components/box_collider_2d.h"
#include "components/circle_collider_2d.h"
#include "components/character_controller_2d.h"
#include "components/joint_2d.h"
#include "components/joint_3d.h"
#include "components/2d/component_2d.h"
#include "components/2d/basic_rect.h"
#include "components/2d/label.h"
#include "components/2d/sprite_2d.h"
#include "components/2d/shape.h"
#include "components/2d/camera_2d.h"
#include "components/2d/light_2d.h"
#include "components/2d/ambient_light_2d.h"
#include "components/2d/particle_emitter.h"
#include "components/2d/parallax_background.h"
#include "components/2d/skybox_2d.h"
#include "components/2d/tilemap.h"

using namespace gryce_engine::components;

// 嵌套命名空间组件引入短名，便于宏注册（宏会把 Class token 字符串化）
using Component2D = gryce_engine::components::d2::Component2D;
using BasicRect = gryce_engine::components::d2::basic_rect::BasicRect;
using ColorRect = gryce_engine::components::d2::basic_rect::ColorRect;
using Label = gryce_engine::components::d2::text::Label;
using Sprite2D = gryce_engine::components::d2::sprite::Sprite2D;
using Circle = gryce_engine::components::d2::shape::Circle;
using Polygon = gryce_engine::components::d2::shape::Polygon;
using Camera2D = gryce_engine::components::d2::camera::Camera2D;
using Light2D = gryce_engine::components::d2::light::Light2D;
using AmbientLight2D = gryce_engine::components::d2::light::AmbientLight2D;
using ParticleEmitter2D = gryce_engine::components::d2::ParticleEmitter2D;
using ParallaxBackground = gryce_engine::components::d2::parallax::ParallaxBackground;
using Skybox2D = gryce_engine::components::d2::skybox::Skybox2D;
using Tilemap = gryce_engine::components::d2::tilemap::Tilemap;

// 基类：enabled 对所有组件经继承链可见
GRYCE_REFLECT_CLASS(Component, )
    GRYCE_REFLECT_FIELD(enabled)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Transform, Component)
    GRYCE_REFLECT_FIELD(position)
    GRYCE_REFLECT_FIELD(rotation)
    GRYCE_REFLECT_FIELD(scale)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Node2D, Component)
    GRYCE_REFLECT_FIELD(z_index)
    GRYCE_REFLECT_FIELD(top_level)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Node3D, Component)
    GRYCE_REFLECT_FIELD(visible)
GRYCE_REFLECT_END()

// ---------------------------------------------------------------------------
// 2D 渲染/UI 组件基类与派生类
// ---------------------------------------------------------------------------
GRYCE_REFLECT_CLASS(Component2D, Component)
    GRYCE_REFLECT_FIELD(render_order)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(BasicRect, Component2D)
    GRYCE_REFLECT_FIELD_RANGE(width, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(height, 0.0f, 10000.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(ColorRect, BasicRect)
    GRYCE_REFLECT_FIELD(color)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Label, Component2D)
    GRYCE_REFLECT_FIELD(text)
    GRYCE_REFLECT_FIELD_RANGE(font_size, 1.0f, 512.0f)
    GRYCE_REFLECT_FIELD(color)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Sprite2D, Component2D)
    GRYCE_REFLECT_FIELD(texture_path)
    GRYCE_REFLECT_FIELD(normal_map_path)
    GRYCE_REFLECT_FIELD(color)
    GRYCE_REFLECT_FIELD_RANGE(width, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(height, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(lit)
    GRYCE_REFLECT_FIELD(cast_shadow)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Circle, Component2D)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(segments)
    GRYCE_REFLECT_FIELD(color)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Polygon, Component2D)
    GRYCE_REFLECT_FIELD(color)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Camera2D, Component2D)
    GRYCE_REFLECT_FIELD(is_active)
    GRYCE_REFLECT_FIELD_RANGE(zoom, 0.01f, 100.0f)
    GRYCE_REFLECT_FIELD(offset)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Light2D, Component2D)
    GRYCE_REFLECT_FIELD_ENUM(light_type)
    GRYCE_REFLECT_FIELD(color)
    GRYCE_REFLECT_FIELD_RANGE(intensity, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(range, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(direction)
    GRYCE_REFLECT_FIELD_RANGE(spot_angle, 1.0f, 179.0f)
    GRYCE_REFLECT_FIELD_RANGE(spot_softness, 0.0f, 1.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(AmbientLight2D, Component2D)
    GRYCE_REFLECT_FIELD(color)
    GRYCE_REFLECT_FIELD_RANGE(intensity, 0.0f, 100.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(ParticleEmitter2D, Component2D)
    GRYCE_REFLECT_FIELD_RANGE(emission_rate, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(max_particles)
    GRYCE_REFLECT_FIELD(burst_min)
    GRYCE_REFLECT_FIELD(burst_max)
    GRYCE_REFLECT_FIELD_RANGE(lifetime_min, 0.0f, 60.0f)
    GRYCE_REFLECT_FIELD_RANGE(lifetime_max, 0.0f, 60.0f)
    GRYCE_REFLECT_FIELD_RANGE(velocity_min, -10000.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(velocity_max, -10000.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(direction_min, -3.14159f, 3.14159f)
    GRYCE_REFLECT_FIELD_RANGE(direction_max, -3.14159f, 3.14159f)
    GRYCE_REFLECT_FIELD(acceleration)
    GRYCE_REFLECT_FIELD(start_color)
    GRYCE_REFLECT_FIELD(end_color)
    GRYCE_REFLECT_FIELD_RANGE(start_size, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(end_size, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(rotation_min, -360.0f, 360.0f)
    GRYCE_REFLECT_FIELD_RANGE(rotation_max, -360.0f, 360.0f)
    GRYCE_REFLECT_FIELD_RANGE(angular_velocity_min, -3600.0f, 3600.0f)
    GRYCE_REFLECT_FIELD_RANGE(angular_velocity_max, -3600.0f, 3600.0f)
    GRYCE_REFLECT_FIELD(texture_path)
    GRYCE_REFLECT_FIELD(additive)
    GRYCE_REFLECT_FIELD(emission_offset)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(ParallaxBackground, Component2D)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Skybox2D, Component2D)
    GRYCE_REFLECT_FIELD(texture_path)
    GRYCE_REFLECT_FIELD(color)
    GRYCE_REFLECT_FIELD_RANGE(scroll_factor, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD(tile)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Tilemap, Component2D)
    GRYCE_REFLECT_FIELD(tileset_path)
    GRYCE_REFLECT_FIELD(map_width)
    GRYCE_REFLECT_FIELD(map_height)
    GRYCE_REFLECT_FIELD_RANGE(cell_width, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(cell_height, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(generate_colliders)
    GRYCE_REFLECT_FIELD(debug_draw_colliders)
    GRYCE_REFLECT_FIELD(use_tileset_texture)
    GRYCE_REFLECT_FIELD(lit)
    GRYCE_REFLECT_FIELD(cast_shadow)
GRYCE_REFLECT_END()

// ---------------------------------------------------------------------------
// 3D 渲染与Gameplay组件
// ---------------------------------------------------------------------------
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

GRYCE_REFLECT_CLASS(Terrain, Component)
    GRYCE_REFLECT_FIELD_RANGE(width, 1.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(depth, 1.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(resolution, 2, 512)
    GRYCE_REFLECT_FIELD_RANGE(height_scale, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD(base_texture_path)
    GRYCE_REFLECT_FIELD_RANGE(seed, 0, 1000000)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Camera, Component)
    GRYCE_REFLECT_FIELD_RANGE(fov, 1.0f, 179.0f)
    GRYCE_REFLECT_FIELD_RANGE(near_plane, 0.001f, 100.0f)
    GRYCE_REFLECT_FIELD(far_plane)
    GRYCE_REFLECT_FIELD(is_main)
    GRYCE_REFLECT_FIELD(background_color)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Light, Component)
    GRYCE_REFLECT_FIELD_ENUM(light_type)
    GRYCE_REFLECT_FIELD(color)
    GRYCE_REFLECT_FIELD_RANGE(intensity, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD(direction)
    GRYCE_REFLECT_FIELD_RANGE(range, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(spot_angle, 1.0f, 179.0f)
    GRYCE_REFLECT_FIELD_RANGE(spot_softness, 0.0f, 1.0f)
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

GRYCE_REFLECT_CLASS(AudioListener, Component)
    GRYCE_REFLECT_FIELD_RANGE(global_volume, 0.0f, 1.0f)
GRYCE_REFLECT_END()

// ---------------------------------------------------------------------------
// 3D 物理组件
// ---------------------------------------------------------------------------
GRYCE_REFLECT_CLASS(RigidBody, Component)
    GRYCE_REFLECT_FIELD_RANGE(mass, 0.001f, 10000.0f)
    GRYCE_REFLECT_FIELD(use_gravity)
    GRYCE_REFLECT_FIELD(is_kinematic)
    GRYCE_REFLECT_FIELD(velocity)
    GRYCE_REFLECT_FIELD(angular_velocity)
    GRYCE_REFLECT_FIELD_RANGE(linear_damping, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD_RANGE(angular_damping, 0.0f, 1.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(StaticBody, Component)
    GRYCE_REFLECT_FIELD(kinematic)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(BoxCollider, Component)
    GRYCE_REFLECT_FIELD(size)
    GRYCE_REFLECT_FIELD(center)
    GRYCE_REFLECT_FIELD(is_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(SphereCollider, Component)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD(center)
    GRYCE_REFLECT_FIELD(is_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(PlaneCollider, Component)
    GRYCE_REFLECT_FIELD(normal)
    GRYCE_REFLECT_FIELD(offset)
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
    GRYCE_REFLECT_FIELD_RANGE(push_recovery_speed, 0.0f, 100.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(PhysicsBody, Component)
    GRYCE_REFLECT_FIELD(simulate)
    GRYCE_REFLECT_FIELD(gravity)
    GRYCE_REFLECT_FIELD_RANGE(damping, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD(floor_y)
    GRYCE_REFLECT_FIELD_RANGE(restitution, 0.0f, 1.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(PhysicalMaterial, Component)
    GRYCE_REFLECT_FIELD(preset_name)
    GRYCE_REFLECT_FIELD_RANGE(softness, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD_RANGE(drag_coefficient, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD_RANGE(density, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(friction, 0.0f, 1.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(FragmentBody, Component)
    GRYCE_REFLECT_FIELD_RANGE(lifetime, 0.0f, 600.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(DestructibleBody, Component)
    GRYCE_REFLECT_FIELD_RANGE(fracture_threshold, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(explosive_impulse, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD(segments)
    GRYCE_REFLECT_FIELD(max_fragments)
    GRYCE_REFLECT_FIELD_RANGE(fragment_lifetime, 0.0f, 600.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(PrefabInstance, Component)
    GRYCE_REFLECT_FIELD(prefab_path)
    GRYCE_REFLECT_FIELD(root_template_uuid)
    GRYCE_REFLECT_FIELD_RO(variant_of)
GRYCE_REFLECT_END()

// ---------------------------------------------------------------------------
// 2D 物理组件
// ---------------------------------------------------------------------------
GRYCE_REFLECT_CLASS(RigidBody2D, Component)
    GRYCE_REFLECT_FIELD_RANGE(mass, 0.001f, 10000.0f)
    GRYCE_REFLECT_FIELD(use_gravity)
    GRYCE_REFLECT_FIELD(is_kinematic)
    GRYCE_REFLECT_FIELD(fixed_rotation)
    GRYCE_REFLECT_FIELD(velocity)
    GRYCE_REFLECT_FIELD(acceleration)
    GRYCE_REFLECT_FIELD_RANGE(restitution, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD_RANGE(friction, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD_RANGE(linear_damping, 0.0f, 1.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(StaticBody2D, Component)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(BoxCollider2D, Component)
    GRYCE_REFLECT_FIELD(size)
    GRYCE_REFLECT_FIELD(center)
    GRYCE_REFLECT_FIELD(is_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(CircleCollider2D, Component)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD(center)
    GRYCE_REFLECT_FIELD(is_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(CharacterController2D, Component)
    GRYCE_REFLECT_FIELD_RANGE(speed, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(jump_force, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD(ground_check_offset)
    GRYCE_REFLECT_FIELD_RANGE(ground_check_distance, 0.0f, 10.0f)
    GRYCE_REFLECT_FIELD_RANGE(ground_check_span, 0.0f, 10.0f)
    GRYCE_REFLECT_FIELD(fixed_rotation)
    GRYCE_REFLECT_FIELD_RANGE(slope_limit_degrees, 0.0f, 89.0f)
    GRYCE_REFLECT_FIELD_RANGE(step_height, 0.0f, 5.0f)
    GRYCE_REFLECT_FIELD_RANGE(push_recovery_speed, 0.0f, 100.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Joint2D, Component)
    GRYCE_REFLECT_FIELD_ENUM(joint_type)
    GRYCE_REFLECT_FIELD(anchor_a)
    GRYCE_REFLECT_FIELD(anchor_b)
    GRYCE_REFLECT_FIELD_RANGE(length, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(frequency, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(damping, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD(collide_connected)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Joint3D, Component)
    GRYCE_REFLECT_FIELD_ENUM(joint_type)
    GRYCE_REFLECT_FIELD(anchor_a)
    GRYCE_REFLECT_FIELD(anchor_b)
    GRYCE_REFLECT_FIELD(axis_a)
    GRYCE_REFLECT_FIELD(axis_b)
    GRYCE_REFLECT_FIELD_RANGE(length, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(frequency, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(damping, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD(collide_connected)
GRYCE_REFLECT_END()

namespace gryce_engine::reflection {

// 锚点：本 TU 被链接后，上述静态注册对象才会执行。
// 由 components::register_builtin_components() 调用。
void register_builtin_reflections() {
    // 触碰单例，语义上标记注册入口；真正的注册由本 TU 静态初始化完成
    (void)Registry::instance().type_count();
}

} // namespace gryce_engine::reflection
