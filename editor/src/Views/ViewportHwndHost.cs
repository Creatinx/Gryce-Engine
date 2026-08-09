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
    private IntPtr _oldHostWndProc;

    /// <summary>HWND of the embedded GLFW render window (valid after AttachGlfwChild).</summary>
    public IntPtr GlfwChildHandle => _glfwChild;
    private static readonly Dictionary<IntPtr, ViewportHwndHost> s_hosts = new();
    private static readonly Dictionary<IntPtr, ViewportHwndHost> s_hostProcs = new();
    private static readonly WndProcDelegate s_proc = WndProc;
    private static readonly WndProcDelegate s_hostProc = HostWndProc;

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
        // Subclass the host window so the embedded GLFW child can be resized
        // in lockstep with the host: WPF resizes the host HWND during layout,
        // and its WM_SIZE carries the exact client size, which we apply to the
        // GLFW child immediately (no GetClientRect timing races, no lag while
        // moving/resizing the editor window).
        _oldHostWndProc = SetWindowLongPtr(_hwnd, GWLP_WNDPROC, Marshal.GetFunctionPointerForDelegate(s_hostProc));
        if (_oldHostWndProc != IntPtr.Zero)
        {
            s_hostProcs[_hwnd] = this;
        }
        HwndCreated?.Invoke(this, _hwnd);
        return new HandleRef(this, _hwnd);
    }

    protected override void DestroyWindowCore(HandleRef hwnd)
    {
        if (_hwnd != IntPtr.Zero && _oldHostWndProc != IntPtr.Zero)
        {
            SetWindowLongPtr(_hwnd, GWLP_WNDPROC, _oldHostWndProc);
            s_hostProcs.Remove(_hwnd);
            _oldHostWndProc = IntPtr.Zero;
        }
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

        // Match the GLFW child to the host's current client size (the host
        // WM_SIZE handler keeps it in sync afterwards).
        if (TryGetClientSize(_hwnd, out int cw, out int ch))
        {
            SetWindowPos(_glfwChild, IntPtr.Zero, 0, 0, cw, ch,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    /// <summary>Host WndProc: on WM_SIZE, resize the embedded GLFW child to the
    /// host's client size so the rendered viewport tracks the window frame
    /// during move/resize without lag.</summary>
    private static IntPtr HostWndProc(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam)
    {
        if (s_hostProcs.TryGetValue(hWnd, out var host))
        {
            if (msg == 0x0005) // WM_SIZE
            {
                int w = (int)((long)lParam & 0xFFFF);
                int h = (int)(((long)lParam >> 16) & 0xFFFF);
                host.ResizeGlfwChild(w, h);
            }
        }
        return CallWindowProc(host?._oldHostWndProc ?? IntPtr.Zero, hWnd, msg, wParam, lParam);
    }

    private void ResizeGlfwChild(int w, int h)
    {
        if (_glfwChild == IntPtr.Zero || w <= 0 || h <= 0) return;
        // Render target is capped at 1280x720; larger hosts just show the
        // fixed-size frame in the top-left of the panel.
        int cw = Math.Min(w, 1280);
        int ch = Math.Min(h, 720);
        SetWindowPos(_glfwChild, IntPtr.Zero, 0, 0, cw, ch,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    }

    private static IntPtr WndProc(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam)
    {
        if (s_hosts.TryGetValue(hWnd, out var host))
        {
            switch (msg)
            {
                case 0x0200: // WM_MOUSEMOVE
                    // lParam carries signed 16-bit client coordinates. Without
                    // the sign conversion a captured drag that leaves the
                    // viewport wraps to ~65535 and the camera "teleports".
                    host.NativeMouseMove?.Invoke(SignedLo(lParam), SignedHi(lParam));
                    break;
                case 0x0201: // WM_LBUTTONDOWN
                case 0x0204: // WM_RBUTTONDOWN
                case 0x0207: // WM_MBUTTONDOWN
                    host.NativeMouseButton?.Invoke(ToButton(msg), true, SignedLo(lParam), SignedHi(lParam));
                    break;
                case 0x0202: // WM_LBUTTONUP
                case 0x0205: // WM_RBUTTONUP
                case 0x0208: // WM_MBUTTONUP
                    host.NativeMouseButton?.Invoke(ToButton(msg), false, SignedLo(lParam), SignedHi(lParam));
                    break;
                case 0x020A: // WM_MOUSEWHEEL
                    host.NativeMouseWheel?.Invoke(SignedHi(wParam));
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

    private static int SignedLo(IntPtr v) => (short)((long)v & 0xFFFF);
    private static int SignedHi(IntPtr v) => (short)(((long)v >> 16) & 0xFFFF);

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

    [DllImport("user32.dll")]
    private static extern bool GetClientRect(IntPtr hWnd, out NativeRect rect);

    [DllImport("user32.dll")]
    private static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter,
        int x, int y, int cx, int cy, uint flags);

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left, Top, Right, Bottom;
    }

    private static bool TryGetClientSize(nint hwnd, out int w, out int h)
    {
        w = 0;
        h = 0;
        if (hwnd == IntPtr.Zero || !GetClientRect(hwnd, out var r)) return false;
        w = r.Right - r.Left;
        h = r.Bottom - r.Top;
        return w > 0 && h > 0;
    }

    private const uint SWP_NOZORDER = 0x0004;
    private const uint SWP_NOACTIVATE = 0x0010;
    private const uint SWP_NOCOPYBITS = 0x0100;
}
