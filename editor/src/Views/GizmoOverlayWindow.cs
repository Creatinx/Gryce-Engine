using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;

namespace GryceEngine.Editor.Views;

/// <summary>
/// Transparent, click-through overlay window used to draw the editor gizmo on
/// top of the native GLFW viewport (HwndHost airspace cannot be overlaid by
/// in-window WPF content).
/// </summary>
public sealed class GizmoOverlayWindow : Window
{
    public Canvas Canvas { get; } = new Canvas { ClipToBounds = true };
    private readonly Border _hint;
    private readonly Border _modeBadge;

    public void SetHint(string text) => _hintText.Text = text;
    private readonly TextBlock _hintText;
    private readonly TextBlock _modeText;

    public void SetMode(string mode) => _modeText.Text = mode;

    public GizmoOverlayWindow()
    {
        WindowStyle = WindowStyle.None;
        AllowsTransparency = true;
        // A nearly-invisible (alpha=1) background makes the layered window
        // hit-testable everywhere; fully transparent layered windows let
        // clicks fall through to the window below.
        Background = new SolidColorBrush(Color.FromArgb(1, 0, 0, 0));
        ShowInTaskbar = false;
        ShowActivated = false;
        Focusable = false;
        ResizeMode = ResizeMode.NoResize;

        _hintText = new TextBlock
        {
            FontSize = 10.5,
            Foreground = new SolidColorBrush(Color.FromArgb(0xC8, 0xB8, 0xB8, 0xB8)),
            VerticalAlignment = VerticalAlignment.Center
        };
        _hint = new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(0xAA, 0x1E, 0x1E, 0x1E)),
            CornerRadius = new CornerRadius(4),
            Padding = new Thickness(8, 4, 8, 4),
            Child = _hintText,
            VerticalAlignment = VerticalAlignment.Bottom,
            HorizontalAlignment = HorizontalAlignment.Left,
            Margin = new Thickness(8, 0, 0, 8)
        };

        _modeText = new TextBlock
        {
            FontSize = 10.5,
            FontWeight = FontWeights.SemiBold,
            Foreground = new SolidColorBrush(Color.FromArgb(0xFF, 0x8C, 0xFF, 0x8C)),
            VerticalAlignment = VerticalAlignment.Center
        };
        _modeBadge = new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(0xBB, 0x1E, 0x1E, 0x1E)),
            CornerRadius = new CornerRadius(4),
            Padding = new Thickness(8, 4, 8, 4),
            Child = _modeText,
            VerticalAlignment = VerticalAlignment.Top,
            HorizontalAlignment = HorizontalAlignment.Right,
            Margin = new Thickness(0, 8, 8, 0)
        };

        var root = new Grid();
        root.Children.Add(Canvas);
        root.Children.Add(_hint);
        root.Children.Add(_modeBadge);
        Content = root;
    }

}
