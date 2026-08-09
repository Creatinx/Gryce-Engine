#include "component_factory.h"

#include "components/transform.h"
#include "components/prefab_instance.h"
#include "components/mesh_renderer.h"
#include "components/subviewport.h"
#include "components/skinned_mesh_renderer.h"
#include "components/terrain.h"
#include "components/physical_material.h"
#include "components/physics_body.h"
#include "components/node2d.h"
#include "components/node3d.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/static_body.h"
#include "components/rigid_body.h"
#include "components/box_collider.h"
#include "components/sphere_collider.h"
#include "components/plane_collider.h"
#include "components/static_body_2d.h"
#include "components/rigid_body_2d.h"
#include "components/box_collider_2d.h"
#include "components/circle_collider_2d.h"
#include "components/character_controller_2d.h"
#include "components/character_controller_3d.h"
#include "components/joint_2d.h"
#include "components/joint_3d.h"
#include "components/destructible_body.h"
#include "components/fragment_body.h"
#include "components/audio_source.h"
#include "components/audio_listener.h"
#include "components/2d/basic_rect.h"
#include "components/2d/shape.h"
#include "components/2d/label.h"
#include "components/2d/light_2d.h"
#include "components/2d/ambient_light_2d.h"
#include "components/2d/skybox_2d.h"
#include "components/2d/sprite_2d.h"
#include "components/2d/tilemap.h"
#include "components/2d/camera_2d.h"
#include "components/2d/parallax_background.h"
#include "components/2d/particle_emitter.h"
#include "reflection/reflection.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

