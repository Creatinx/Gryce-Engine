using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

/// <summary>Category node in the Create Entity tree (parent).</summary>
public sealed class CreateEntityCategory
{
    public string Name { get; }
    public string Icon { get; }
    public List<CreateTypeItem> Children { get; } = new();

    public CreateEntityCategory(string name, string icon) { Name = name; Icon = icon; }
}

/// <summary>A type entry in the Create Entity dialog (tree leaf).</summary>
public sealed class CreateTypeItem
{
    public string Name { get; }
    public string? ComponentTypeName { get; }
    public string Description { get; }
    public string Category { get; }
    public string Icon { get; }
    public Brush IconColor { get; }

    public CreateTypeItem(string name, string? componentTypeName, string description,
                          string category, string icon, Brush iconColor)
    {
        Name = name;
        ComponentTypeName = componentTypeName;
        Description = description;
        Category = category;
        Icon = icon;
        IconColor = iconColor;
    }

    public override string ToString() => Name;
}

/// <summary>
/// Godot-style Create Entity dialog: a searchable tree of node/component types
/// (Empty Entity at the root, then categories -> component types) with recent
/// items. Creating an entity is delegated to the ViewModel, which talks to the
/// engine exclusively through the C API command buffer.
/// </summary>
public partial class CreateEntityDialog : Window
{
    private static readonly List<string> RecentTypes = new();

    private readonly EditorViewModel _vm;
    private readonly GEntityHandle _parent;
    private CreateTypeItem? _selected;

    public CreateEntityDialog(EditorViewModel vm, GEntityHandle parent)
    {
        InitializeComponent();
        _vm = vm;
        _parent = parent;
        NameBox.Text = LocalizationService.Instance.T("hierarchy.new_entity_name");
        if (parent != GEntityHandle.Null)
        {
            var sb = new System.Text.StringBuilder(128);
            if (EntityAPI.GEntity_GetName(parent, sb, sb.Capacity) == 0 &&
                sb.Length > 0)
            {
                Title = string.Format(
                    LocalizationService.Instance.T("create_entity.dialog_title_child"),
                    sb.ToString());
            }
        }
        BuildTypeTree(string.Empty);
        RefreshRecentList();
        // Godot-style: focus the search box so typing filters immediately.
        Loaded += (_, _) =>
        {
            SearchBox.Focus();
            SearchBox.SelectAll();
        };
    }

    private static string T(string key) => LocalizationService.Instance.T(key);

    private void BuildTypeTree(string filter)
    {
        TypeTree.Items.Clear();
        _selected = null;
        DescriptionText.Text = T("create_entity.no_selection");

        var empty = new CreateTypeItem(
            T("create_entity.empty_entity"),
            null,
            T("create_entity.desc.empty"),
            string.Empty,
            "\uE710",
            ComponentCatalog.NewBrush(0xAA, 0xAA, 0xAA));
        if (Matches(empty.Name, filter))
        {
            TypeTree.Items.Add(new TreeViewItem { Header = empty, Tag = empty });
        }

        var groups = new List<CreateEntityCategory>();
        CreateEntityCategory GroupOf(string category, string icon)
        {
            var g = groups.Find(x => x.Name == category);
            if (g == null)
            {
                g = new CreateEntityCategory(LocalizeCategory(category), icon);
                groups.Add(g);
            }
            return g;
        }

        foreach (var type in _vm.RegisteredTypes)
        {
            if (ComponentTypeFilter.IsExcluded(type.TypeName)) continue;
            if (!Matches(type.TypeName, filter)) continue;
            var (category, icon, color) = ComponentCatalog.Categorize(type.TypeName);
            var item = new CreateTypeItem(
                type.TypeName,
                type.TypeName,
                ComponentCatalog.Describe(type.TypeName),
                category,
                icon,
                color);
            GroupOf(category, icon).Children.Add(item);
        }

        foreach (var g in groups)
        {
            var node = new TreeViewItem { Header = g, Tag = g, IsExpanded = true };
            foreach (var child in g.Children)
            {
                node.Items.Add(new TreeViewItem { Header = child, Tag = child });
            }
            TypeTree.Items.Add(node);
        }

        if (TypeTree.Items.Count == 0)
        {
            DescriptionText.Text = T("create_entity.no_result");
            return;
        }

        SelectFirstType();
    }

