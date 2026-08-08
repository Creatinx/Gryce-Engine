using System;
using System.Runtime.InteropServices;

namespace GryceEngine.Editor.Services;

/// <summary>
/// Low-level mouse hook (WH_MOUSE_LL) used to capture viewport input reliably
/// even when unrelated top-level windows overlap the editor window. Messages
/// inside the active viewport rect are swallowed and forwarded to the editor.
/// </summary>
public sealed class GlobalMouseHook : IDisposable
{
    private const int WH_MOUSE_LL = 14;
    private const int WM_MOUSEMOVE = 0x0200;
    private const int WM_LBUTTONDOWN = 0x0201;
    private const int WM_LBUTTONUP = 0x0202;
    private const int WM_RBUTTONDOWN = 0x0204;
    private const int WM_RBUTTONUP = 0x0205;
    private const int WM_MBUTTONDOWN = 0x0207;
    private const int WM_MBUTTONUP = 0x0208;
    private const int WM_MOUSEWHEEL = 0x020A;

    private readonly HookProc _proc;
    private IntPtr _hook;
    private bool _captured;

    // Active capture region in physical screen pixels.
    public int RegionLeft { get; set; }
    public int RegionTop { get; set; }
    public int RegionRight { get; set; }
    public int RegionBottom { get; set; }

    public event Action<int, int>? ScreenMouseMove;
    public event Action<int, bool, int, int>? ScreenMouseButton;
    public event Action<int>? MouseWheel;

    /// <summary>Currently held mouse buttons (1=left, 2=right, 4=middle).</summary>
    public int CurrentButtons { get; private set; }

    public GlobalMouseHook()
    {
        _proc = HookCallback;
    }

    public bool Start()
    {
        if (_hook != IntPtr.Zero) return true;
        using var cur = System.Diagnostics.Process.GetCurrentProcess();
        using var module = cur.MainModule;
        _hook = SetWindowsHookEx(WH_MOUSE_LL, _proc, GetModuleHandle(module?.ModuleName), 0);
        return _hook != IntPtr.Zero;
    }

    private IntPtr HookCallback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0)
        {
            try
            {
                var info = Marshal.PtrToStructure<MSLLHOOKSTRUCT>(lParam);
                int x = info.pt.x;
                int y = info.pt.y;
                bool inside = x >= RegionLeft && x <= RegionRight && y >= RegionTop && y <= RegionBottom;
                int msg = (int)wParam;

                switch (msg)
                {
                    case WM_LBUTTONDOWN:
                    case WM_RBUTTONDOWN:
                    case WM_MBUTTONDOWN:
                        CurrentButtons |= ToButtonFlag(msg);
                        if (inside) _captured = true;
                        if (_captured)
                        {
                            ScreenMouseButton?.Invoke(ToButton(msg), true, x, y);
                            return (IntPtr)1;
                        }
                        break;
                    case WM_LBUTTONUP:
                    case WM_RBUTTONUP:
                    case WM_MBUTTONUP:
                        CurrentButtons &= ~ToButtonFlag(msg);
                        if (_captured)
                        {
                            ScreenMouseButton?.Invoke(ToButton(msg), false, x, y);
                            if (CurrentButtons == 0) _captured = false;
                            return (IntPtr)1;
                        }
                        break;
                    case WM_MOUSEMOVE:
                        if (inside && CurrentButtons != 0 && !_captured) _captured = true;
                        if (_captured || inside)
                        {
                            ScreenMouseMove?.Invoke(x, y);
                            return (IntPtr)1; // swallow
                        }
                        break;
                    case WM_MOUSEWHEEL:
                        if (_captured || inside)
                        {
                            MouseWheel?.Invoke((short)(info.mouseData >> 16));
                            return (IntPtr)1;
                        }
                        break;
                }
            }
            catch
            {
                // Never let a handler exception crash the process from inside a hook.
            }
        }
        return CallNextHookEx(_hook, nCode, wParam, lParam);
    }

    private static int ToButton(int msg)
    {
        switch (msg)
        {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP: return 0;
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP: return 1;
            default: return 2;
        }
    }

    private static int ToButtonFlag(int msg)
    {
        switch (msg)
        {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP: return 1;
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP: return 2;
            default: return 4;
        }
    }

    public void Dispose()
    {
        if (_hook != IntPtr.Zero)
        {
            UnhookWindowsHookEx(_hook);
            _hook = IntPtr.Zero;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT
    {
        public int x, y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MSLLHOOKSTRUCT
    {
        public POINT pt;
        public uint mouseData;
        public uint flags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    private delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowsHookEx(int idHook, HookProc lpfn, IntPtr hMod, uint dwThreadId);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnhookWindowsHookEx(IntPtr hhk);

    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr GetModuleHandle(string? lpModuleName);
}
