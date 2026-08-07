using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

/// <summary>A type entry in the Create Entity dialog.</summary>
public sealed class CreateTypeItem
{
    public string Name { get; }
    public string? ComponentTypeName { get; }
    public string Description { get; }
    public string Category { get; }

    public CreateTypeItem(string name, string? componentTypeName, string description, string category)
    {
        Name = name;
        ComponentTypeName = componentTypeName;
        Description = description;
        Category = category;
    }

    public override string ToString() => Name;

    public Visibility CategoryVisibility => string.IsNullOrEmpty(Category)
        ? Visibility.Collapsed
        : Visibility.Visible;
}

/// <summary>
/// Godot-style Create Entity dialog: searchable type list with recent items.
/// Creating an entity is delegated to the ViewModel, which talks to the engine
/// exclusively through the C API command buffer.
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
        BuildTypeList(string.Empty);
        RefreshRecentList();
    }

    private static string T(string key) => LocalizationService.Instance.T(key);

    private void BuildTypeList(string filter)
    {
        TypeList.Items.Clear();

        var empty = new CreateTypeItem(
            T("create_entity.empty_entity"),
            null,
            T("create_entity.desc.empty"),
            string.Empty);
        if (Matches(empty.Name, filter)) TypeList.Items.Add(empty);

        foreach (var type in _vm.RegisteredTypes)
        {
            if (!Matches(type.TypeName, filter)) continue;
            var item = new CreateTypeItem(
                type.TypeName,
                type.TypeName,
                Describe(type.TypeName),
                Categorize(type.TypeName));
            TypeList.Items.Add(item);
        }

        if (TypeList.Items.Count == 0)
        {
            DescriptionText.Text = T("create_entity.no_result");
            return;
        }

        TypeList.SelectedIndex = 0;
    }

    private static bool Matches(string name, string filter)
    {
        if (string.IsNullOrWhiteSpace(filter)) return true;
        return name.IndexOf(filter.Trim(), System.StringComparison.OrdinalIgnoreCase) >= 0;
    }

    private string Describe(string typeName)
    {
        string key = "create_entity.desc." + typeName;
        string localized = T(key);
        if (localized != key) return localized;
        return string.Format(T("create_entity.generic_desc"), typeName);
    }

    private static string Categorize(string typeName)
    {
        if (typeName.IndexOf("2D", System.StringComparison.Ordinal) >= 0) return "2D";
        if (typeName.IndexOf("Light", System.StringComparison.Ordinal) >= 0) return "Light";
        if (typeName.IndexOf("Camera", System.StringComparison.Ordinal) >= 0) return "Camera";
        if (typeName.IndexOf("Audio", System.StringComparison.Ordinal) >= 0) return "Audio";
        if (typeName.IndexOf("Body", System.StringComparison.Ordinal) >= 0 ||
            typeName.IndexOf("Collider", System.StringComparison.Ordinal) >= 0 ||
            typeName.IndexOf("Joint", System.StringComparison.Ordinal) >= 0 ||
            typeName.IndexOf("Material", System.StringComparison.Ordinal) >= 0)
            return "Physics";
        return "3D";
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

    private void OnSearchTextChanged(object sender, TextChangedEventArgs e)
    {
        BuildTypeList(SearchBox.Text);
    }

    private void OnTypeSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (TypeList.SelectedItem is CreateTypeItem item)
        {
            _selected = item;
            DescriptionText.Text = item.Description;
        }
    }

    private void OnRecentSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (RecentList.SelectedItem is not string typeName) return;
        foreach (CreateTypeItem item in TypeList.Items)
        {
            string key = item.ComponentTypeName ?? item.Name;
            if (key == typeName)
            {
                TypeList.SelectedItem = item;
                TypeList.ScrollIntoView(item);
                break;
            }
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
