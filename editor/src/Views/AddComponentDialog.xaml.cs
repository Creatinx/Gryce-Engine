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
    private readonly bool _renameEntityToType;
    private ComponentTypeEntry? _selected;

    /// <summary>
    /// <paramref name="renameEntityToType"/> is set when the dialog is opened
    /// from the Create-Entity flow: committing a component then renames the
    /// freshly created entity to the component type (e.g. "MeshRenderer"), so
    /// the new node "becomes" that type instead of staying a generic entity.
    /// </summary>
    public AddComponentDialog(EditorViewModel vm, bool renameEntityToType = false)
    {
        InitializeComponent();
        _vm = vm;
        _renameEntityToType = renameEntityToType;
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
            if (ComponentTypeFilter.IsExcluded(t.TypeName)) continue;
            if (!string.IsNullOrWhiteSpace(filter) &&
                t.TypeName.IndexOf(filter.Trim(), StringComparison.OrdinalIgnoreCase) < 0)
                continue;

            var (category, icon, color) = ComponentCatalog.Categorize(t.TypeName);
            GroupOf(category, icon).Children.Add(new ComponentTypeEntry(
                t.TypeName, t.TypeHash, icon, color, ComponentCatalog.Describe(t.TypeName)));
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
        int result = _vm.AddComponent(_selected.TypeHash);
        if (result == 0 && _renameEntityToType && _vm.SelectedEntity != null)
        {
            _vm.RenameEntity(_vm.SelectedEntity.Handle, _selected.Name);
        }
        DialogResult = true;
    }
}
