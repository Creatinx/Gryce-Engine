using GryceEngine.Editor.Models;
using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

public partial class InspectorView : UserControl
{
    private EditorViewModel? VM => DataContext as EditorViewModel;

    // Baseline captured on focus for incremental undo of float property edits.
    private readonly Dictionary<TextBox, byte[]> _propertyBaselines = new();
    // Baseline captured on focus for incremental undo of Transform edits.
    private readonly Dictionary<TextBox, TransformBaseline> _transformBaselines = new();

    public InspectorView()
    {
        InitializeComponent();
    }

    private void OnEntityNameChanged(object sender, RoutedEventArgs e)
    {
        if (VM?.SelectedEntity == null) return;
        VM.RenameEntity(VM.SelectedEntity.Handle, VM.SelectedEntity.Name);
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

    /// <summary>Browse for a file and write it into a string path property.</summary>
    private void OnPropertyBrowseClick(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement fe || fe.Tag is not PropertyModel prop) return;
        var dialog = new Microsoft.Win32.OpenFileDialog
        {
            Title = LocalizationService.Instance.T("common.browse"),
            FileName = prop.StringValue ?? string.Empty
        };
        string? initialDir = null;
        if (!string.IsNullOrEmpty(prop.StringValue))
        {
            try { initialDir = System.IO.Path.GetDirectoryName(prop.StringValue); } catch { }
        }
        if (string.IsNullOrEmpty(initialDir))
        {
            initialDir = App.Engine?.ProjectRoot;
        }
        if (!string.IsNullOrEmpty(initialDir) && System.IO.Directory.Exists(initialDir))
        {
            dialog.InitialDirectory = initialDir;
        }
        if (dialog.ShowDialog(Window.GetWindow(this)) == true)
        {
            prop.StringValue = dialog.FileName ?? string.Empty;
            if (VM?.SelectedEntity != null)
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

    /// <summary>Open the New Script dialog and assign the created script to the
    /// Script component's script_path property.</summary>
    private void OnScriptNewClick(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement fe || fe.Tag is not PropertyModel prop) return;
        if (VM?.SelectedEntity == null) return;

        string projectRoot = App.Engine?.ProjectRoot ?? string.Empty;
        string scriptsDir = string.IsNullOrEmpty(projectRoot)
            ? System.IO.Path.GetDirectoryName(prop.StringValue ?? string.Empty) ?? projectRoot
            : System.IO.Path.Combine(projectRoot, "scripts");
        try { System.IO.Directory.CreateDirectory(scriptsDir); } catch { }

        var dialog = new NewScriptWindow(scriptsDir,
            VM.RegisteredTypes.Select(t => t.TypeName), null,
            VM.GetSceneComponentTypes());
        dialog.Owner = Window.GetWindow(this);
        string? created = null;
        if (dialog.ShowDialog() == true)
        {
            created = dialog.CreatedPath;
        }
        if (!string.IsNullOrEmpty(created))
        {
            prop.StringValue = created!;
            foreach (var comp in VM.SelectedEntity.Components)
            {
                if (comp.Properties.Contains(prop))
                {
                    VM.WritePropertyValue(comp, prop);
                    break;
                }
            }
            VM.AppendConsole(string.Format(
                LocalizationService.Instance.T("new_script.created"), created));
            ViewportView.OpenScriptRequested?.Invoke(created!);
        }
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

    // ═══ Resource drag & drop onto path slots ═══

    /// <summary>Accept a Project-panel file drop only for path-like properties.</summary>
    private void OnPathDragOver(object sender, DragEventArgs e)
    {
        bool accept = e.Data.GetDataPresent(DataFormats.FileDrop) &&
                      sender is TextBox tb && tb.Tag is PropertyModel prop &&
                      (prop.IsPathProperty || prop.IsScriptPath);
        e.Effects = accept ? DragDropEffects.Copy : DragDropEffects.None;
        e.Handled = true;
    }

    private void OnPathDrop(object sender, DragEventArgs e)
    {
        if (sender is not TextBox tb || tb.Tag is not PropertyModel prop) return;
        if (!(prop.IsPathProperty || prop.IsScriptPath)) return;
        if (e.Data.GetData(DataFormats.FileDrop) is not string[] paths || paths.Length == 0) return;

        prop.StringValue = paths[0];
        if (VM?.SelectedEntity != null)
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
        e.Handled = true;
    }

    // ═══ Realtime float property editing (Inspector) ═══

    private ComponentModel? FindCompFor(PropertyModel prop)
    {
        if (VM?.SelectedEntity == null) return null;
        foreach (var comp in VM.SelectedEntity.Components)
        {
            if (comp.Properties.Contains(prop)) return comp;
        }
        return null;
    }

    private static float ComputeStep()
    {
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Shift)) return 10f;
        if (Keyboard.Modifiers.HasFlag(ModifierKeys.Control)) return 0.1f;
        return 1f;
    }

