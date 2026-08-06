import re

p = r'D:/Gryce-Engine/core/api/core_api.cpp'
with open(p, 'r', encoding='utf-8') as f:
    content = f.read()

# The file has been corrupted with duplicate EntityResolver::resolve and get_component_type_name.
# Let's rebuild the Helpers section cleanly.

old_helpers = '''// ============================================================================
// Helpers
// ============================================================================
Entity* EntityResolver::resolve(GEntityHandle h) {
    if (!g_core_state.world || h == 0) return nullptr;
    gryce_engine::scene::UUID* uuid = g_core_state.entity_map.resolve_uuid(h);
    if (!uuid) return nullptr;
    Scene* s = g_core_state.world->scene();
    if (!s) return nullptr;
    return s->find_entity_by_uuid(static_cast<const gryce_engine::scene::UUID&>(*uuid));
}

// Forward: defined in component_api.cpp
std::string get_component_type_name(gryce_engine::components::Component* comp);
    if (!g_core_state.world || h == 0) return nullptr;
    gryce_engine::scene::UUID* uuid = g_core_state.entity_map.resolve_uuid(h);
    if (!uuid) return nullptr;
    Scene* s = g_core_state.world->scene();
    if (!s) return nullptr;
    return s->find_entity_by_uuid(static_cast<const gryce_engine::scene::UUID&>(*uuid));
}

// Helper: get type name from a Component* via dynamic_cast chain
std::string get_component_type_name(gryce_engine::components::Component* comp) {
    if (!comp) return "";
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
}
    if (!g_core_state.world || h == 0) return nullptr;
    gryce_engine::scene::UUID* uuid = g_core_state.entity_map.resolve_uuid(h);
    if (!uuid) return nullptr;
    Scene* s = g_core_state.world->scene();
    if (!s) return nullptr;
    return s->find_entity_by_uuid(static_cast<const gryce_engine::scene::UUID&>(*uuid));
}

static void fire_callback_entity_selected(GEntityHandle h) {'''

new_helpers = '''// ============================================================================
// Helpers
// ============================================================================
Entity* EntityResolver::resolve(GEntityHandle h) {
    if (!g_core_state.world || h == 0) return nullptr;
    gryce_engine::scene::UUID* uuid = g_core_state.entity_map.resolve_uuid(h);
    if (!uuid) return nullptr;
    Scene* s = g_core_state.world->scene();
    if (!s) return nullptr;
    return s->find_entity_by_uuid(static_cast<const gryce_engine::scene::UUID&>(*uuid));
}

// Forward: defined in component_api.cpp
std::string get_component_type_name(gryce_engine::components::Component* comp);

static void fire_callback_entity_selected(GEntityHandle h) {'''

if old_helpers in content:
    content = content.replace(old_helpers, new_helpers)
    with open(p, 'w', encoding='utf-8') as f:
        f.write(content)
    print('Fixed core_api.cpp helpers section')
else:
    print('Could not find exact match, trying regex...')
    # Fallback: just rewrite the file from scratch
    print('Fallback needed')
