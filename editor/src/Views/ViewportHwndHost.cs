using System.Windows.Interop;
using System.Windows;
using System.Runtime.InteropServices;
using System.Windows.Input;

namespace GryceEngine.Editor.Views;

/// <summary>Hosts a native Win32 HWND child window for the renderer.</summary>
public class ViewportHwndHost : HwndHost
{
    private IntPtr _hwnd;
    private readonly int _width;
    private readonly int _height;

    public event EventHandler<IntPtr>? HwndCreated;
    public new event EventHandler<MouseEventArgs>? MouseMove;
    public new event EventHandler<MouseButtonEventArgs>? MouseDown;
    public new event EventHandler<MouseButtonEventArgs>? MouseUp;
    public new event EventHandler<MouseWheelEventArgs>? MouseWheel;

    public ViewportHwndHost(int width, int height)
    {
        _width = width;
        _height = height;
    }

    protected override HandleRef BuildWindowCore(HandleRef hwndParent)
    {
        // WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS
        const int style = 0x40000000 | 0x10000000 | 0x02000000 | 0x04000000;
        _hwnd = CreateWindowEx(0, "static", "", style,
            0, 0, _width, _height,
            hwndParent.Handle, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero);
        HwndCreated?.Invoke(this, _hwnd);
        return new HandleRef(this, _hwnd);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
        if (_hwnd != IntPtr.Zero)
        {
            DestroyWindow(_hwnd);
            _hwnd = IntPtr.Zero;
        }
    }

    // Forward mouse events from the parent WPF control
    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        MouseMove?.Invoke(this, e);
    }

    protected override void OnMouseDown(MouseButtonEventArgs e)
    {
        base.OnMouseDown(e);
        MouseDown?.Invoke(this, e);
    }

    protected override void OnMouseUp(MouseButtonEventArgs e)
    {
        base.OnMouseUp(e);
        MouseUp?.Invoke(this, e);
    }

    protected override void OnMouseWheel(MouseWheelEventArgs e)
    {
        base.OnMouseWheel(e);
        MouseWheel?.Invoke(this, e);
    }

    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr CreateWindowEx(int dwExStyle, string lpClassName, string lpWindowName,
        int dwStyle, int x, int y, int nWidth, int nHeight,
        IntPtr hWndParent, IntPtr hMenu, IntPtr hInstance, IntPtr lpParam);

    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    private static extern bool DestroyWindow(IntPtr hwnd);
}
