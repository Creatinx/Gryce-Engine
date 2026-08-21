// 组件反射集中注册（M1-E1）
//
// 与 component_factory.cpp 的集中注册风格一致：组件头/实现零改动。
// 只注册编辑器有意义的公有值字段；跳过 GPU 句柄、unique_ptr、alive_token、
// 容器/嵌套结构等暂不支持字段。

#include "reflection/reflection.h"

#include "components/component.h"
#include "components/transform.h"
#include "components/script_component.h"
using ScriptComponent = gryce_engine::components::ScriptComponent;
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
#include "components/3d/visual_components.h"
#include "components/3d/colliders.h"
#include "components/3d/navigation_components.h"
#include "components/3d/system_components.h"
#include "components/2d/anim_components.h"
#include "components/2d/visual_components.h"
#include "components/2d/physics_components.h"
#include "components/2d/navigation_components.h"
#include "components/2d/misc_components.h"
#include "components/common/system_components.h"

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
using Animator = gryce_engine::components::Animator;
using ParticleSystem3D = gryce_engine::components::ParticleSystem3D;
using TrailRenderer = gryce_engine::components::TrailRenderer;
using LineRenderer3D = gryce_engine::components::LineRenderer3D;
using Decal = gryce_engine::components::Decal;
using Billboard = gryce_engine::components::Billboard;
using TextMesh3D = gryce_engine::components::TextMesh3D;
using Skybox3D = gryce_engine::components::Skybox3D;
using ReflectionProbe = gryce_engine::components::ReflectionProbe;
using LightProbeGroup = gryce_engine::components::LightProbeGroup;
using FogVolume = gryce_engine::components::FogVolume;
using VolumetricLight = gryce_engine::components::VolumetricLight;
using LODGroup = gryce_engine::components::LODGroup;
using InstancedMeshRenderer = gryce_engine::components::InstancedMeshRenderer;
using CapsuleCollider = gryce_engine::components::CapsuleCollider;
using CylinderCollider = gryce_engine::components::CylinderCollider;
using ConvexMeshCollider = gryce_engine::components::ConvexMeshCollider;
using MeshCollider = gryce_engine::components::MeshCollider;
using TriggerVolume = gryce_engine::components::TriggerVolume;
using WheelCollider = gryce_engine::components::WheelCollider;
using NavigationMesh3D = gryce_engine::components::NavigationMesh3D;
using NavMeshAgent3D = gryce_engine::components::NavMeshAgent3D;
using NavMeshObstacle3D = gryce_engine::components::NavMeshObstacle3D;
using RayCast3D = gryce_engine::components::RayCast3D;
using SpringArm3D = gryce_engine::components::SpringArm3D;
using VisibilityNotifier3D = gryce_engine::components::VisibilityNotifier3D;
using AudioReverbZone = gryce_engine::components::AudioReverbZone;
using Timer = gryce_engine::components::Timer;
using TweenPlayer = gryce_engine::components::TweenPlayer;
using AnimatedSprite2D = gryce_engine::components::d2::AnimatedSprite2D;
using Skeleton2D = gryce_engine::components::d2::Skeleton2D;
using Path2D = gryce_engine::components::d2::Path2D;
using PathFollow2D = gryce_engine::components::d2::PathFollow2D;
using NinePatchRect = gryce_engine::components::d2::NinePatchRect;
using LightOccluder2D = gryce_engine::components::d2::LightOccluder2D;
using PolygonCollider2D = gryce_engine::components::d2::PolygonCollider2D;
using CapsuleCollider2D = gryce_engine::components::d2::CapsuleCollider2D;
using EdgeCollider2D = gryce_engine::components::d2::EdgeCollider2D;
using TileMapCollider = gryce_engine::components::d2::TileMapCollider;
using Area2D = gryce_engine::components::d2::Area2D;
using RayCast2D = gryce_engine::components::d2::RayCast2D;
using HingeJoint2D = gryce_engine::components::d2::HingeJoint2D;
using WeldJoint2D = gryce_engine::components::d2::WeldJoint2D;
using PrismaticJoint2D = gryce_engine::components::d2::PrismaticJoint2D;
using WheelJoint2D = gryce_engine::components::d2::WheelJoint2D;
using RopeJoint2D = gryce_engine::components::d2::RopeJoint2D;
using NavigationRegion2D = gryce_engine::components::d2::NavigationRegion2D;
using NavigationAgent2D = gryce_engine::components::d2::NavigationAgent2D;
using Marker2D = gryce_engine::components::d2::Marker2D;
using VisibilityNotifier2D = gryce_engine::components::d2::VisibilityNotifier2D;

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
    GRYCE_REFLECT_FIELD(billboard)
    GRYCE_REFLECT_FIELD(depth_test)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(SkinnedMeshRenderer, Component)
    GRYCE_REFLECT_FIELD(model_path)
    GRYCE_REFLECT_FIELD(clip_name)
    GRYCE_REFLECT_FIELD(playing)
    GRYCE_REFLECT_FIELD(loop)
    GRYCE_REFLECT_FIELD_RANGE(speed, 0.0f, 4.0f)
    GRYCE_REFLECT_FIELD(time)
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
    GRYCE_REFLECT_FIELD_RANGE(speed, 0.1f, 4.0f)
    GRYCE_REFLECT_FIELD(loop)
    GRYCE_REFLECT_FIELD(play_on_awake)
    GRYCE_REFLECT_FIELD(is_3d)
    GRYCE_REFLECT_FIELD_RANGE(min_distance, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(max_distance, 0.0f, 10000.0f)
GRYCE_REFLECT_END()
GRYCE_REFLECT_CLASS(AudioListener, Component)
    GRYCE_REFLECT_FIELD_RANGE(global_volume, 0.0f, 1.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(ScriptComponent, Component)
    GRYCE_REFLECT_FIELD(script_path)
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
    GRYCE_REFLECT_FIELD_RANGE(restitution, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD_RANGE(friction, 0.0f, 1.0f)
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

// ---------------------------------------------------------------------------
// 新增 3D 视觉组件
// ---------------------------------------------------------------------------
GRYCE_REFLECT_CLASS(Animator, Component)
    GRYCE_REFLECT_FIELD(clip_name)
    GRYCE_REFLECT_FIELD(playing)
    GRYCE_REFLECT_FIELD(loop)
    GRYCE_REFLECT_FIELD_RANGE(speed, 0.0f, 4.0f)
    GRYCE_REFLECT_FIELD(time)
    GRYCE_REFLECT_FIELD_RANGE(blend_duration, 0.0f, 10.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(ParticleSystem3D, Component)
    GRYCE_REFLECT_FIELD(texture_path)
    GRYCE_REFLECT_FIELD(loop)
    GRYCE_REFLECT_FIELD(play_on_awake)
    GRYCE_REFLECT_FIELD_RANGE(max_particles, 1, 100000)
    GRYCE_REFLECT_FIELD_RANGE(emission_rate, 0.0f, 100000.0f)
    GRYCE_REFLECT_FIELD_RANGE(lifetime_min, 0.0f, 60.0f)
    GRYCE_REFLECT_FIELD_RANGE(lifetime_max, 0.0f, 60.0f)
    GRYCE_REFLECT_FIELD_RANGE(speed_min, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(speed_max, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(start_size, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(end_size, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD(start_color)
    GRYCE_REFLECT_FIELD(end_color)
    GRYCE_REFLECT_FIELD(additive)
    GRYCE_REFLECT_FIELD(emission_offset)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(TrailRenderer, Component)
    GRYCE_REFLECT_FIELD_RANGE(lifetime, 0.0f, 60.0f)
    GRYCE_REFLECT_FIELD_RANGE(min_vertex_distance, 0.0f, 10.0f)
    GRYCE_REFLECT_FIELD_RANGE(width, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD(color)
    GRYCE_REFLECT_FIELD(autodestruct)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(LineRenderer3D, Component)
    GRYCE_REFLECT_FIELD(loop)
    GRYCE_REFLECT_FIELD_RANGE(width, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD(color)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Decal, Component)
    GRYCE_REFLECT_FIELD(texture_path)
    GRYCE_REFLECT_FIELD(size)
    GRYCE_REFLECT_FIELD_RANGE(opacity, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD(fade_edges)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Billboard, Component)
    GRYCE_REFLECT_FIELD(texture_path)
    GRYCE_REFLECT_FIELD(lock_x_axis)
    GRYCE_REFLECT_FIELD(size)
    GRYCE_REFLECT_FIELD_RANGE(opacity, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD(shaded)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(TextMesh3D, Component)
    GRYCE_REFLECT_FIELD(text)
    GRYCE_REFLECT_FIELD(font_path)
    GRYCE_REFLECT_FIELD_RANGE(font_size, 0.0f, 512.0f)
    GRYCE_REFLECT_FIELD_RANGE(pixel_height, 0.001f, 100.0f)
    GRYCE_REFLECT_FIELD(color)
    GRYCE_REFLECT_FIELD(double_sided)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Skybox3D, Component)
    GRYCE_REFLECT_FIELD(texture_path)
    GRYCE_REFLECT_FIELD(environment_path)
    GRYCE_REFLECT_FIELD_RANGE(exposure, 0.0f, 8.0f)
    GRYCE_REFLECT_FIELD(visible)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(ReflectionProbe, Component)
    GRYCE_REFLECT_FIELD_RANGE(resolution, 16, 2048)
    GRYCE_REFLECT_FIELD(box_extents)
    GRYCE_REFLECT_FIELD_RANGE(intensity, 0.0f, 8.0f)
    GRYCE_REFLECT_FIELD(realtime)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(LightProbeGroup, Component)
    GRYCE_REFLECT_FIELD_RANGE(grid_x, 1, 16)
    GRYCE_REFLECT_FIELD_RANGE(grid_y, 1, 16)
    GRYCE_REFLECT_FIELD_RANGE(grid_z, 1, 16)
    GRYCE_REFLECT_FIELD(size)
    GRYCE_REFLECT_FIELD_RANGE(intensity, 0.0f, 8.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(FogVolume, Component)
    GRYCE_REFLECT_FIELD(color)
    GRYCE_REFLECT_FIELD_RANGE(density, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD_RANGE(height_falloff, 0.0f, 10.0f)
    GRYCE_REFLECT_FIELD(size)
    GRYCE_REFLECT_FIELD(volumetric)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(VolumetricLight, Component)
    GRYCE_REFLECT_FIELD_RANGE(intensity, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(range, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(color)
    GRYCE_REFLECT_FIELD_RANGE(steps, 1, 128)
    GRYCE_REFLECT_FIELD_RANGE(jitter, 0.0f, 1.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(LODGroup, Component)
    GRYCE_REFLECT_FIELD_RANGE(transition_duration, 0.0f, 10.0f)
    GRYCE_REFLECT_FIELD_RANGE(active_lod, 0, 16)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(InstancedMeshRenderer, Component)
    GRYCE_REFLECT_FIELD(mesh_path)
    GRYCE_REFLECT_FIELD(material_path)
    GRYCE_REFLECT_FIELD_RANGE(instance_count, 1, 1000000)
    GRYCE_REFLECT_FIELD_RANGE(spacing, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(seed, 0, 1000000)
GRYCE_REFLECT_END()

// ---------------------------------------------------------------------------
// 新增 3D 碰撞体
// ---------------------------------------------------------------------------
GRYCE_REFLECT_CLASS(CapsuleCollider, Component)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(height, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(center)
    GRYCE_REFLECT_FIELD(is_trigger)
    GRYCE_REFLECT_FIELD_RANGE(direction, 0, 2)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(CylinderCollider, Component)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(height, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(center)
    GRYCE_REFLECT_FIELD(is_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(ConvexMeshCollider, Component)
    GRYCE_REFLECT_FIELD(mesh_path)
    GRYCE_REFLECT_FIELD(center)
    GRYCE_REFLECT_FIELD(is_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(MeshCollider, Component)
    GRYCE_REFLECT_FIELD(mesh_path)
    GRYCE_REFLECT_FIELD(is_trigger)
    GRYCE_REFLECT_FIELD(convex)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(TriggerVolume, Component)
    GRYCE_REFLECT_FIELD(is_box)
    GRYCE_REFLECT_FIELD(size)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(center)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(WheelCollider, Component)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(suspension_rest_length, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(suspension_stiffness, 0.0f, 100000.0f)
    GRYCE_REFLECT_FIELD_RANGE(mass, 0.001f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(friction, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD_RANGE(motor_torque, -100000.0f, 100000.0f)
    GRYCE_REFLECT_FIELD_RANGE(brake_torque, 0.0f, 100000.0f)
    GRYCE_REFLECT_FIELD_RANGE(steer_angle, -180.0f, 180.0f)
GRYCE_REFLECT_END()

// ---------------------------------------------------------------------------
// 新增 3D 寻路与系统级
// ---------------------------------------------------------------------------
GRYCE_REFLECT_CLASS(NavigationMesh3D, Component)
    GRYCE_REFLECT_FIELD(navmesh_path)
    GRYCE_REFLECT_FIELD(cell_size)
    GRYCE_REFLECT_FIELD_RANGE(agent_radius, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(agent_height, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(agent_max_slope, 0.0f, 89.0f)
    GRYCE_REFLECT_FIELD(bake_dynamic)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(NavMeshAgent3D, Component)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(height, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(speed, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(angular_speed, 0.0f, 3600.0f)
    GRYCE_REFLECT_FIELD(avoidance_enabled)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(NavMeshObstacle3D, Component)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(height, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD(carve)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(RayCast3D, Component)
    GRYCE_REFLECT_FIELD(direction)
    GRYCE_REFLECT_FIELD_RANGE(max_distance, 0.0f, 100000.0f)
    GRYCE_REFLECT_FIELD(ignore_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(SpringArm3D, Component)
    GRYCE_REFLECT_FIELD_RANGE(spring_length, 0.0f, 1000.0f)
    GRYCE_REFLECT_FIELD_RANGE(collision_margin, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(lerp_speed, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD(collide_with_world)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(VisibilityNotifier3D, Component)
    GRYCE_REFLECT_FIELD(size)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(AudioReverbZone, Component)
    GRYCE_REFLECT_FIELD_RANGE(min_distance, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(max_distance, 0.0f, 100000.0f)
    GRYCE_REFLECT_FIELD_RANGE(reverb_level, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD_RANGE(decay_time, 0.0f, 10.0f)
    GRYCE_REFLECT_FIELD_RANGE(density, 0.0f, 1.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Timer, Component)
    GRYCE_REFLECT_FIELD_RANGE(wait_time, 0.0f, 100000.0f)
    GRYCE_REFLECT_FIELD(one_shot)
    GRYCE_REFLECT_FIELD(auto_start)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(TweenPlayer, Component)
    GRYCE_REFLECT_FIELD(from)
    GRYCE_REFLECT_FIELD(to)
    GRYCE_REFLECT_FIELD_RANGE(duration, 0.0f, 100000.0f)
    GRYCE_REFLECT_FIELD(playing)
    GRYCE_REFLECT_FIELD(loop)
    GRYCE_REFLECT_FIELD(tween_scale)
    GRYCE_REFLECT_FIELD_RANGE(easing, 0, 1)
GRYCE_REFLECT_END()

// ---------------------------------------------------------------------------
// 新增 2D 组件
// ---------------------------------------------------------------------------
GRYCE_REFLECT_CLASS(AnimatedSprite2D, Component2D)
    GRYCE_REFLECT_FIELD(texture_path)
    GRYCE_REFLECT_FIELD(playing)
    GRYCE_REFLECT_FIELD(loop)
    GRYCE_REFLECT_FIELD_RANGE(fps, 0.0f, 240.0f)
    GRYCE_REFLECT_FIELD_RANGE(frame_count, 1, 100000)
    GRYCE_REFLECT_FIELD_RANGE(frame_width, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(frame_height, 0.0f, 10000.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Skeleton2D, Component2D)
    GRYCE_REFLECT_FIELD(skeleton_path)
    GRYCE_REFLECT_FIELD(animation_name)
    GRYCE_REFLECT_FIELD(playing)
    GRYCE_REFLECT_FIELD_RANGE(speed, 0.0f, 10.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Path2D, Component)
    GRYCE_REFLECT_FIELD(closed)
    GRYCE_REFLECT_FIELD(curve_smooth)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(PathFollow2D, Component)
    GRYCE_REFLECT_FIELD_RANGE(progress, 0.0f, 1.0f)
    GRYCE_REFLECT_FIELD(loop)
    GRYCE_REFLECT_FIELD_RANGE(speed, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD(rotate)
    GRYCE_REFLECT_FIELD_RANGE(offset, -1.0f, 1.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(NinePatchRect, Component2D)
    GRYCE_REFLECT_FIELD(texture_path)
    GRYCE_REFLECT_FIELD_RANGE(left_margin, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(right_margin, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(top_margin, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(bottom_margin, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(size)
    GRYCE_REFLECT_FIELD(modulate)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(LightOccluder2D, Component2D)
    GRYCE_REFLECT_FIELD(visible)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(PolygonCollider2D, Component2D)
    GRYCE_REFLECT_FIELD(offset)
    GRYCE_REFLECT_FIELD(is_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(CapsuleCollider2D, Component2D)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(height, 0.0f, 100000.0f)
    GRYCE_REFLECT_FIELD(offset)
    GRYCE_REFLECT_FIELD(is_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(EdgeCollider2D, Component2D)
    GRYCE_REFLECT_FIELD(p1)
    GRYCE_REFLECT_FIELD(p2)
    GRYCE_REFLECT_FIELD(is_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(TileMapCollider, Component2D)
    GRYCE_REFLECT_FIELD(one_way)
    GRYCE_REFLECT_FIELD(is_trigger)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Area2D, Component2D)
    GRYCE_REFLECT_FIELD(is_box)
    GRYCE_REFLECT_FIELD(size)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD(monitorable)
    GRYCE_REFLECT_FIELD(monitor)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(RayCast2D, Component2D)
    GRYCE_REFLECT_FIELD(direction)
    GRYCE_REFLECT_FIELD_RANGE(max_distance, 0.0f, 100000.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(HingeJoint2D, Component)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(WeldJoint2D, Component)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(PrismaticJoint2D, Component)
    GRYCE_REFLECT_FIELD(axis)
    GRYCE_REFLECT_FIELD(enable_limit)
    GRYCE_REFLECT_FIELD_RANGE(lower_translation, -100000.0f, 100000.0f)
    GRYCE_REFLECT_FIELD_RANGE(upper_translation, -100000.0f, 100000.0f)
    GRYCE_REFLECT_FIELD(enable_motor)
    GRYCE_REFLECT_FIELD_RANGE(motor_speed, -100000.0f, 100000.0f)
    GRYCE_REFLECT_FIELD_RANGE(max_motor_force, 0.0f, 1000000.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(WheelJoint2D, Component)
    GRYCE_REFLECT_FIELD(axis)
    GRYCE_REFLECT_FIELD(enable_motor)
    GRYCE_REFLECT_FIELD_RANGE(motor_speed, -100000.0f, 100000.0f)
    GRYCE_REFLECT_FIELD_RANGE(max_motor_torque, 0.0f, 1000000.0f)
    GRYCE_REFLECT_FIELD_RANGE(frequency, 0.0f, 100.0f)
    GRYCE_REFLECT_FIELD_RANGE(damping, 0.0f, 1.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(RopeJoint2D, Component)
    GRYCE_REFLECT_FIELD_RANGE(length, 0.0f, 100000.0f)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(NavigationRegion2D, Component2D)
    GRYCE_REFLECT_FIELD(navmesh_path)
    GRYCE_REFLECT_FIELD(bake_navigation)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(NavigationAgent2D, Component2D)
    GRYCE_REFLECT_FIELD_RANGE(radius, 0.0f, 10000.0f)
    GRYCE_REFLECT_FIELD_RANGE(speed, 0.0f, 100000.0f)
    GRYCE_REFLECT_FIELD(avoidance_enabled)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(Marker2D, Component2D)
    GRYCE_REFLECT_FIELD(marker_name)
    GRYCE_REFLECT_FIELD(show_in_editor)
GRYCE_REFLECT_END()

GRYCE_REFLECT_CLASS(VisibilityNotifier2D, Component2D)
    GRYCE_REFLECT_FIELD(size)
GRYCE_REFLECT_END()

namespace gryce_engine::reflection {

// 锚点：本 TU 被链接后，上述静态注册对象才会执行。
// 由 components::register_builtin_components() 调用。
void register_builtin_reflections() {
    // 触碰单例，语义上标记注册入口；真正的注册由本 TU 静态初始化完成
    (void)Registry::instance().type_count();
}

} // namespace gryce_engine::reflection
