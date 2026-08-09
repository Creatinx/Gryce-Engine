using GryceEngine.Editor.Models;
using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

public partial class InspectorView : UserControl
{
    private EditorViewModel? VM => DataContext as EditorViewModel;

    public InspectorView()
    {
        InitializeComponent();
    }

    private void OnEntityNameChanged(object sender, RoutedEventArgs e)
    {
        if (VM?.SelectedEntity == null) return;
        VM.RenameEntity(VM.SelectedEntity.Handle, VM.SelectedEntity.Name);
    }

    private void OnTransformValueChanged(object sender, RoutedEventArgs e)
    {
        VM?.WriteTransform();
    }

    private void OnAddComponentClick(object sender, RoutedEventArgs e)
    {
        if (VM == null) return;
        ModalDialog.Show(new AddComponentDialog(VM), Window.GetWindow(this));
    }

    private void OnComponentHeaderClick(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (sender is FrameworkElement fe && fe.Tag is ComponentModel comp)
        {
            comp.IsExpanded = !comp.IsExpanded;
        }
    }

    private void OnComponentContextMenuClick(object sender, RoutedEventArgs e)
    {
        if (VM == null) return;
        if (sender is FrameworkElement fe && fe.Tag is ulong typeHash)
        {
            var contextMenu = new ContextMenu();
            var removeItem = new MenuItem
            {
                Header = LocalizationService.Instance.T("inspector.remove_component"),
                InputGestureText = "Del"
            };
            removeItem.Icon = new TextBlock
            {
                Text = "\uE74D",
                FontFamily = new System.Windows.Media.FontFamily("Segoe MDL2 Assets"),
                FontSize = 12
            };
            removeItem.Click += (_, _) => VM.RemoveComponent(typeHash);
            contextMenu.Items.Add(removeItem);

            contextMenu.IsOpen = true;
        }
    }

    private void OnPropertyValueChanged(object sender, RoutedEventArgs e)
    {
        if (VM == null) return;
        if (sender is FrameworkElement fe && fe.Tag is PropertyModel prop)
        {
            if (VM.SelectedEntity != null)
            {
                foreach (var comp in VM.SelectedEntity.Components)
                {
                    if (comp.Properties.Contains(prop))
                    {
                        VM.WritePropertyValue(comp, prop);
                        break;
                    }
                }
            }
        }
    }

    private void OnPropertyBoolChanged(object sender, RoutedEventArgs e)
    {
        OnPropertyValueChanged(sender, e);
    }

    private void OnPropertyStringChanged(object sender, RoutedEventArgs e)
    {
        OnPropertyValueChanged(sender, e);
    }

    private void OnScriptPropFloatChanged(object sender, RoutedEventArgs e)
    {
        if (VM?.SelectedEntity == null) return;
        if (sender is FrameworkElement fe && fe.Tag is ScriptPropModel prop)
        {
            VM.WriteScriptProp(VM.SelectedEntity.Handle, prop.Name, prop.FloatValue);
        }
    }

    private void OnScriptPropStringChanged(object sender, RoutedEventArgs e)
    {
        if (VM?.SelectedEntity == null) return;
        if (sender is FrameworkElement fe && fe.Tag is ScriptPropModel prop)
        {
            VM.WriteScriptProp(VM.SelectedEntity.Handle, prop.Name, prop.StringValue);
        }
    }

    private void OnEditMaterialClick(object sender, RoutedEventArgs e)
    {
        if (VM?.SelectedEntity == null) return;
        foreach (var comp in VM.SelectedEntity.Components)
        {
            if (comp.TypeName is "MeshRenderer" or "SkinnedMeshRenderer")
            {
                var window = new MaterialEditorWindow(VM, VM.SelectedEntity, comp)
                {
                    Owner = Window.GetWindow(this)
                };
                window.Show();
                return;
            }
        }
    }
}
