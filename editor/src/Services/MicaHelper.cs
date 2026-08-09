using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;

namespace GryceEngine.Editor.Services;

/// <summary>
/// Windows 11 Mica backdrop helper.
/// Applies DWMWA_SYSTEMBACKDROP_TYPE to the editor window so the title bar and
/// background surfaces use the Mica material (setting is user-controllable).
/// </summary>
public static class MicaHelper
{
    private const int DWMWA_SYSTEMBACKDROP_TYPE = 38;
    private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
    private const int DWMSBT_MAINWINDOW = 2;        // Mica
    private const int DWMSBT_TRANSIENTWINDOW = 3;   // Mica alt

    [DllImport("dwmapi.dll", PreserveSig = true)]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

    [DllImport("dwmapi.dll", PreserveSig = true)]
    private static extern int DwmIsCompositionEnabled(out bool enabled);

    /// <summary>Applies Mica to a window; returns true when the OS/driver accepted it.</summary>
    public static bool TryApplyMica(Window window)
    {
        if (window == null) return false;
        var source = PresentationSource.FromVisual(window) as HwndSource;
        if (source == null || source.Handle == IntPtr.Zero) return false;
        return TryApplyMica(source.Handle);
    }

    public static bool TryApplyMica(IntPtr hwnd)
    {
        if (hwnd == IntPtr.Zero) return false;
        try
        {
            if (!IsWindows11OrLater()) return false;
            // Mica 背景与标题栏颜色跟随应用主题（而非系统主题）
            bool dark = iNKORE.UI.WPF.Modern.ThemeManager.Current.ActualApplicationTheme !=
                        iNKORE.UI.WPF.Modern.ApplicationTheme.Light;
            int useDark = dark ? 1 : 0;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref useDark, sizeof(int));
            int backdrop = DWMSBT_MAINWINDOW;
            int hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref backdrop, sizeof(int));
            return hr == 0;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Reverts a window to the standard solid backdrop.</summary>
    public static bool TryRemoveMica(IntPtr hwnd)
    {
        if (hwnd == IntPtr.Zero) return false;
        try
        {
            if (!IsWindows11OrLater()) return false;
            int backdrop = 1; // DWMSBT_AUTO
            int hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, ref backdrop, sizeof(int));
            return hr == 0;
        }
        catch
        {
            return false;
        }
    }

    private static bool IsWindows11OrLater()
    {
        try
        {
            var version = Environment.OSVersion.Version;
            // Windows 11 reports build >= 22000.
            return version.Major >= 10 && version.Build >= 22000;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Best-effort Mica-look for a WPF ContextMenu popup: DWM backdrops do not
    /// apply to layered popup windows, so we fall back to a translucent dark
    /// surface that blends with the desktop (acrylic-like).
    /// </summary>
    public static bool TryStyleContextMenu(System.Windows.Controls.ContextMenu menu, bool micaEnabled)
    {
        if (menu == null) return false;
        try
        {
            if (micaEnabled)
            {
                menu.Background = new SolidColorBrush(Color.FromArgb(0xF2, 0x2F, 0x2F, 0x2F));
                menu.BorderBrush = new SolidColorBrush(Color.FromArgb(0x40, 0xFF, 0xFF, 0xFF));
            }
            else
            {
                menu.ClearValue(System.Windows.Controls.Control.BackgroundProperty);
                menu.ClearValue(System.Windows.Controls.Control.BorderBrushProperty);
            }
            return true;
        }
        catch
        {
            return false;
        }
    }
}
