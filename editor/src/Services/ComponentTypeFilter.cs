using System.Collections.Generic;

namespace GryceEngine.Editor.Services;

/// <summary>
/// Shared filter for the Add-Component / Create-Entity pickers. 2D and UI
/// components do not belong in this 3D editor, and the Core registers some
/// internal types (Transform, PrefabInstance, PhysicsBody) that must not be
/// attached manually.
/// </summary>
public static class ComponentTypeFilter
{
    private static readonly HashSet<string> Excluded = new(System.StringComparer.OrdinalIgnoreCase)
    {
        // 2D / UI
        "Node2D", "Sprite2D", "Label", "ColorRect", "Circle", "Polygon", "Camera2D",
        "Tilemap", "ParticleEmitter2D", "ParallaxBackground", "Skybox2D", "Light2D",
        "AmbientLight2D",
        "RigidBody2D", "StaticBody2D", "BoxCollider2D", "CircleCollider2D",
        "CharacterController2D", "Joint2D",
        // Internal components (Core: 不应手动添加)
        "Transform", "PrefabInstance", "PhysicsBody"
    };

    public static bool IsExcluded(string typeName)
        => Excluded.Contains(typeName);
}
