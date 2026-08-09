using System;
using System.Windows.Media;

namespace GryceEngine.Editor.Services;

/// <summary>
/// Visual categorization shared by the Add-Component and Create-Entity pickers:
/// category, Segoe Fluent icon glyph and accent color for each registered type.
/// </summary>
public static class ComponentCatalog
{
    public static (string Category, string Icon, Brush Color) Categorize(string typeName)
    {
        if (typeName.IndexOf("MeshRenderer", StringComparison.Ordinal) >= 0)
            return ("Rendering", "\uE8B8", NewBrush(0x8C, 0xCB, 0xFF));
        if (typeName.IndexOf("Camera", StringComparison.Ordinal) >= 0)
            return ("Camera & Light", "\uE722", NewBrush(0x80, 0xC0, 0xFF));
        if (typeName.IndexOf("Light", StringComparison.Ordinal) >= 0)
            return ("Camera & Light", "\uE706", NewBrush(0xFF, 0xD0, 0x66));
        if (typeName.IndexOf("Body", StringComparison.Ordinal) >= 0 ||
            typeName.IndexOf("Collider", StringComparison.Ordinal) >= 0 ||
            typeName.IndexOf("Joint", StringComparison.Ordinal) >= 0 ||
            typeName.IndexOf("Controller", StringComparison.Ordinal) >= 0)
            return ("Physics", "\uE8B5", NewBrush(0x8C, 0xFF, 0xD0));
        if (typeName.IndexOf("Audio", StringComparison.Ordinal) >= 0)
            return ("Audio", "\uE8D6", NewBrush(0xFF, 0xCC, 0x8C));
        if (typeName.IndexOf("Terrain", StringComparison.Ordinal) >= 0 ||
            typeName.IndexOf("Material", StringComparison.Ordinal) >= 0 ||
            typeName.IndexOf("Destructible", StringComparison.Ordinal) >= 0 ||
            typeName.IndexOf("Fragment", StringComparison.Ordinal) >= 0)
            return ("World", "\uE8B9", NewBrush(0x8C, 0xFF, 0x8C));
        return ("Node", "\uE8B7", NewBrush(0xAA, 0xAA, 0xAA));
    }

    public static SolidColorBrush NewBrush(byte r, byte g, byte b)
        => new(Color.FromRgb(r, g, b));

    /// <summary>Localized description; falls back to the generic template.</summary>
    public static string Describe(string typeName)
    {
        string key = "create_entity.desc." + typeName;
        string localized = LocalizationService.Instance.T(key);
        if (localized != key) return localized;
        return string.Format(LocalizationService.Instance.T("create_entity.generic_desc"), typeName);
    }
}
