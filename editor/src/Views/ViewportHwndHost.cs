using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;

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

    // Raw native events forwarded from the embedded GLFW child window.
    public event Action<double, double>? NativeMouseMove;
    public event Action<int, bool, double, double>? NativeMouseButton;
    public event Action<int>? NativeMouseWheel;
    public event Action<int, bool>? NativeKey;

    private IntPtr _glfwChild;
    private IntPtr _oldWndProc;
    private static readonly Dictionary<IntPtr, ViewportHwndHost> s_hosts = new();
    private static readonly WndProcDelegate s_proc = WndProc;

    private delegate IntPtr WndProcDelegate(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

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
        if (_glfwChild != IntPtr.Zero && _oldWndProc != IntPtr.Zero)
        {
            SetWindowLongPtr(_glfwChild, GWLP_WNDPROC, _oldWndProc);
            s_hosts.Remove(_glfwChild);
            _glfwChild = IntPtr.Zero;
        }
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

    /// <summary>
    /// Subclasses the embedded GLFW child window so native mouse/key messages
    /// are forwarded to the editor (WPF HwndHost events never fire because the
    /// GLFW child covers the host).
    /// </summary>
    public void AttachGlfwChild()
    {
        _glfwChild = FindGlfwChild(_hwnd);
        if (_glfwChild == IntPtr.Zero) return;

        // GLFW creates its child window with WS_EX_TRANSPARENT, which makes
        // hit-testing skip it (clicks fall through to the host). Clear it so
        // the viewport receives mouse input.
        int ex = GetWindowLong(_glfwChild, GWL_EXSTYLE);
        ex &= ~WS_EX_TRANSPARENT;
        SetWindowLong(_glfwChild, GWL_EXSTYLE, ex);

        _oldWndProc = SetWindowLongPtr(_glfwChild, GWLP_WNDPROC, Marshal.GetFunctionPointerForDelegate(s_proc));
        if (_oldWndProc != IntPtr.Zero)
        {
            s_hosts[_glfwChild] = this;
        }
    }

    private static IntPtr WndProc(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam)
    {
        if (s_hosts.TryGetValue(hWnd, out var host))
        {
            switch (msg)
            {
                case 0x0200: // WM_MOUSEMOVE
                    host.NativeMouseMove?.Invoke(LoWord(lParam), HiWord(lParam));
                    break;
                case 0x0201: // WM_LBUTTONDOWN
                case 0x0204: // WM_RBUTTONDOWN
                case 0x0207: // WM_MBUTTONDOWN
                    host.NativeMouseButton?.Invoke(ToButton(msg), true, LoWord(lParam), HiWord(lParam));
                    break;
                case 0x0202: // WM_LBUTTONUP
                case 0x0205: // WM_RBUTTONUP
                case 0x0208: // WM_MBUTTONUP
                    host.NativeMouseButton?.Invoke(ToButton(msg), false, LoWord(lParam), HiWord(lParam));
                    break;
                case 0x020A: // WM_MOUSEWHEEL
                    host.NativeMouseWheel?.Invoke((short)HiWord(wParam));
                    break;
                case 0x0100: // WM_KEYDOWN
                    host.NativeKey?.Invoke((int)wParam, true);
                    break;
                case 0x0101: // WM_KEYUP
                    host.NativeKey?.Invoke((int)wParam, false);
                    break;
            }
        }
        return CallWindowProc(host?._oldWndProc ?? IntPtr.Zero, hWnd, msg, wParam, lParam);
    }

    private static int ToButton(int msg)
    {
        switch (msg)
        {
            case 0x0201: return 0; // left
            case 0x0202: return 0;
            case 0x0204: return 1; // right
            case 0x0205: return 1;
            case 0x0207: return 2; // middle
            case 0x0208: return 2;
            default: return 0;
        }
    }

    private static int LoWord(IntPtr v) => (int)((long)v & 0xFFFF);
    private static int HiWord(IntPtr v) => (int)(((long)v >> 16) & 0xFFFF);

    private static IntPtr FindGlfwChild(IntPtr parent)
    {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, l) =>
        {
            var cls = new StringBuilder(128);
            GetClassName(h, cls, cls.Capacity);
            string name = cls.ToString();
            if (name.Contains("GLFW") || name.StartsWith("STATIC", StringComparison.OrdinalIgnoreCase))
            {
                found = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr CreateWindowEx(int dwExStyle, string lpClassName, string lpWindowName,
        int dwStyle, int x, int y, int nWidth, int nHeight,
        IntPtr hWndParent, IntPtr hMenu, IntPtr hInstance, IntPtr lpParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool DestroyWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr lParam);

    private delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    private static extern IntPtr SetWindowLongPtr(IntPtr hWnd, int nIndex, IntPtr dwNewLong);

    [DllImport("user32.dll")]
    private static extern IntPtr CallWindowProc(IntPtr lpPrevWndFunc, IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    private const int GWLP_WNDPROC = -4;
    private const int GWL_EXSTYLE = -20;
    private const int WS_EX_TRANSPARENT = 0x00000020;

    [DllImport("user32.dll")]
    private static extern int GetWindowLong(IntPtr hWnd, int nIndex);

    [DllImport("user32.dll")]
    private static extern int SetWindowLong(IntPtr hWnd, int nIndex, int dwNewLong);
}
