using System.Collections.Generic;

namespace GryceEngine.Editor.Services;

/// <summary>
/// Shared filter for the Add-Component / Create-Entity pickers.
/// 仅排除 Core 内部类型（Transform、PrefabInstance、PhysicsBody），
/// 2D / UI / 物理组件全部开放，便于在统一场景树中构建 2D 或 2D+3D 游戏。
/// </summary>
public static class ComponentTypeFilter
{
    private static readonly HashSet<string> Excluded = new(System.StringComparer.OrdinalIgnoreCase)
    {
        // Internal components (Core: 不应手动添加)
        "Transform", "PrefabInstance", "PhysicsBody"
    };

    public static bool IsExcluded(string typeName)
        => Excluded.Contains(typeName);
}
