using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Threading;

namespace GryceEngine.Editor.Services;

/// <summary>
/// Attached behavior that gives WPF ContextMenus a Mica-style surface:
/// applies the DWM backdrop to the popup window where the OS supports it, and
/// falls back to a translucent acrylic-like fill otherwise.
/// </summary>
public static class MenuMica
{
    public static readonly DependencyProperty EnableMicaProperty =
        DependencyProperty.RegisterAttached(
            "EnableMica", typeof(bool), typeof(MenuMica),
            new PropertyMetadata(false, OnEnableMicaChanged));

    public static bool GetEnableMica(DependencyObject obj) => (bool)obj.GetValue(EnableMicaProperty);

    public static void SetEnableMica(DependencyObject obj, bool value) => obj.SetValue(EnableMicaProperty, value);

    private static void OnEnableMicaChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is ContextMenu menu && (bool)e.NewValue)
        {
            menu.Opened += OnMenuOpened;
        }
    }

    private static void OnMenuOpened(object sender, RoutedEventArgs e)
    {
        if (sender is not ContextMenu menu) return;
        // The popup window is created during open; apply right after layout.
        menu.Dispatcher.BeginInvoke(new Action(() => Apply(menu)), DispatcherPriority.Loaded);
    }

    private static void Apply(ContextMenu menu)
    {
        try
        {
            menu.ApplyTemplate();
            var popup = menu.Template?.FindName("PART_Popup", menu) as Popup;
            var child = popup?.Child;
            if (child != null)
            {
                var source = PresentationSource.FromVisual(child) as HwndSource;
                if (source != null && source.Handle != IntPtr.Zero)
                {
                    // Real Mica backdrop where the popup window accepts it.
                    MicaHelper.TryApplyMica(source.Handle);
                }
            }

            // Translucent surface so the Mica/acrylic material shows through.
            bool dark = iNKORE.UI.WPF.Modern.ThemeManager.Current.ActualApplicationTheme !=
                        iNKORE.UI.WPF.Modern.ApplicationTheme.Light;
            byte a = dark ? (byte)0xE8 : (byte)0xF2;
            byte baseC = dark ? (byte)0x2C : (byte)0xF0;
            menu.SetValue(Control.BackgroundProperty,
                new SolidColorBrush(Color.FromArgb(a, baseC, baseC, baseC)));
        }
        catch { /* best effort */ }
    }
}
