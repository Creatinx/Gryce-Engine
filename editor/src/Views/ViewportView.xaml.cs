using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using Microsoft.Web.WebView2.Core;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace GryceEngine.Editor.Views;

public partial class ViewportView : UserControl, IDisposable
{
    private ViewportHwndHost? _hwndHost;
    private GizmoOverlayWindow? _overlay;
    private DispatcherTimer? _gizmoTimer;
    private EditorViewModel? _vmCached;
    private System.Threading.Thread? _renderThread;
    private volatile bool _renderThreadRunning;
    // Set when the render surface is re-shown after being hidden (code-editor
    // tab): the render loop then forces a full surface recreate so a stale
    // Vulkan swapchain / destroyed GLFW child cannot freeze the viewport.
    private volatile bool _needRenderRecovery;
    private readonly object _cameraLock = new();
    private nint _renderHandle;
    private bool _rendererInitialized;
    // Set once the embedded GLFW surface exists and the renderer is
    // initialized; the render loop never touches GL before this is true.
    private volatile bool _renderSurfaceReady;
    private GRenderAPI _renderApi;
    private volatile bool _isGameView;
    private volatile bool _windowMinimized;
    private bool _is2DMode;
    private string _displayMode = "Shaded";

    // Script tab (Monaco via WebView2) state
    private bool _scriptMode;
    private bool _scriptWebReady;
    private bool _scriptLoadingContent;
    private string? _currentScriptPath;
    private string? _scriptInitialContent;

    /// <summary>Raised by the project panel to open a .lua file in the
    /// viewport's Script tab.</summary>
    public static Action<string>? OpenScriptRequested;

    /// <summary>True while the Game tab is active. EngineService uses this to
    /// sync editor-injected input into the core so Lua scripts can read it.</summary>
    public static volatile bool GameViewActive;

    /// <summary>Raised when the Script (code editor) tab becomes active/closed,
    /// so the main window can hide the scene-editor toolbar tools.</summary>
    public event Action<bool>? ScriptModeChanged;

    // ---- 2D 场景编辑器 ----
    private GEntityHandle _editor2DCamera = GEntityHandle.Null;
    private string? _pending2DCameraName;
    private bool _editor2DCameraCreated;
    private bool _2dCameraReady;
    private double _2dCenterX, _2dCenterY;
    private float _2dZoom = 1.0f;
    private readonly List<Line> _gridLines = new();
    private bool _gridBuilt;
    private bool _appliedShowStats;
    private (int W, int H) _appliedPixelSize;
    private double _overlayScale = -1.0;
    private DateTime _lastResizeTime = DateTime.MinValue;

    // ---- editor viewport interaction ----
    private readonly ViewportCamera _sceneCamera = new();
    private bool _leftDown, _rightDown, _middleDown;
    private double _lastMouseX, _lastMouseY;
    private double _trackedMouseX, _trackedMouseY;
    private readonly HashSet<Key> _heldKeys = new();
    private readonly HashSet<int> _nativeKeys = new();
    private volatile bool _pointerLocked;
    private bool _cursorHidden;
    private GEntityHandle _mainCamera = GEntityHandle.Null;
    private int _cameraResolveCounter;

    // ---- picking / gizmo drag ----
    private int _dragAxis = -1;          // 0=X, 1=Y, 2=Z
    private string _dragMode = "";       // "Translate" | "Rotate" | "Scale"
    private double _dragStartPosX, _dragStartPosY, _dragStartPosZ;
    private double _dragStartScaleX, _dragStartScaleY, _dragStartScaleZ;
    private double _dragStartScreenDist;
    private double _gizmoScreenX, _gizmoScreenY;
    private double _dragStartScreenX, _dragStartScreenY;
    private GQuat _dragStartRot;
    private bool _dragCaptured;

    // Batched gizmo transform: mouse events only compute the target and mark it
    // dirty; the render loop applies it once per frame so a high mouse rate
    // cannot flood the core with per-event writes and stall the UI.
    private readonly object _gizmoApplyLock = new();
    private bool _gizmoApplyDirty;
    private GEntityHandle _gizmoApplyEntity;
    private GVec3 _gizmoApplyPos;
    private GQuat _gizmoApplyRot;
    private GVec3 _gizmoApplyScale;
    private int _gizmoApplyMask; // 1=pos, 2=rot, 4=scale