    private void OnPropertyGotFocus(object sender, RoutedEventArgs e)
    {
        if (sender is not TextBox tb || tb.Tag is not PropertyModel prop) return;
        var comp = FindCompFor(prop);
        if (comp == null || VM?.SelectedEntity == null) return;
        var baseline = EditorViewModel.ReadPropertyBytes(
            VM.SelectedEntity.Handle, comp.TypeHash, prop.Name, prop.Size);
        _propertyBaselines[tb] = baseline ?? System.Array.Empty<byte>();
    }

    private void OnPropertyTextChanged(object sender, RoutedEventArgs e)
    {
        if (sender is not TextBox tb || tb.Tag is not PropertyModel prop) return;
        var comp = FindCompFor(prop);
        if (comp == null) return;
        if (float.TryParse(tb.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var val))
        {
            prop.FloatValue = val;
            VM?.WritePropertyLive(comp, prop);
        }
    }

    private void OnPropertyCommit(object sender, RoutedEventArgs e)
    {
        if (sender is not TextBox tb || tb.Tag is not PropertyModel prop) return;
        var comp = FindCompFor(prop);
        if (comp == null) return;
        if (_propertyBaselines.TryGetValue(tb, out var baseline))
        {
            VM?.FlushPropertyUndo(comp, prop, baseline.Length == 0 ? null : baseline);
            _propertyBaselines.Remove(tb);
        }
    }

    private void OnPropertyMouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (sender is not TextBox tb || tb.Tag is not PropertyModel prop) return;
        e.Handled = true;
        var comp = FindCompFor(prop);
        if (comp == null) return;
        prop.FloatValue += (e.Delta > 0 ? 1f : -1f) * ComputeStep();
        VM?.WritePropertyLive(comp, prop);
    }

    // ═══ Realtime Transform editing with incremental undo ═══

    private void OnTransformGotFocus(object sender, RoutedEventArgs e)
    {
        if (sender is not TextBox tb || VM?.SelectedEntity == null) return;
        _transformBaselines[tb] = new TransformBaseline(
            VM.SelectedEntity.LocalPosition,
            VM.SelectedEntity.LocalRotation,
            VM.SelectedEntity.LocalScale);
    }

    private void OnTransformTextChanged(object sender, RoutedEventArgs e)
    {
        if (sender is TextBox tb) ApplyTransformField(tb);
    }

    private void OnTransformCommit(object sender, RoutedEventArgs e)
    {
        if (sender is not TextBox tb) return;
        if (_transformBaselines.TryGetValue(tb, out var bl))
        {
            VM?.FlushTransformUndo(bl.Position, bl.Rotation, bl.Scale);
            _transformBaselines.Remove(tb);
        }
    }

    private void OnTransformMouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (sender is not TextBox tb) return;
        e.Handled = true;
        ApplyTransformField(tb, (e.Delta > 0 ? 1f : -1f) * ComputeStep());
    }

    /// <summary>Write the parsed field value (or a wheel delta) into the matching
    /// Transform sub-field and push a live update to the engine.</summary>
    private void ApplyTransformField(TextBox tb, float? wheelDelta = null)
    {
        var entity = VM?.SelectedEntity;
        if (entity == null) return;
        float val;
        if (wheelDelta.HasValue)
        {
            val = wheelDelta.Value;
        }
        else if (!float.TryParse(tb.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out val))
        {
            return;
        }

        switch (tb.Name)
        {
            case "TransPosX": entity.PositionX = val; break;
            case "TransPosY": entity.PositionY = val; break;
            case "TransPosZ": entity.PositionZ = val; break;
            case "TransRotX": entity.RotationX = val; break;
            case "TransRotY": entity.RotationY = val; break;
            case "TransRotZ": entity.RotationZ = val; break;
            case "TransScaleX": entity.ScaleX = val; break;
            case "TransScaleY": entity.ScaleY = val; break;
            case "TransScaleZ": entity.ScaleZ = val; break;
            default: return;
        }
        VM?.WriteTransformLive();
    }
}

/// <summary>Transform state captured on focus for incremental undo.</summary>
internal readonly struct TransformBaseline
{
    public GVec3 Position { get; }
    public GQuat Rotation { get; }
    public GVec3 Scale { get; }

    public TransformBaseline(GVec3 position, GQuat rotation, GVec3 scale)
    {
        Position = position;
        Rotation = rotation;
        Scale = scale;
    }
}
