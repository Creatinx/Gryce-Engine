using GryceEngine.Editor.Models;
using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

public partial class HierarchyView : UserControl
{
    private EditorViewModel? VM => DataContext as EditorViewModel;

    public HierarchyView()
    {
        InitializeComponent();
    }

    private void RenameSelected()
    {
        if (VM?.SelectedEntity == null) return;
        var entity = VM.SelectedEntity;
        var dialog = new InputDialog(
            LocalizationService.Instance.T("menu.rename"),
            LocalizationService.Instance.T("common.name"),
            entity.Name);
        if (ModalDialog.Show(dialog, Window.GetWindow(this)) == true &&
            !string.IsNullOrWhiteSpace(dialog.InputText))
        {
            VM.RenameEntity(entity.Handle, dialog.InputText);
        }
    }

    private void OnEntitySelected(object sender, RoutedEventArgs e)
    {
        if (VM == null) return;
        if (sender is TreeViewItem item && item.DataContext is EntityModel entity)
        {
            VM.SelectedEntity = entity;
        }
    }

    private void OnEntityRightClick(object sender, MouseButtonEventArgs e)
    {
        if (sender is TreeViewItem item && item.DataContext is EntityModel entity)
        {
            item.IsSelected = true;
            VM!.SelectedEntity = entity;
        }
    }

    /// <summary>
    /// Right-clicking empty space inside the hierarchy opens the same
    /// context menu (Create Entity / Create Child), while right-clicking an
    /// entity row lets the ContextMenuService open the menu normally.
    /// </summary>
    private void OnTreePreviewRightClick(object sender, MouseButtonEventArgs e)
    {
        if (e.OriginalSource is not DependencyObject source) return;
        if (FindAncestor<TreeViewItem>(source) != null) return;

        var menu = EntityTree.ContextMenu;
        if (menu == null) return;
        menu.PlacementTarget = sender as UIElement ?? EntityTree;
        menu.Placement = System.Windows.Controls.Primitives.PlacementMode.MousePoint;
        menu.IsOpen = true;
        e.Handled = true;
    }

    private static T? FindAncestor<T>(DependencyObject? child) where T : DependencyObject
    {
        while (child != null)
        {
            if (child is T match) return match;
            child = VisualTreeHelper.GetParent(child);
        }
        return null;
    }

    private void OnEntityKeyDown(object sender, KeyEventArgs e)
    {
        if (VM == null) return;
        switch (e.Key)
        {
            case Key.Delete:
                VM.DeleteSelectedEntity();
                e.Handled = true;
                break;
            case Key.F2:
                OnRenameEntityClick(sender, e);
                e.Handled = true;
                break;
            case Key.D when Keyboard.Modifiers == ModifierKeys.Control:
                VM.DuplicateCommand.Execute(null);
                e.Handled = true;
                break;
        }
    }

    private void OnDeleteEntityClick(object sender, RoutedEventArgs e)
    {
        VM?.DeleteSelectedEntity();
    }

    private void OnCreateEntityClick(object sender, RoutedEventArgs e)
    {
        OpenCreateEntityDialog(GEntityHandle.Null);
    }

    private void OnCreateChildClick(object sender, RoutedEventArgs e)
    {
        if (VM?.SelectedEntity != null)
            OpenCreateEntityDialog(VM.SelectedEntity.Handle);
    }

    private void OnDuplicateEntityClick(object sender, RoutedEventArgs e)
    {
        VM?.DuplicateSelectedEntity();
    }

    private void OpenCreateEntityDialog(GEntityHandle parent)
    {
        if (VM == null) return;
        var dialog = new CreateEntityDialog(VM, parent)
        {
            Owner = Window.GetWindow(this)
        };
        ModalDialog.Show(dialog, Window.GetWindow(this));
    }

    private void OnRenameEntityClick(object sender, RoutedEventArgs e)
    {
        RenameSelected();
    }

    private void OnSearchTextChanged(object sender, TextChangedEventArgs e)
    {
        ClearSearchBtn.Visibility = string.IsNullOrEmpty(SearchBox.Text)
            ? Visibility.Collapsed : Visibility.Visible;
        FilterEntities(SearchBox.Text);
    }

    private void OnClearSearchClick(object sender, RoutedEventArgs e)
    {
        SearchBox.Text = string.Empty;
    }

    private void FilterEntities(string searchText)
    {
        if (string.IsNullOrWhiteSpace(searchText))
        {
            foreach (var item in GetAllTreeViewItems(EntityTree))
            {
                item.Visibility = Visibility.Visible;
            }
            return;
        }

        foreach (var item in GetAllTreeViewItems(EntityTree))
        {
            if (item.DataContext is EntityModel entity)
            {
                bool matches = entity.Name.IndexOf(searchText, StringComparison.OrdinalIgnoreCase) >= 0;
                item.Visibility = matches ? Visibility.Visible : Visibility.Collapsed;
                if (matches)
                {
                    ExpandToItem(item);
                }
            }
        }
    }

    private static void ExpandToItem(TreeViewItem item)
    {
        var parent = GetParentTreeViewItem(item);
        while (parent != null)
        {
            parent.IsExpanded = true;
            parent = GetParentTreeViewItem(parent);
        }
    }

    private static IEnumerable<TreeViewItem> GetAllTreeViewItems(ItemsControl container)
    {
        foreach (var child in container.Items)
        {
            if (container.ItemContainerGenerator.ContainerFromItem(child) is TreeViewItem item)
            {
                yield return item;
                foreach (var nested in GetAllTreeViewItems(item))
                    yield return nested;
            }
        }
    }

    private static TreeViewItem? GetParentTreeViewItem(TreeViewItem item)
    {
        var parent = VisualTreeHelper.GetParent(item);
        while (parent != null && parent is not TreeViewItem)
            parent = VisualTreeHelper.GetParent(parent);
        return parent as TreeViewItem;
    }
}
