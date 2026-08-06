import re

# Fix component_api.cpp
path = r'D:/Gryce-Engine/core/api/component_api.cpp'
content = open(path).read()

# 1. Add #include <typeinfo> after line 10 (#include <vector>)
content = content.replace('#include <vector>', '#include <vector>\n#include <typeinfo>')

# 2. Replace get_component_type_name function
old_func = '''// Helper: get type name from a component via dynamic_cast chain
static std::string get_component_type_name(gryce_engine::components::Component* comp) {
    if (!comp) return "";
    // Use reflection registry to find matching type
    // Try common types by dynamic_cast
    // This is a simplified approach - in production we'd use RTTI typeid
    #define TRY_TYPE(T) if (dynamic_cast<gryce_engine::components::T*>(comp)) return #T
    TRY_TYPE(MeshRenderer);
    TRY_TYPE(SkinnedMeshRenderer);
    TRY_TYPE(Camera);
    TRY_TYPE(Light);
    TRY_TYPE(AudioSource);
    TRY_TYPE(AudioListener);
    TRY_TYPE(RigidBody);
    TRY_TYPE(StaticBody);
    TRY_TYPE(BoxCollider);
    TRY_TYPE(SphereCollider);
    TRY_TYPE(PlaneCollider);
    TRY_TYPE(CharacterController3D);
    TRY_TYPE(PhysicsBody);
    TRY_TYPE(PhysicalMaterial);
    TRY_TYPE(FragmentBody);
    TRY_TYPE(DestructibleBody);
    TRY_TYPE(PrefabInstance);
    TRY_TYPE(RigidBody2D);
    TRY_TYPE(StaticBody2D);
    TRY_TYPE(BoxCollider2D);
    TRY_TYPE(CircleCollider2D);
    TRY_TYPE(CharacterController2D);
    TRY_TYPE(Joint2D);
    TRY_TYPE(Joint3D);
    TRY_TYPE(Node2D);
    TRY_TYPE(Node3D);
    TRY_TYPE(Terrain);
    // 2D components
    TRY_TYPE(BasicRect);
    TRY_TYPE(ColorRect);
    TRY_TYPE(Label);
    TRY_TYPE(Sprite2D);
    TRY_TYPE(Circle);
    TRY_TYPE(Polygon);
    TRY_TYPE(Camera2D);
    TRY_TYPE(Light2D);
    TRY_TYPE(AmbientLight2D);
    TRY_TYPE(ParticleEmitter2D);
    TRY_TYPE(ParallaxBackground);
    TRY_TYPE(Skybox2D);
    TRY_TYPE(Tilemap);
    #undef TRY_TYPE
    return "Component";
}'''

new_func = '''// Helper: get type name from a component via RTTI typeid
static std::string get_component_type_name(gryce_engine::components::Component* comp) {
    if (!comp) return "";
    const char* raw = typeid(*comp).name();
    // MSVC: prefix "class " or "struct " — strip it
    if (std::strncmp(raw, "class ", 6) == 0) raw += 6;
    else if (std::strncmp(raw, "struct ", 7) == 0) raw += 7;
    return std::string(raw);
}'''

content = content.replace(old_func, new_func)

open(path, 'w').write(content)
print("component_api.cpp fixed")