void register_builtin_components() {
    // 强制链接反射注册 TU（静态库中无引用符号的 TU 会被链接器丢弃）
    reflection::register_builtin_reflections();

    auto& factory = ComponentFactory::instance();

    // 3D 渲染
    factory.register_type("MeshRenderer", []() { return std::make_unique<MeshRenderer>(); },
                          "渲染 3D 网格模型，支持 PBR 材质与阴影。");
    factory.register_type("SkinnedMeshRenderer", []() { return std::make_unique<SkinnedMeshRenderer>(); },
                          "渲染带骨骼蒙皮的 3D 网格，支持动画。");
    factory.register_type("Camera", []() { return std::make_unique<Camera>(); },
                          "定义 3D 相机参数（FOV、裁剪面、背景色）。");
    factory.register_type("Light", []() { return std::make_unique<Light>(); },
                          "3D 光源：方向光、点光源或聚光灯。");
    factory.register_type("SubViewport", []() { return std::make_unique<SubViewport>(); },
                          "运行时 3D→2D 视口：把场景渲染到纹理并注入 Sprite2D。");

    // 2D 节点与渲染
    factory.register_type("Node2D", []() { return std::make_unique<Node2D>(); },
                          "2D 节点的基类，提供 Z 索引等 2D 属性。");
    factory.register_type("Node3D", []() { return std::make_unique<Node3D>(); },
                          "3D 节点的基类，提供可见性开关。");
    factory.register_type("ColorRect", []() { return std::make_unique<d2::basic_rect::ColorRect>(); },
                          "填充指定颜色的矩形 2D 图形。");
    factory.register_type("Circle", []() { return std::make_unique<d2::shape::Circle>(); },
                          "填充圆形 2D 图形，可设置半径与分段数。");
    factory.register_type("Polygon", []() { return std::make_unique<d2::shape::Polygon>(); },
                          "自定义多边形 2D 图形。");
    factory.register_type("Label", []() { return std::make_unique<d2::text::Label>(); },
                          "显示文本的 2D UI 元素。");
    factory.register_type("Sprite2D", []() { return std::make_unique<d2::sprite::Sprite2D>(); },
                          "显示 2D 纹理精灵。");
    factory.register_type("Tilemap", []() { return std::make_unique<d2::tilemap::Tilemap>(); },
                          "基于瓦片集的 2D 地图。");
    factory.register_type("Camera2D", []() { return std::make_unique<d2::camera::Camera2D>(); },
                          "2D 场景相机，控制可视区域。");
    factory.register_type("ParallaxBackground", []() { return std::make_unique<d2::parallax::ParallaxBackground>(); },
                          "2D 视差滚动背景。");
    factory.register_type("ParticleEmitter2D", []() { return std::make_unique<d2::ParticleEmitter2D>(); },
                          "2D 粒子发射器，用于火焰、烟雾等效果。");
    factory.register_type("Light2D", []() { return std::make_unique<d2::light::Light2D>(); },
                          "2D 光源，照亮受光照的 2D 物体。");
    factory.register_type("AmbientLight2D", []() { return std::make_unique<d2::light::AmbientLight2D>(); },
                          "2D 环境光，影响所有受光照 2D 物体。");
    factory.register_type("Skybox2D", []() { return std::make_unique<d2::skybox::Skybox2D>(); },
                          "2D 场景的天空背景。");

    // 物理
    factory.register_type("StaticBody", []() { return std::make_unique<StaticBody>(); },
                          "3D 静态物理体，不会移动但可与其他物体碰撞。");
    factory.register_type("RigidBody", []() { return std::make_unique<RigidBody>(); },
                          "3D 刚体，受物理模拟影响。");
    factory.register_type("BoxCollider", []() { return std::make_unique<BoxCollider>(); },
                          "3D 盒状碰撞体。");
    factory.register_type("SphereCollider", []() { return std::make_unique<SphereCollider>(); },
                          "3D 球状碰撞体。");
    factory.register_type("PlaneCollider", []() { return std::make_unique<PlaneCollider>(); },
                          "3D 无限平面碰撞体。");
    factory.register_type("CharacterController3D", []() { return std::make_unique<CharacterController3D>(); },
                          "3D 角色控制器，提供跳跃、坡度限制等角色移动能力。");
    factory.register_type("StaticBody2D", []() { return std::make_unique<StaticBody2D>(); },
                          "2D 静态物理体。");
    factory.register_type("RigidBody2D", []() { return std::make_unique<RigidBody2D>(); },
                          "2D 刚体，受 2D 物理模拟影响。");
    factory.register_type("BoxCollider2D", []() { return std::make_unique<BoxCollider2D>(); },
                          "2D 盒状碰撞体。");
    factory.register_type("CircleCollider2D", []() { return std::make_unique<CircleCollider2D>(); },
                          "2D 圆形碰撞体。");
    factory.register_type("CharacterController2D", []() { return std::make_unique<CharacterController2D>(); },
                          "2D 角色控制器。");
    factory.register_type("Joint2D", []() { return std::make_unique<Joint2D>(); },
                          "2D 关节，用于连接两个 2D 刚体。");
    factory.register_type("Joint3D", []() { return std::make_unique<Joint3D>(); },
                          "3D 关节，用于连接两个 3D 刚体。");
    factory.register_type("PhysicalMaterial", []() { return std::make_unique<PhysicalMaterial>(); },
                          "物理材质预设，定义摩擦、弹性等。");
    factory.register_type("PhysicsBody", []() { return std::make_unique<PhysicsBody>(); },
                          "通用 3D 物理体基类。");
    factory.register_type("DestructibleBody", []() { return std::make_unique<DestructibleBody>(); },
                          "可破坏体，受到足够冲击后碎裂。");
    factory.register_type("FragmentBody", []() { return std::make_unique<FragmentBody>(); },
                          "碎片体，由可破坏体碎裂后生成。");

    // 音频
    factory.register_type("AudioSource", []() { return std::make_unique<AudioSource>(); },
                          "3D/2D 音频源，可播放音效与音乐。");
    factory.register_type("AudioListener", []() { return std::make_unique<AudioListener>(); },
                          "音频监听点，决定 3D 声音的空间效果。");

    // 地形与特殊
    factory.register_type("Terrain", []() { return std::make_unique<Terrain>(); },
                          "程序化地形，支持高度图与噪声生成。");

    // 内部组件（通常不应手动添加，但保留注册以保证反序列化与工厂完整性）
    factory.register_type("Transform", []() { return std::make_unique<Transform>(); },
                          "（内部）实体的位置、旋转、缩放。每个实体自动拥有。");
    factory.register_type("PrefabInstance", []() { return std::make_unique<PrefabInstance>(); },
                          "（内部）标识该实体为预制体实例。");
}

ComponentFactory& ComponentFactory::instance() {
    static ComponentFactory factory;
    return factory;
}

void ComponentFactory::register_type(const std::string& type, Creator creator) {
    register_type(type, std::move(creator), {});
}

void ComponentFactory::register_type(const std::string& type, Creator creator, const std::string& description) {
    if (!creators_.contains(type)) {
        type_order_.push_back(type);
    }
    creators_[type] = TypeInfo{std::move(creator), description};
}

std::unique_ptr<Component> ComponentFactory::create(const std::string& type) const {
    auto it = creators_.find(type);
    if (it != creators_.end()) {
        return it->second.creator();
    }
    GLOG_WARN("ComponentFactory: unknown component type '{}'", type);
    return nullptr;
}

bool ComponentFactory::has_type(const std::string& type) const {
    return creators_.find(type) != creators_.end();
}

std::vector<std::string> ComponentFactory::all_types() const {
    return type_order_;
}

const char* ComponentFactory::description(const std::string& type) const {
    auto it = creators_.find(type);
    if (it != creators_.end() && !it->second.description.empty()) {
        return it->second.description.c_str();
    }
    return "";
}

} // namespace gryce_engine::components
