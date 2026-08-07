using GryceEngine.Editor.ViewModels;
using GryceEngine.Editor.Services;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

public partial class ToolbarView : UserControl
{
    private EditorViewModel? VM => DataContext as EditorViewModel;
    private string _activeTool = "Translate";

    public ToolbarView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
    }

    private void OnDataContextChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (VM != null)
        {
            VM.PropertyChanged += (_, args) =>
            {
                if (args.PropertyName == nameof(VM.IsPlaying) ||
                    args.PropertyName == nameof(VM.IsPaused))
                {
                    UpdateModeIndicator();
                    UpdatePlayButtons();
                }
            };
        }
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
        // Center/Pivot 目前仅为选择点指示按钮，自身 IsChecked 由 ToggleButton 维护，
        // 不参与坐标系切换（本地/全局由 OnGizmoSpaceClick 控制）。
    }

    private void OnGizmoSpaceClick(object sender, RoutedEventArgs e)
    {
        VM?.GizmoToggleSpaceCommand.Execute(null);
        BtnGizmoSpace.Content = LocalizationService.Instance.T(
            VM?.IsGizmoLocal == true ? "toolbar.local" : "toolbar.global");
    }

    private void OnPlayClick(object sender, RoutedEventArgs e)
    {
        if (VM == null) return;
        if (VM.IsPlaying && VM.IsPaused)
        {
            // Resume from pause
            VM.Play();
        }
        else if (VM.IsPlaying)
        {
            // Already playing, pause
            VM.Pause();
        }
        else
        {
            VM.Play();
        }
    }

    private void OnPauseClick(object sender, RoutedEventArgs e)
    {
        if (VM == null) return;
        if (VM.IsPlaying && !VM.IsPaused)
        {
            VM.Pause();
        }
        else if (VM.IsPlaying && VM.IsPaused)
        {
            // Resume (step)
            VM.Play();
        }
    }

    private void OnStopClick(object sender, RoutedEventArgs e)
    {
        VM?.Stop();
    }

    private void UpdateModeIndicator()
    {
        if (VM == null) return;
        if (VM.IsPlaying && VM.IsPaused)
        {
            ModeIndicator.Text = LocalizationService.Instance.T("status.paused");
            ModeIndicator.Background = new SolidColorBrush(
                Color.FromRgb(0xCC, 0xAA, 0x00));
        }
        else if (VM.IsPlaying)
        {
            ModeIndicator.Text = LocalizationService.Instance.T("status.playing");
            ModeIndicator.Background = new SolidColorBrush(
                Color.FromRgb(0x00, 0x88, 0x00));
        }
        else
        {
            ModeIndicator.Text = LocalizationService.Instance.T("status.editor");
            ModeIndicator.Background = (Brush)FindResource("AccentFillColorDefaultBrush");
        }
    }

    private void UpdatePlayButtons()
    {
        if (VM == null) return;
        if (VM.IsPlaying && !VM.IsPaused)
        {
            // Playing: show pause icon in play button
            BtnPlay.Content = "\uE769"; // Pause icon
            BtnPlay.Background = new SolidColorBrush(
                Color.FromArgb(0x33, 0xFF, 0xCC, 0x44));
            BtnPlay.Foreground = new SolidColorBrush(
                Color.FromArgb(0xFF, 0xFF, 0xCC, 0x44));
            BtnPause.IsEnabled = true;
            BtnStop.IsEnabled = true;
        }
        else if (VM.IsPlaying && VM.IsPaused)
        {
            // Paused: show play icon in play button
            BtnPlay.Content = "\uE768"; // Play icon
            BtnPlay.Background = new SolidColorBrush(
                Color.FromArgb(0x33, 0xFF, 0xCC, 0x44));
            BtnPlay.Foreground = new SolidColorBrush(
                Color.FromArgb(0xFF, 0xFF, 0xCC, 0x44));
            BtnPause.IsEnabled = true;
            BtnStop.IsEnabled = true;
        }
        else
        {
            // Stopped
            BtnPlay.Content = "\uE768"; // Play icon
            BtnPlay.Background = new SolidColorBrush(
                Color.FromArgb(0x1A, 0x8C, 0xFF, 0x8C));
            BtnPlay.Foreground = new SolidColorBrush(
                Color.FromArgb(0xFF, 0x8C, 0xFF, 0x8C));
            BtnPause.IsEnabled = false;
            BtnStop.IsEnabled = false;
        }
    }
}
