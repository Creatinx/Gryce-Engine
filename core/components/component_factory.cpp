#include "component_factory.h"

#include "components/transform.h"
#include "components/mesh_renderer.h"
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
#include "utils/glog/glog_lib.h"

namespace gryce_engine::components {

void register_builtin_components() {
    ComponentFactory::instance().register_type("Transform", []() {
        return std::make_unique<Transform>();
    });
    ComponentFactory::instance().register_type("MeshRenderer", []() {
        return std::make_unique<MeshRenderer>();
    });
    ComponentFactory::instance().register_type("PhysicalMaterial", []() {
        return std::make_unique<PhysicalMaterial>();
    });
    ComponentFactory::instance().register_type("PhysicsBody", []() {
        return std::make_unique<PhysicsBody>();
    });
    ComponentFactory::instance().register_type("Node2D", []() {
        return std::make_unique<Node2D>();
    });
    ComponentFactory::instance().register_type("Node3D", []() {
        return std::make_unique<Node3D>();
    });
    ComponentFactory::instance().register_type("Camera", []() {
        return std::make_unique<Camera>();
    });
    ComponentFactory::instance().register_type("Light", []() {
        return std::make_unique<Light>();
    });
    ComponentFactory::instance().register_type("StaticBody", []() {
        return std::make_unique<StaticBody>();
    });
    ComponentFactory::instance().register_type("RigidBody", []() {
        return std::make_unique<RigidBody>();
    });
    ComponentFactory::instance().register_type("BoxCollider", []() {
        return std::make_unique<BoxCollider>();
    });
    ComponentFactory::instance().register_type("SphereCollider", []() {
        return std::make_unique<SphereCollider>();
    });
    ComponentFactory::instance().register_type("PlaneCollider", []() {
        return std::make_unique<PlaneCollider>();
    });
    ComponentFactory::instance().register_type("StaticBody2D", []() {
        return std::make_unique<StaticBody2D>();
    });
    ComponentFactory::instance().register_type("RigidBody2D", []() {
        return std::make_unique<RigidBody2D>();
    });
    ComponentFactory::instance().register_type("BoxCollider2D", []() {
        return std::make_unique<BoxCollider2D>();
    });
    ComponentFactory::instance().register_type("CircleCollider2D", []() {
        return std::make_unique<CircleCollider2D>();
    });
    ComponentFactory::instance().register_type("DestructibleBody", []() {
        return std::make_unique<DestructibleBody>();
    });
    ComponentFactory::instance().register_type("FragmentBody", []() {
        return std::make_unique<FragmentBody>();
    });
    ComponentFactory::instance().register_type("AudioSource", []() {
        return std::make_unique<AudioSource>();
    });
    ComponentFactory::instance().register_type("AudioListener", []() {
        return std::make_unique<AudioListener>();
    });
    ComponentFactory::instance().register_type("ColorRect", []() {
        return std::make_unique<d2::basic_rect::ColorRect>();
    });
    ComponentFactory::instance().register_type("Circle", []() {
        return std::make_unique<d2::shape::Circle>();
    });
    ComponentFactory::instance().register_type("Polygon", []() {
        return std::make_unique<d2::shape::Polygon>();
    });
    ComponentFactory::instance().register_type("Label", []() {
        return std::make_unique<d2::text::Label>();
    });
    ComponentFactory::instance().register_type("Light2D", []() {
        return std::make_unique<d2::light::Light2D>();
    });
    ComponentFactory::instance().register_type("AmbientLight2D", []() {
        return std::make_unique<d2::light::AmbientLight2D>();
    });
    ComponentFactory::instance().register_type("Skybox2D", []() {
        return std::make_unique<d2::skybox::Skybox2D>();
    });
    ComponentFactory::instance().register_type("Sprite2D", []() {
        return std::make_unique<d2::sprite::Sprite2D>();
    });
    ComponentFactory::instance().register_type("Tilemap", []() {
        return std::make_unique<d2::tilemap::Tilemap>();
    });
    ComponentFactory::instance().register_type("Camera2D", []() {
        return std::make_unique<d2::camera::Camera2D>();
    });
    ComponentFactory::instance().register_type("ParallaxBackground", []() {
        return std::make_unique<d2::parallax::ParallaxBackground>();
    });
    ComponentFactory::instance().register_type("ParticleEmitter2D", []() {
        return std::make_unique<d2::ParticleEmitter2D>();
    });
}

ComponentFactory& ComponentFactory::instance() {
    static ComponentFactory factory;
    return factory;
}

void ComponentFactory::register_type(const std::string& type, Creator creator) {
    creators_[type] = std::move(creator);
}

std::unique_ptr<Component> ComponentFactory::create(const std::string& type) const {
    auto it = creators_.find(type);
    if (it != creators_.end()) {
        return it->second();
    }
    GLOG_WARN("ComponentFactory: unknown component type '{}'", type);
    return nullptr;
}

bool ComponentFactory::has_type(const std::string& type) const {
    return creators_.find(type) != creators_.end();
}

} // namespace gryce_engine::components
