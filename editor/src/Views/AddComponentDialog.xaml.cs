using GryceEngine.Editor.Models;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using System;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;

namespace GryceEngine.Editor.Views;

/// <summary>
/// Godot/Unity-style component picker: searchable list of registered component
/// types; double-click, Enter, or the Add button attaches the component to the
/// selected entity.
/// </summary>
public partial class AddComponentDialog : Window
{
    private readonly EditorViewModel _vm;
    private RegisteredTypeItem? _selected;

    public AddComponentDialog(EditorViewModel vm)
    {
        InitializeComponent();
        _vm = vm;
        RefreshList(string.Empty);
        Loaded += (_, _) =>
        {
            SearchBox.Focus();
            SearchBox.SelectAll();
        };
    }

    private static string T(string key) => LocalizationService.Instance.T(key);

    private void RefreshList(string filter)
    {
        TypeList.Items.Clear();
        var items = string.IsNullOrWhiteSpace(filter)
            ? _vm.RegisteredTypes
            : _vm.RegisteredTypes.Where(t =>
                t.TypeName.IndexOf(filter.Trim(), StringComparison.OrdinalIgnoreCase) >= 0);

        foreach (var item in items)
        {
            TypeList.Items.Add(item);
        }

        if (TypeList.Items.Count == 0)
        {
            DescriptionText.Text = T("create_entity.no_result");
            AddButton.IsEnabled = false;
            return;
        }

        TypeList.SelectedIndex = 0;
        AddButton.IsEnabled = true;
    }

    private void OnSearchTextChanged(object sender, TextChangedEventArgs e)
        => RefreshList(SearchBox.Text);

    private void OnSearchKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter)
        {
            e.Handled = true;
            Commit();
        }
    }

    private void OnTypeSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (TypeList.SelectedItem is RegisteredTypeItem item)
        {
            _selected = item;
            DescriptionText.Text = item.TypeName;
        }
    }

    private void OnTypeListDoubleClick(object sender, MouseButtonEventArgs e)
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
