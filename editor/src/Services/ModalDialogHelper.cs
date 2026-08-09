using System.Windows;
using GryceEngine.Editor.Views;

namespace GryceEngine.Editor.Services;

/// <summary>
/// Tracks the floating gizmo overlay window so that while a modal dialog is
/// open the overlay is hidden. The overlay is a transparent top-level window
/// that floats over the native GLFW viewport; if a dialog is positioned over
/// that area, the overlay must not stay above it (WPF owned-window z-order can
/// leave the non-activated overlay covering the dialog).
/// </summary>
public static class ViewportOverlayManager
{
    private static readonly object Sync = new();
    private static int _suppressCount;

    /// <summary>The transparent gizmo overlay owned by the main window.</summary>
    public static Window? Overlay { get; set; }

    public static void Suppress()
    {
        lock (Sync)
        {
            if (Overlay == null) return;
            if (_suppressCount++ == 0)
            {
                if (Overlay is GizmoOverlayWindow gizmo)
                {
                    gizmo.Suppressed = true;
                }
                if (Overlay.IsVisible)
                {
                    Overlay.Hide();
                }
            }
        }
    }

    public static void Restore()
    {
        lock (Sync)
        {
            if (Overlay == null || _suppressCount <= 0) return;
            if (--_suppressCount == 0)
            {
                if (Overlay is GizmoOverlayWindow gizmo)
                {
                    gizmo.Suppressed = false;
                }
                if (!Overlay.IsVisible)
                {
                    Overlay.Show();
                }
            }
        }
    }
}

/// <summary>
/// Shows editor modal dialogs with the floating viewport overlay suppressed
/// for the duration of the modal session, so rendered content can never cover
/// the dialog.
/// </summary>
public static class ModalDialog
{
    public static bool? Show(Window dialog, Window? owner)
    {
        if (owner != null)
        {
            dialog.Owner = owner;
        }
        ViewportOverlayManager.Suppress();
        try
        {
            return dialog.ShowDialog();
        }
        finally
        {
            ViewportOverlayManager.Restore();
        }
    }
}
