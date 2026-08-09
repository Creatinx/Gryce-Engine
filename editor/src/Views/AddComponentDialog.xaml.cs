using GryceEngine.Editor.Models;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

/// <summary>Category node in the Add Component tree.</summary>
public sealed class ComponentCategory
{
    public string Name { get; }
    public string Icon { get; }
    public List<ComponentTypeEntry> Children { get; } = new();

    public ComponentCategory(string name, string icon) { Name = name; Icon = icon; }
}

/// <summary>Leaf node in the Add Component tree.</summary>
public sealed class ComponentTypeEntry
{
    public string Name { get; }
    public ulong TypeHash { get; }
    public string Icon { get; }
    public Brush IconColor { get; }
    public string Description { get; }

    public ComponentTypeEntry(string name, ulong typeHash, string icon, Brush color, string description)
    {
        Name = name;
        TypeHash = typeHash;
        Icon = icon;
        IconColor = color;
        Description = description;
    }
}

/// <summary>
/// Godot/Unity-style component picker: searchable list of registered component
/// types; double-click, Enter, or the Add button attaches the component to the
/// selected entity.
/// </summary>
public partial class AddComponentDialog : Window
{
    private readonly EditorViewModel _vm;
    private ComponentTypeEntry? _selected;

    // 2D/UI components that do not belong in this 3D editor (removed per user).
    private static readonly HashSet<string> ExcludedTypes = new(StringComparer.OrdinalIgnoreCase)
    {
        "Node2D", "Sprite2D", "Label", "ColorRect", "Circle", "Polygon", "Camera2D",
        "Tilemap", "ParticleEmitter2D", "ParallaxBackground", "Skybox2D",
        "RigidBody2D", "StaticBody2D", "BoxCollider2D", "CircleCollider2D",
        "CharacterController2D", "Joint2D"
    };

    public AddComponentDialog(EditorViewModel vm)
    {
        InitializeComponent();
        _vm = vm;
        RebuildTree(string.Empty);
        Loaded += (_, _) =>
        {
            SearchBox.Focus();
            SearchBox.SelectAll();
        };
    }

    private static string T(string key) => LocalizationService.Instance.T(key);

    private void RebuildTree(string filter)
    {
        TypeTree.Items.Clear();
        var groups = new List<ComponentCategory>();
        ComponentCategory GroupOf(string category, string icon)
        {
            var g = groups.FirstOrDefault(x => x.Name == category);
            if (g == null) { g = new ComponentCategory(category, icon); groups.Add(g); }
            return g;
        }

        foreach (var t in _vm.RegisteredTypes)
        {
            if (ExcludedTypes.Contains(t.TypeName)) continue;
            if (!string.IsNullOrWhiteSpace(filter) &&
                t.TypeName.IndexOf(filter.Trim(), StringComparison.OrdinalIgnoreCase) < 0)
                continue;

            var (category, icon, color) = Categorize(t.TypeName);
            GroupOf(category, icon).Children.Add(new ComponentTypeEntry(
                t.TypeName, t.TypeHash, icon, color, Describe(t.TypeName)));
        }

        foreach (var g in groups)
        {
            var node = new TreeViewItem { Header = g, IsExpanded = true, Tag = g };
            foreach (var child in g.Children)
            {
                node.Items.Add(new TreeViewItem { Header = child, Tag = child });
            }
            TypeTree.Items.Add(node);
        }

        if (groups.Sum(g => g.Children.Count) == 0)
        {
            DescriptionText.Text = T("create_entity.no_result");
            AddButton.IsEnabled = false;
            return;
        }

        SelectFirstType();
        AddButton.IsEnabled = true;
    }

    private static (string Category, string Icon, Brush Color) Categorize(string typeName)
    {
        if (typeName.IndexOf("MeshRenderer", StringComparison.Ordinal) >= 0)
            return ("Rendering", "\uE8B8", NewBrush(0x8C, 0xCB, 0xFF));
        if (typeName.IndexOf("Camera", StringComparison.Ordinal) >= 0)
            return ("Camera & Light", "\uE722", NewBrush(0x80, 0xC0, 0xFF));
        if (typeName.IndexOf("Light", StringComparison.Ordinal) >= 0)
            return ("Camera & Light", "\uE706", NewBrush(0xFF, 0xD0, 0x66));
        if (typeName.IndexOf("Body", StringComparison.Ordinal) >= 0 ||
            typeName.IndexOf("Collider", StringComparison.Ordinal) >= 0 ||
            typeName.IndexOf("Joint", StringComparison.Ordinal) >= 0)
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

    private static SolidColorBrush NewBrush(byte r, byte g, byte b)
        => new(Color.FromRgb(r, g, b));

    private string Describe(string typeName)
    {
        string key = "create_entity.desc." + typeName;
        string localized = T(key);
        if (localized != key) return localized;
        return string.Format(T("create_entity.generic_desc"), typeName);
    }

    private void SelectFirstType()
    {
        foreach (var node in TypeTree.Items)
        {
            if (node is TreeViewItem tvi && tvi.Items.Count > 0)
            {
                tvi.IsSelected = true;
                tvi.IsExpanded = true;
                if (tvi.Items[0] is TreeViewItem first) first.IsSelected = true;
                return;
            }
        }
    }

    private void OnSearchTextChanged(object sender, TextChangedEventArgs e)
        => RebuildTree(SearchBox.Text);

    private void OnSearchKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter)
        {
            e.Handled = true;
            Commit();
        }
    }

    private void OnTypeSelectionChanged(object sender, RoutedPropertyChangedEventArgs<object> e)
    {
        if (e.NewValue is TreeViewItem tvi && tvi.Tag is ComponentTypeEntry item)
        {
            _selected = item;
            DescriptionText.Text = item.Description;
        }
    }

    private void OnTypeTreeDoubleClick(object sender, MouseButtonEventArgs e)
        => Commit();

    private void OnAddClick(object sender, RoutedEventArgs e)
        => Commit();

    private void OnCancelClick(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
    }

    private void Commit()
    {
        if (_selected == null) return;
        _vm.AddComponent(_selected.TypeHash);
        DialogResult = true;
    }
}
