#include "component_factory.h"

#include "components/transform.h"
#include "components/prefab_instance.h"
#include "components/mesh_renderer.h"
#include "components/subviewport.h"
#include "components/skinned_mesh_renderer.h"
#include "components/terrain.h"
#include "components/node2d.h"
#include "components/node3d.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/hierarchy_components.h"
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

// 新增组件（3D / 2D / 系统级）
#include "components/3d/visual_components.h"
#include "components/3d/system_components.h"
#include "components/2d/anim_components.h"
#include "components/2d/visual_components.h"
#include "components/2d/misc_components.h"
#include "components/common/system_components.h"

#include "reflection/reflection.h"
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

namespace {

// 分类常量：Create Entity 对话框按 Node3D / Node2D / Other 分组
constexpr const char* k_cat_3d = "Node3D";
constexpr const char* k_cat_2d = "Node2D";
constexpr const char* k_cat_other = "Other";

} // namespace

void register_builtin_components() {
    // 强制链接反射注册 TU（静态库中无引用符号的 TU 会被链接器丢弃）
    reflection::register_builtin_reflections();

    auto& factory = ComponentFactory::instance();

    // 3D 渲染
    factory.register_type("MeshRenderer", []() { return std::make_unique<MeshRenderer>(); },
                          "渲染 3D 网格模型，支持 PBR 材质与阴影。", k_cat_3d);
    factory.register_type("SkinnedMeshRenderer", []() { return std::make_unique<SkinnedMeshRenderer>(); },
                          "渲染带骨骼蒙皮的 3D 网格，支持动画。", k_cat_3d);
    factory.register_type("Camera", []() { return std::make_unique<Camera>(); },
                          "定义 3D 相机参数（FOV、裁剪面、背景色）。", k_cat_3d);
    factory.register_type("Light", []() { return std::make_unique<Light>(); },
                          "3D 光源：方向光、点光源或聚光灯。", k_cat_3d);
    factory.register_type("SubViewport", []() { return std::make_unique<SubViewport>(); },
                          "运行时 3D→2D 视口：把场景渲染到纹理并注入 Sprite2D。", k_cat_3d);

    // 2D 节点与渲染
    factory.register_type("Node2D", []() { return std::make_unique<Node2D>(); },
                          "2D 节点的基类，提供 Z 索引等 2D 属性。", k_cat_2d);
    factory.register_type("Node3D", []() { return std::make_unique<Node3D>(); },
                          "3D 节点的基类，提供可见性开关。", k_cat_3d);
    factory.register_type("ColorRect", []() { return std::make_unique<d2::basic_rect::ColorRect>(); },
                          "填充指定颜色的矩形 2D 图形。", k_cat_2d);
    factory.register_type("Circle", []() { return std::make_unique<d2::shape::Circle>(); },
                          "填充圆形 2D 图形，可设置半径与分段数。", k_cat_2d);
    factory.register_type("Polygon", []() { return std::make_unique<d2::shape::Polygon>(); },
                          "自定义多边形 2D 图形。", k_cat_2d);
    factory.register_type("Label", []() { return std::make_unique<d2::text::Label>(); },
                          "显示文本的 2D UI 元素。", k_cat_2d);
    factory.register_type("Sprite2D", []() { return std::make_unique<d2::sprite::Sprite2D>(); },
                          "显示 2D 纹理精灵。", k_cat_2d);
    factory.register_type("Tilemap", []() { return std::make_unique<d2::tilemap::Tilemap>(); },
                          "基于瓦片集的 2D 地图。", k_cat_2d);
    factory.register_type("Camera2D", []() { return std::make_unique<d2::camera::Camera2D>(); },
                          "2D 场景相机，控制可视区域。", k_cat_2d);
    factory.register_type("ParallaxBackground", []() { return std::make_unique<d2::parallax::ParallaxBackground>(); },
                          "2D 视差滚动背景。", k_cat_2d);
    factory.register_type("ParticleEmitter2D", []() { return std::make_unique<d2::ParticleEmitter2D>(); },
                          "2D 粒子发射器，用于火焰、烟雾等效果。", k_cat_2d);
    factory.register_type("Light2D", []() { return std::make_unique<d2::light::Light2D>(); },
                          "2D 光源，照亮受光照的 2D 物体。", k_cat_2d);
    factory.register_type("AmbientLight2D", []() { return std::make_unique<d2::light::AmbientLight2D>(); },
                          "2D 环境光，影响所有受光照 2D 物体。", k_cat_2d);
    factory.register_type("Skybox2D", []() { return std::make_unique<d2::skybox::Skybox2D>(); },
                          "2D 场景的天空背景。", k_cat_2d);

    // 地形与特殊
    factory.register_type("Terrain", []() { return std::make_unique<Terrain>(); },
                          "程序化地形，支持高度图与噪声生成。", k_cat_3d);

    // 内部组件（通常不应手动添加，但保留注册以保证反序列化与工厂完整性）
    factory.register_type("Transform", []() { return std::make_unique<Transform>(); },
                          "（内部）实体的位置、旋转、缩放。每个实体自动拥有。", k_cat_other);
    factory.register_type("ParentComponent", []() { return std::make_unique<ParentComponent>(); },
                          "（内部）存储父实体 ECS ID。每个实体自动拥有。", k_cat_other);
    factory.register_type("ChildrenComponent", []() { return std::make_unique<ChildrenComponent>(); },
                          "（内部）存储子实体 ECS ID 列表。每个实体自动拥有。", k_cat_other);
    factory.register_type("PrefabInstance", []() { return std::make_unique<PrefabInstance>(); },
                          "（内部）标识该实体为预制体实例。", k_cat_other);

    // =====================================================================
    // 新增 3D 组件（20）
    // =====================================================================
    factory.register_type("Animator", []() { return std::make_unique<Animator>(); },
                          "动画状态机组件，驱动 SkinnedMeshRenderer 的片段切换与混合。", k_cat_3d);
    factory.register_type("ParticleSystem3D", []() { return std::make_unique<ParticleSystem3D>(); },
                          "GPU 3D 粒子：火焰、烟雾、魔法、瀑布。", k_cat_3d);
    factory.register_type("TrailRenderer", []() { return std::make_unique<TrailRenderer>(); },
                          "3D 拖尾：弹道、飞船尾焰、刀光。", k_cat_3d);
    factory.register_type("LineRenderer3D", []() { return std::make_unique<LineRenderer3D>(); },
                          "3D 线段/折线：技能指示线、绳子、调试绘制。", k_cat_3d);
    factory.register_type("Decal", []() { return std::make_unique<Decal>(); },
                          "贴花投影：弹孔、血迹、地面标识。", k_cat_3d);
    factory.register_type("Billboard", []() { return std::make_unique<Billboard>(); },
                          "广告牌：血条、公告、草丛，始终面向相机。", k_cat_3d);
    factory.register_type("TextMesh3D", []() { return std::make_unique<TextMesh3D>(); },
                          "3D 世界空间文本。", k_cat_3d);
    factory.register_type("Skybox3D", []() { return std::make_unique<Skybox3D>(); },
                          "3D 天空盒组件，接入现有 IBL 环境贴图。", k_cat_3d);
    factory.register_type("ReflectionProbe", []() { return std::make_unique<ReflectionProbe>(); },
                          "反射探针，静态场景反射（接入现有 IBLGenerator）。", k_cat_3d);
    factory.register_type("LightProbeGroup", []() { return std::make_unique<LightProbeGroup>(); },
                          "光照探针组，动态物体间接光插值。", k_cat_3d);
    factory.register_type("FogVolume", []() { return std::make_unique<FogVolume>(); },
                          "体积雾区域（局部雾）。", k_cat_3d);
    factory.register_type("VolumetricLight", []() { return std::make_unique<VolumetricLight>(); },
                          "体积光柱 / God Rays。", k_cat_3d);
    factory.register_type("LODGroup", []() { return std::make_unique<LODGroup>(); },
                          "远近自动切换模型细节层级。", k_cat_3d);
    factory.register_type("InstancedMeshRenderer", []() { return std::make_unique<InstancedMeshRenderer>(); },
                          "GPU 实例化渲染：植被、碎石、人群。", k_cat_3d);

    // =====================================================================
    // 新增 2D 组件（20）
    // =====================================================================
    factory.register_type("AnimatedSprite2D", []() { return std::make_unique<d2::AnimatedSprite2D>(); },
                          "序列帧动画精灵（图集/逐帧播放）。", k_cat_2d);
    factory.register_type("NinePatchRect", []() { return std::make_unique<d2::NinePatchRect>(); },
                          "九宫格图像：对话框、按钮底，任意缩放不变形。", k_cat_2d);
    factory.register_type("LightOccluder2D", []() { return std::make_unique<d2::LightOccluder2D>(); },
                          "2D 光遮挡/阴影投射体，配合 Light2D 阴影系统。", k_cat_2d);
    factory.register_type("Skeleton2D", []() { return std::make_unique<d2::Skeleton2D>(); },
                          "2D 骨骼动画（Spine 风格）。", k_cat_2d);
    factory.register_type("Path2D", []() { return std::make_unique<d2::Path2D>(); },
                          "2D 路径曲线，供敌人巡逻、轨道移动。", k_cat_2d);
    factory.register_type("PathFollow2D", []() { return std::make_unique<d2::PathFollow2D>(); },
                          "路径跟随者，沿父级 Path2D 移动。", k_cat_2d);
    factory.register_type("Marker2D", []() { return std::make_unique<d2::Marker2D>(); },
                          "标记点：出生点、挂点、编辑器辅助。", k_cat_2d);

    // =====================================================================
    // 新增：3D 寻路与系统级（10）
    // =====================================================================
    factory.register_type("Timer", []() { return std::make_unique<Timer>(); },
                          "通用计时器组件（2D/3D 通用）。", k_cat_other);
    factory.register_type("TweenPlayer", []() { return std::make_unique<TweenPlayer>(); },
                          "简单缓动动画组件（Transform 位置/缩放）。", k_cat_other);
    factory.register_type("VisibilityNotifier2D", []() { return std::make_unique<d2::VisibilityNotifier2D>(); },
                          "2D 屏幕可见通知（对象池/剔除）。", k_cat_2d);
    factory.register_type("VisibilityNotifier3D", []() { return std::make_unique<VisibilityNotifier3D>(); },
                          "3D 屏幕可见通知（对象池/剔除）。", k_cat_3d);
}

ComponentFactory& ComponentFactory::instance() {
    static ComponentFactory factory;
    return factory;
}

void ComponentFactory::register_type(const std::string& type, Creator creator) {
    register_type(type, std::move(creator), {}, "Other");
}

void ComponentFactory::register_type(const std::string& type, Creator creator, const std::string& description) {
    register_type(type, std::move(creator), description, "Other");
}

void ComponentFactory::register_type(const std::string& type, Creator creator,
                                     const std::string& description, const std::string& category) {
    if (!creators_.contains(type)) {
        type_order_.push_back(type);
    }
    creators_[type] = TypeInfo{std::move(creator), description, category};
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

const char* ComponentFactory::category(const std::string& type) const {
    auto it = creators_.find(type);
    if (it != creators_.end() && !it->second.category.empty()) {
        return it->second.category.c_str();
    }
    return "Other";
}

} // namespace gryce_engine::components