    private static string LocalizeCategory(string category)
    {
        string key = "create_entity.cat." + category.ToLowerInvariant().Replace(" & ", "_");
        string localized = LocalizationService.Instance.T(key);
        return localized != key ? localized : category;
    }

    private static bool Matches(string name, string filter)
    {
        if (string.IsNullOrWhiteSpace(filter)) return true;
        return name.IndexOf(filter.Trim(), System.StringComparison.OrdinalIgnoreCase) >= 0;
    }

    private void RefreshRecentList()
    {
        RecentList.Items.Clear();
        if (RecentTypes.Count == 0)
        {
            RecentList.Items.Add(T("create_entity.empty_list"));
            return;
        }
        foreach (var name in RecentTypes)
            RecentList.Items.Add(name);
    }

    private void PushRecent(CreateTypeItem item)
    {
        string key = item.ComponentTypeName ?? item.Name;
        RecentTypes.Remove(key);
        RecentTypes.Insert(0, key);
        if (RecentTypes.Count > 10) RecentTypes.RemoveAt(RecentTypes.Count - 1);
    }

    private void SelectFirstType()
    {
        foreach (var node in TypeTree.Items)
        {
            if (node is not TreeViewItem tvi) continue;
            if (tvi.Tag is CreateTypeItem)
            {
                tvi.IsSelected = true;
                return;
            }
            if (tvi.Items.Count > 0 && tvi.Items[0] is TreeViewItem first)
            {
                first.IsSelected = true;
                return;
            }
        }
    }

    private void OnSearchTextChanged(object sender, TextChangedEventArgs e)
    {
        BuildTypeTree(SearchBox.Text);
    }

    private void OnTypeSelectionChanged(object sender, RoutedPropertyChangedEventArgs<object> e)
    {
        if (e.NewValue is TreeViewItem tvi && tvi.Tag is CreateTypeItem item)
        {
            _selected = item;
            DescriptionText.Text = item.Description;
        }
    }

    private void OnRecentSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (RecentList.SelectedItem is not string typeName) return;
        foreach (var item in AllTypeItems())
        {
            string key = item.ComponentTypeName ?? item.Name;
            if (key == typeName)
            {
                SelectItem(item);
                return;
            }
        }
    }

    private IEnumerable<CreateTypeItem> AllTypeItems()
    {
        foreach (var node in TypeTree.Items)
        {
            if (node is not TreeViewItem tvi) continue;
            if (tvi.Tag is CreateTypeItem item) yield return item;
            foreach (var child in tvi.Items)
            {
                if (child is TreeViewItem c && c.Tag is CreateTypeItem ci) yield return ci;
            }
        }
    }

    private void SelectItem(CreateTypeItem item)
    {
        foreach (var node in TypeTree.Items)
        {
            if (node is not TreeViewItem tvi) continue;
            foreach (var child in tvi.Items)
            {
                if (child is TreeViewItem c && ReferenceEquals(c.Tag, item))
                {
                    c.IsSelected = true;
                    c.BringIntoView();
                    return;
                }
            }
        }
    }

    /// <summary>Godot-style: double-clicking a type creates it directly.</summary>
    private void OnTypeTreeDoubleClick(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        OnCreateClick(sender, e);
    }

    private void OnSearchKeyDown(object sender, System.Windows.Input.KeyEventArgs e)
    {
        if (e.Key == System.Windows.Input.Key.Enter)
        {
            e.Handled = true;
            OnCreateClick(sender, e);
        }
    }

    private void OnCreateClick(object sender, RoutedEventArgs e)
    {
        if (_selected == null) return;
        string name = string.IsNullOrWhiteSpace(NameBox.Text)
            ? T("hierarchy.new_entity_name")
            : NameBox.Text.Trim();

        if (_selected.ComponentTypeName == null)
            _vm.CreateEntity(name, _parent);
        else
            _vm.CreateEntityWithComponent(name, _parent, _selected.ComponentTypeName);

        PushRecent(_selected);
        DialogResult = true;
    }

    private void OnCancelClick(object sender, RoutedEventArgs e)
    {
        DialogResult = false;
    }
}