    // ---- gizmo visuals (reused shapes) ----
    private readonly Line[] _axisLines = new Line[3];
    private readonly Polygon[] _axisHeads = new Polygon[3];
    private readonly Rectangle[] _axisBoxes = new Rectangle[3];
    private readonly Polyline[] _axisRings = new Polyline[3];
    private bool _gizmoShapesBuilt;
    public ViewportView()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        SizeChanged += OnSizeChanged;
        _sceneCamera.LookAtTarget(0, 2.5, 8, 0, 1, 0);
    }

    private EditorViewModel? VM => DataContext as EditorViewModel;

    /// <summary>
    /// Converts WPF layout size (DIP) to physical pixels.
    /// </summary>
    private (int W, int H) GetPixelSize(double? width = null, double? height = null)
    {
        // Prefer the GLFW child's actual client size: it is resized immediately
        // (host WM_SIZE -> SetWindowPos), while the host HWND client rect lags
        // a layout pass and would give the render thread a stale size, making
        // the picture appear not to change on window resize.
        var glfw = _hwndHost?.GlfwChildHandle ?? IntPtr.Zero;
        if (glfw != IntPtr.Zero && TryGetClientSize(glfw, out int gcw, out int gch) &&
            gcw > 0 && gch > 0)
        {
            return CapViewport(Math.Max(gcw, 100), Math.Max(gch, 100));
        }

        if (_hwndHost != null && _hwndHost.Handle != IntPtr.Zero)
        {
            if (TryGetHostClientSize(out int cw, out int ch) && cw > 0 && ch > 0)
            {
                return CapViewport(Math.Max(cw, 100), Math.Max(ch, 100));
            }
        }

        double w = width ?? (ViewportBorder != null && ViewportBorder.ActualWidth > 0
            ? ViewportBorder.ActualWidth : ActualWidth);
        double h = height ?? (ViewportBorder != null && ViewportBorder.ActualHeight > 0
            ? ViewportBorder.ActualHeight : ActualHeight);
        return CapViewport(Math.Max((int)Math.Round(w), 100),
                           Math.Max((int)Math.Round(h), 100));
    }

    /// <summary>Render target is capped at 1280x720: the editor viewport never
    /// renders above that, so a large panel just shows the 1280x720 frame.</summary>
    private static (int W, int H) CapViewport(int w, int h)
        => (Math.Min(w, 1280), Math.Min(h, 720));

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool GetClientRect(IntPtr hWnd, out NativeRect rect);

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    [return: System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr hWnd);

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left, Top, Right, Bottom;
    }

    private bool TryGetHostClientSize(out int w, out int h)
    {
        w = 0;
        h = 0;
        if (_hwndHost == null || _hwndHost.Handle == IntPtr.Zero) return false;
        return TryGetClientSize(_hwndHost.Handle, out w, out h);
    }

    private static bool TryGetClientSize(nint hwnd, out int w, out int h)
    {
        w = 0;
        h = 0;
        if (hwnd == IntPtr.Zero) return false;
        if (!GetClientRect(hwnd, out var r)) return false;
        w = r.Right - r.Left;
        h = r.Bottom - r.Top;
        return w > 0 && h > 0;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        _vmCached = DataContext as EditorViewModel;
        OpenScriptRequested += OnOpenScriptRequested;
        int w = Math.Max((int)ActualWidth, 100);
        int h = Math.Max((int)ActualHeight, 100);
        var px = GetPixelSize();

        _hwndHost = new ViewportHwndHost(w, h);
        _hwndHost.HwndCreated += OnHwndCreated;
        _hwndHost.NativeMouseMove += OnNativeMouseMove;
        _hwndHost.NativeMouseButton += OnNativeMouseButton;
        _hwndHost.NativeMouseWheel += OnNativeMouseWheel;
        _hwndHost.NativeKey += OnNativeKey;
        HostContainer.Content = _hwndHost;

        var win = Window.GetWindow(this);
        if (win != null)
        {
            win.PreviewKeyDown += OnWindowKeyDown;
            win.PreviewKeyUp += OnWindowKeyUp;
            win.Deactivated += OnWindowDeactivated;
            win.StateChanged += OnWindowStateChanged;
            win.LocationChanged += OnWindowLocationChanged;

            _overlay = new GizmoOverlayWindow { Owner = win };
            _overlay.Show();
            ViewportOverlayManager.Overlay = _overlay;

            if (_vmCached != null)
            {
                _vmCached.PropertyChanged += OnVmPropertyChanged;
            }
        }

        _gizmoTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromSeconds(1.0 / 30.0)
        };
        _gizmoTimer.Tick += OnGizmoTick;
        _gizmoTimer.Start();

        UpdateResolutionDisplay(px.W, px.H);
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        OpenScriptRequested -= OnOpenScriptRequested;
        StopRenderThread();
        _gizmoTimer?.Stop();
        _gizmoTimer = null;

        var win = Window.GetWindow(this);
        if (win != null)
        {
            win.PreviewKeyDown -= OnWindowKeyDown;
            win.PreviewKeyUp -= OnWindowKeyUp;
            win.Deactivated -= OnWindowDeactivated;
            win.StateChanged -= OnWindowStateChanged;
            win.LocationChanged -= OnWindowLocationChanged;
        }

        if (_vmCached != null)
        {
            _vmCached.PropertyChanged -= OnVmPropertyChanged;
        }

        if (ReferenceEquals(ViewportOverlayManager.Overlay, _overlay))
        {
            ViewportOverlayManager.Overlay = null;
        }
        _overlay?.Close();
        _overlay = null;

        if (_rendererInitialized)
        {
            try { RenderAPI.GRender_Shutdown(); } catch { }
            _rendererInitialized = false;
        }

        if (_hwndHost != null)
        {
            _hwndHost.HwndCreated -= OnHwndCreated;
            _hwndHost.NativeMouseMove -= OnNativeMouseMove;
            _hwndHost.NativeMouseButton -= OnNativeMouseButton;
            _hwndHost.NativeMouseWheel -= OnNativeMouseWheel;
            _hwndHost.NativeKey -= OnNativeKey;
            _hwndHost.Dispose();
            _hwndHost = null;
        }
    }

    private void OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        _lastResizeTime = DateTime.Now;
        // The embedded GLFW child is resized by the host WM_SIZE during the
        // layout pass, which fires after this event; defer the resolution read
        // until the layout settles so GetClientRect returns the final size.
        Dispatcher.BeginInvoke(new Action(() =>
        {
            var px = GetPixelSize();
            UpdateResolutionDisplay(px.W, px.H);
            PositionOverlay();
        }), DispatcherPriority.Loaded);
    }

    private void UpdateResolutionDisplay(int w, int h)
    {
        ViewportResolution.Text = $"{w}x{h}";
    }

    private void OnHwndCreated(object? sender, nint hwnd)
    {
        // Re-creation guard: the UI thread must never re-run the platform /
        // renderer init while the render thread owns the GL context (a second
        // GWindow_InitExternal destroys the old context under the render
        // thread and can hang it). With Visibility.Hidden the surface stays
        // alive across the Script tab, so this normally fires only once; if a
        // host rebuild still happens, re-attach the child instead of re-init.
        if (_rendererInitialized)
        {
            VM?.AppendConsole("[Viewport] host HWND recreated; skipping renderer re-init");
            _hwndHost?.ReattachGlfwChild();
            return;
        }

        var px = TryGetClientSize(hwnd, out int hw, out int hh)
            ? (W: hw, H: hh)
            : GetPixelSize();
        int w = px.W;
        int h = px.H;

        try
        {
            var settings = Services.ProjectSettingsService.Load();
            var renderApi = string.Equals(settings.RenderApi, "opengl",
                System.StringComparison.OrdinalIgnoreCase)
                ? GRenderAPI.OpenGL
                : GRenderAPI.Vulkan;
            _renderApi = renderApi;
            int result = renderApi == GRenderAPI.Vulkan
                ? WindowAPI.GWindow_InitExternalEx(new GWindowHandle(hwnd), w, h, renderApi)
                : WindowAPI.GWindow_InitExternal(new GWindowHandle(hwnd), w, h);
            if (result != 0)
            {
                VM?.AppendConsole("[Viewport] Failed to init external window");
                InitStatusText.Text = "Failed to initialize renderer window.";
                return;
            }

            // Subclass the GLFW child so native mouse/key messages reach the editor.
            _hwndHost?.AttachGlfwChild();

            var renderDesc = new GRenderInitDesc
            {
                Version = (uint)System.Runtime.InteropServices.Marshal.SizeOf<GRenderInitDesc>(),
                NativeWindow = new GWindowHandle(hwnd),
                Api = renderApi,
                ViewportW = w,
                ViewportH = h,
                SyncMode = true
            };

            int renderResult = RenderAPI.GRender_Init(ref renderDesc);
            if (renderResult != 0)
            {
                VM?.AppendConsole("[Viewport] Failed to init renderer");
                InitStatusText.Text = $"Renderer init failed (code {renderResult}).";
                return;
            }

            _rendererInitialized = true;
            _renderSurfaceReady = true;
            NoRendererMessage.Visibility = Visibility.Collapsed;
            InitStatusText.Text = "Renderer initialized.";
            VM?.AppendConsole("[Viewport] Renderer initialized successfully");

            // Move rendering to a dedicated thread so the UI thread (menu,
            // panels, input) is never blocked by GL work.
            _renderHandle = WindowAPI.GWindow_GetRenderHandle().Value;
            // Hand the GL context over cleanly: release it on the UI thread
            // before the render thread makes it current (mirrors the core's
            // async-mode handoff and prevents driver-level thread confusion).
            WindowAPI.GWindow_ReleaseContext();
            StartRenderThread();

            Dispatcher.BeginInvoke(new Action(() =>
            {
                var finalPx = GetPixelSize();
                UpdateResolutionDisplay(finalPx.W, finalPx.H);
            }), DispatcherPriority.Loaded);
        }
        catch (Exception ex)
        {
            VM?.AppendConsole($"[Viewport] Init error: {ex.Message}");
            InitStatusText.Text = $"Error: {ex.Message}";
        }
    }
    private static GQuat QuatFromLookAt(double fx, double fy, double fz)
    {
        double len = Math.Sqrt(fx * fx + fy * fy + fz * fz);
        if (len < 1e-9) return new GQuat(0, 0, 0, 1);
        fx /= len; fy /= len; fz /= len;

        double dot = -fz; // (0,0,-1) dot (fx,fy,fz)
        if (dot > 0.99999)
            return new GQuat(0, 0, 0, 1);
        if (dot < -0.99999)
            return new GQuat(0, 1, 0, 0); // 180° about Y

        // Rotation axis = cross((0,0,-1), F) = (fy, -fx, 0). The previous
        // implementation computed (0, fx, 0), which rotated the camera about
        // the wrong axis once the view pitched/orbited, so the rendered
        // MainCamera transform diverged from the editor camera.
        double ax = fy;
        double ay = -fx;
        double az = 0.0;
        double alen = Math.Sqrt(ax * ax + ay * ay + az * az);
        if (alen < 1e-9) return new GQuat(0, 0, 0, 1);
        ax /= alen; ay /= alen; az /= alen;

        double angle = Math.Acos(Clamp(dot, -1, 1));
        double s = Math.Sin(angle / 2.0);
        double c = Math.Cos(angle / 2.0);
        return new GQuat((float)(ax * s), (float)(ay * s), (float)(az * s), (float)c);
    }
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern int ShowCursor(bool bShow);

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern IntPtr SetCapture(IntPtr hWnd);

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool ReleaseCapture();

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool ClientToScreen(IntPtr hWnd, ref NativePoint pt);

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool SetCursorPos(int x, int y);

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);
    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct NativePoint
    {
        public int X;
        public int Y;
    }
}
