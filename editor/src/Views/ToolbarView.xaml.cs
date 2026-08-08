using GryceEngine.Editor.ViewModels;
using GryceEngine.Editor.Services;
using System.Windows;
using System.Windows.Controls;

namespace GryceEngine.Editor.Views;

public partial class ToolbarView : UserControl
{
    private EditorViewModel? VM => DataContext as EditorViewModel;
    private string _activeTool = "Translate";

    public ToolbarView()
    {
        InitializeComponent();
    }

    private void SetActiveTool(string tool)
    {
        _activeTool = tool;
        BtnHand.IsChecked = tool == "Hand";
        BtnTranslate.IsChecked = tool == "Translate";
        BtnRotate.IsChecked = tool == "Rotate";
        BtnScale.IsChecked = tool == "Scale";
        VM?.SetGizmoMode(tool);
    }

    private void OnHandClick(object sender, RoutedEventArgs e) => SetActiveTool("Hand");
    private void OnTranslateClick(object sender, RoutedEventArgs e) => SetActiveTool("Translate");
    private void OnRotateClick(object sender, RoutedEventArgs e) => SetActiveTool("Rotate");
    private void OnScaleClick(object sender, RoutedEventArgs e) => SetActiveTool("Scale");

    private void OnCenterClick(object sender, RoutedEventArgs e)
    {
        // Center/Pivot selection-point button; state kept by the ToggleButton.
    }

    private void OnGizmoSpaceClick(object sender, RoutedEventArgs e)
    {
        VM?.GizmoToggleSpaceCommand.Execute(null);
        BtnGizmoSpace.Content = LocalizationService.Instance.T(
            VM?.IsGizmoLocal == true ? "toolbar.local" : "toolbar.global");
    }
}
