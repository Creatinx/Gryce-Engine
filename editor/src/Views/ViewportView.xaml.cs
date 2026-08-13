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
    private readonly ViewportCamera _gameCamera = new();
    private bool _leftDown, _rightDown, _middleDown;
    private double _lastMouseX, _lastMouseY;
    private double _trackedMouseX, _trackedMouseY;
    private readonly HashSet<Key> _heldKeys = new();
    private readonly HashSet<int> _nativeKeys = new();
    private volatile bool _pointerLocked;
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

    private static readonly Color[] AxisColors =
    {
        Color.FromRgb(0xFF, 0x45, 0x45), // X red
        Color.FromRgb(0x45, 0xFF, 0x45), // Y green
        Color.FromRgb(0x45, 0x8C, 0xFF)  // Z blue
    };

    public ViewportView()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        SizeChanged += OnSizeChanged;
        _sceneCamera.LookAtTarget(0, 2.5, 8, 0, 1, 0);
        _gameCamera.Reset(0, 2.5, 8, -90, 0);
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

    private void OnGizmoTick(object? sender, EventArgs e)
    {
        if (!_rendererInitialized) return;
        try
        {
            if (_is2DMode)
            {
                Update2DCameraSetup();
                Update2DGrid();
            }
            var es = App.Engine?.EditorSettings;
            if (es != null && es.ShowStats != _appliedShowStats)
            {
                _appliedShowStats = es.ShowStats;
                FpsCounter.Visibility = _appliedShowStats ? Visibility.Visible : Visibility.Collapsed;
            }
            UpdateGizmoOverlay();
            // Refresh the inspector transform fields at the UI rate while a
            // gizmo drag is in progress (the render loop applies the values).
            if (_dragCaptured)
            {
                GEntityHandle h;
                lock (_gizmoApplyLock) { h = _gizmoApplyEntity; }
                if (h != GEntityHandle.Null) _vmCached?.RaiseTransformChanged(h);
            }
        }
        catch { /* best-effort overlay refresh */ }
    }

    private void StartRenderThread()
    {
        if (_renderThread != null && _renderThread.IsAlive) return;
        _renderThreadRunning = true;
        _renderThread = new System.Threading.Thread(RenderLoop)
        {
            IsBackground = true,
            Name = "GryceRender"
        };
        _renderThread.Start();
    }

    private void StopRenderThread()
    {
        _renderThreadRunning = false;
        var thread = _renderThread;
        _renderThread = null;
        thread?.Join(1000);
    }

    /// <summary>Logs from the render thread: console output is marshaled to
    /// the UI thread (AppendConsole touches WPF collections).</summary>
    private void LogFromRenderThread(string message)
    {
        try
        {
            Dispatcher.BeginInvoke(new Action(() => _vmCached?.AppendConsole(message)),
                                   DispatcherPriority.Background);
        }
        catch { /* app is shutting down */ }
    }

    /// <summary>
    /// Dedicated render loop: GLFW context is made current here and the frame
    /// is produced at the monitor refresh rate (vsync on a 240Hz display
    /// yields 240 FPS). Game-mode fly movement is integrated once per rendered
    /// frame so motion stays smooth instead of stepping at the UI timer rate.
    /// The UI thread stays free for menus/panels.
    /// </summary>
    private void RenderLoop()
    {
        int frames = 0;
        var sw = Stopwatch.StartNew();
        double lastTick = sw.Elapsed.TotalSeconds;
        var frameSw = Stopwatch.StartNew();
        var resizeSw = Stopwatch.StartNew();
        try
        {
            // The core owns the single GLFW instance; taking the context here
            // guarantees the same instance the renderer was initialized with,
            // regardless of Debug/Release or toolchain layout.
            WindowAPI.GWindow_MakeContextCurrent();
            // vsync paces the loop at the monitor refresh rate (240 FPS on a
            // 240Hz display) with zero CPU spin. GRender_EndFrame now swaps
            // OUTSIDE the API lock, so even if a driver/display stall blocks
            // glfwSwapBuffers, the editor UI thread stays fully responsive.
            WindowAPI.GWindow_SetSwapInterval(1);
            while (_renderThreadRunning)
            {
                // Never touch GL before the embedded surface is ready (e.g.
                // during startup or a rebuild): rendering on a destroyed or
                // half-created context makes the driver stall and the
                // viewport freeze.
                if (!_renderSurfaceReady)
                {
                    System.Threading.Thread.Sleep(16);
                    continue;
                }

                frameSw.Restart();
                double now = sw.Elapsed.TotalSeconds;
                double dt = Math.Min(now - lastTick, 0.05);
                lastTick = now;

                // Resize GL render targets on this thread (the owner of the GL
                // context), reading the host's live client size. The embedded
                // GLFW child window is already resized in lockstep by the host
                // subclass; only the GPU targets lag here, throttled to ~12Hz
                // so interactive resizing does not churn allocations per frame.
                var livePx = GetPixelSize();
                if (livePx != _appliedPixelSize && resizeSw.ElapsedMilliseconds >= 16)
                {
                    try
                    {
                        ViewportAPI.GViewport_SetSize(livePx.W, livePx.H);
                        _appliedPixelSize = livePx;
                        resizeSw.Restart();
                    }
                    catch { /* ignore transient resize errors */ }
                }

                if (_isGameView) UpdateGameFlyCamera(dt);

                if (_windowMinimized)
                {
                    // While minimized, skip GL work entirely: avoids vsync
                    // swap stalls on hidden windows and saves CPU/GPU.
                    System.Threading.Thread.Sleep(16);
                    continue;
                }

                if (_scriptMode)
                {
                    // Script (code editor) tab active: the render surface is
                    // hidden behind WebView2, so stop producing frames. This
                    // avoids rendering to a hidden GLFW window and any GPU
                    // contention with the WebView2 compositor, and the next
                    // Scene tab click resumes rendering with fresh camera/light
                    // state (re-collected every frame).
                    System.Threading.Thread.Sleep(16);
                    continue;
                }

                // Coming back from the code-editor tab: the GLFW child may
                // have been hidden (or destroyed) while HostContainer was
                // collapsed, and a Vulkan swapchain can go stale even when the
                // window itself survives. Force a same-backend surface
                // recreate; the liveHandle check below then re-attaches the
                // fresh child window and re-takes the context.
                if (_needRenderRecovery)
                {
                    _needRenderRecovery = false;
                    _appliedPixelSize = default;
                    try { RenderAPI.GRender_RequestSurfaceRecreate(); }
                    catch { /* recovery runs on next frame */ }
                }

                // After a render-backend switch the core recreates the embedded
                // GLFW window; re-attach the host subclass and take the new
                // context on this thread.
                var liveHandle = WindowAPI.GWindow_GetRenderHandle().Value;
                if (liveHandle != 0 && liveHandle != _renderHandle)
                {
                    _renderHandle = liveHandle;
                    _hwndHost?.ReattachGlfwChild();
                    WindowAPI.GWindow_MakeContextCurrent();
                    _appliedPixelSize = default;
                }

                frames++;
                PushSharedCamera();
                ApplyPendingGizmoTransform();

                RenderAPI.GRender_BeginFrame();
                if (_isGameView) RenderAPI.GRender_RenderGameView();
                else
                {
                    RenderAPI.GRender_RenderWorld();
                    RenderAPI.GRender_RenderGizmo();
                }
                RenderAPI.GRender_EndFrame();

                if (sw.ElapsedMilliseconds >= 1000)
                {
                    int fps = frames;
                    frames = 0;
                    sw.Restart();
                    Dispatcher.BeginInvoke(new Action(() =>
                    {
                        FpsCounter.Text = $"{fps} FPS";
                    }), DispatcherPriority.Background);
                }
            }
        }
        catch (Exception ex)
        {
            LogFromRenderThread($"[Viewport] Render thread error: {ex.Message}");
        }
        finally
        {
            try { WindowAPI.GWindow_ReleaseContext(); } catch { }
            _renderThreadRunning = false;
        }
    }

    private void PushSharedCamera()
    {
        if (_is2DMode) return; // 2D 画布不受 3D 相机影响
        if (++_cameraResolveCounter >= 120 || _mainCamera == GEntityHandle.Null)
        {
            _mainCamera = FindMainCamera();
            _cameraResolveCounter = 0;
        }
        if (_mainCamera == GEntityHandle.Null) return;
        double px, py, pz, fwdX, fwdY, fwdZ;
        lock (_cameraLock)
        {
            var cam = _isGameView ? _gameCamera : _sceneCamera;
            px = cam.PositionX; py = cam.PositionY; pz = cam.PositionZ;
            fwdX = cam.ForwardX; fwdY = cam.ForwardY; fwdZ = cam.ForwardZ;
        }
        try
        {
            var pos = new GVec3((float)px, (float)py, (float)pz);
            EntityAPI.GEntity_SetLocalPosition(_mainCamera, ref pos);
            var q = QuatFromLookAt(fwdX, fwdY, fwdZ);
            EntityAPI.GEntity_SetLocalRotation(_mainCamera, ref q);
        }
        catch { /* ignore */ }
    }

    private void UpdateGameFlyCamera(double dt)
    {
        // Poll the physical keyboard on the render thread so fly movement is
        // integrated at render frequency, not at the UI timer rate.
        bool w = KeyHeld(0x57);
        bool s = KeyHeld(0x53);
        bool a = KeyHeld(0x41);
        bool d = KeyHeld(0x44);
        bool space = KeyHeld(0x20);
        bool ctrl = KeyHeld(0x11);
        bool shift = KeyHeld(0x10);

        double fwd = (w ? 1 : 0) - (s ? 1 : 0);
        double strafe = (d ? 1 : 0) - (a ? 1 : 0);
        double up = (space ? 1 : 0) - (ctrl ? 1 : 0);
        lock (_cameraLock)
        {
            double baseSpeed = _gameCamera.FlyMoveSpeed;
            if (shift) _gameCamera.FlyMoveSpeed = baseSpeed * 2.5;
            _gameCamera.FlyMove(fwd, strafe, up, dt);
            _gameCamera.FlyMoveSpeed = baseSpeed;
        }
    }

    /// <summary>
    /// Applies tracked mouse input inside the render tick so camera movement
    /// stays in sync with rendering (no teleporting when input and render race).
    /// </summary>
    private void ProcessViewportInput()
    {
        if (_isGameView)
        {
            if (_pointerLocked)
            {
                double lookDx = _trackedMouseX - _lastMouseX;
                double lookDy = _trackedMouseY - _lastMouseY;
                _lastMouseX = _trackedMouseX;
                _lastMouseY = _trackedMouseY;
                _gameCamera.Look(lookDx, lookDy);
            }
            return;
        }

        double x = _trackedMouseX, y = _trackedMouseY;
        double dx = x - _lastMouseX;
        double dy = y - _lastMouseY;
        _lastMouseX = x;
        _lastMouseY = y;

        if (_is2DMode)
        {
            if (_leftDown && _dragCaptured) UpdateGizmoDrag(x, y);
            else if (_middleDown || _rightDown) Pan2DView(dx, dy);
        }
        else
        {
            if (_rightDown) _sceneCamera.Orbit(dx, dy);
            else if (_middleDown) _sceneCamera.Pan(dx, dy);
            else if (_leftDown && _dragCaptured) UpdateGizmoDrag(x, y);
        }
    }

    // =====================================================================
    //  Editor camera → scene MainCamera
    // =====================================================================

    private void UpdateViewportCamera()
    {
        if (_is2DMode) return; // 2D 画布不受 3D 相机影响
        if (++_cameraResolveCounter >= 60 || _mainCamera == GEntityHandle.Null)
        {
            _mainCamera = FindMainCamera();
            _cameraResolveCounter = 0;
        }
        if (_mainCamera == GEntityHandle.Null) return;

        var cam = _isGameView ? _gameCamera : _sceneCamera;
        try
        {
            var pos = new GVec3((float)cam.PositionX, (float)cam.PositionY, (float)cam.PositionZ);
            EntityAPI.GEntity_SetLocalPosition(_mainCamera, ref pos);

            var q = QuatFromLookAt(cam.ForwardX, cam.ForwardY, cam.ForwardZ);
            EntityAPI.GEntity_SetLocalRotation(_mainCamera, ref q);
        }
        catch { /* ignore */ }
    }

    private GEntityHandle FindMainCamera()
    {
        try
        {
            int count = EntityAPI.GEntity_GetCount();
            for (int i = 0; i < count; i++)
            {
                var h = EntityAPI.GEntity_GetAt(i);
                if (h == GEntityHandle.Null) continue;
                var name = EntityAPI.GetNameUtf8(h);
                if (name != null &&
                    (name == "MainCamera" || name.Contains("Camera")))
                {
                    return h;
                }
            }
        }
        catch { /* ignore */ }
        return GEntityHandle.Null;
    }

    /// <summary>Builds a quaternion that rotates (0,0,-1) onto the given forward.</summary>
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

    // =====================================================================
    //  Mouse input
    // =====================================================================

    private void OnViewportMouseMove(object? sender, MouseEventArgs e)
    {
        if (!_rendererInitialized) return;
        var pos = e.GetPosition(HostContainer);
        double dx = pos.X - _lastMouseX;
        double dy = pos.Y - _lastMouseY;
        _lastMouseX = pos.X;
        _lastMouseY = pos.Y;

        InputAPI.GInput_InjectMouseMove((float)pos.X, (float)pos.Y);

        if (_isGameView)
        {
            if (_pointerLocked)
            {
                _gameCamera.Look(dx, dy);
            }
            return;
        }

        // Scene mode
        if (_is2DMode)
        {
            if (_leftDown && _dragCaptured) UpdateGizmoDrag(pos.X, pos.Y);
            else if (_middleDown || _rightDown) Pan2DView(dx, dy);
        }
        else
        {
            if (_rightDown)
            {
                _sceneCamera.Orbit(dx, dy);
            }
            else if (_middleDown)
            {
                _sceneCamera.Pan(dx, dy);
            }
            else if (_leftDown && _dragCaptured)
            {
                UpdateGizmoDrag(pos.X, pos.Y);
            }
        }
    }

    private void OnViewportMouseDown(object? sender, MouseButtonEventArgs e)
    {
        if (!_rendererInitialized) return;
        var pos = e.GetPosition(HostContainer);
        _lastMouseX = pos.X;
        _lastMouseY = pos.Y;
        InputAPI.GInput_InjectMouseButton((int)e.ChangedButton, GInputAction.Press, (float)pos.X, (float)pos.Y);

        if (_isGameView)
        {
            _pointerLocked = true;
            return;
        }

        switch (e.ChangedButton)
        {
            case MouseButton.Right: _rightDown = true; break;
            case MouseButton.Middle: _middleDown = true; break;
            case MouseButton.Left:
                _leftDown = true;
                TryStartGizmoDrag(pos.X, pos.Y);
                break;
        }
    }

    private void OnViewportMouseUp(object? sender, MouseButtonEventArgs e)
    {
        if (!_rendererInitialized) return;
        var pos = e.GetPosition(HostContainer);
        InputAPI.GInput_InjectMouseButton((int)e.ChangedButton, GInputAction.Release, (float)pos.X, (float)pos.Y);

        if (_isGameView)
        {
            _pointerLocked = false;
            return;
        }

        switch (e.ChangedButton)
        {
            case MouseButton.Right: _rightDown = false; break;
            case MouseButton.Middle: _middleDown = false; break;
            case MouseButton.Left:
                if (_leftDown && !_dragCaptured)
                {
                    PickEntity(pos.X, pos.Y);
                }
                _leftDown = false;
                EndGizmoDrag();
                break;
        }
    }

    private void OnViewportMouseWheel(object? sender, MouseWheelEventArgs e)
    {
        if (!_rendererInitialized) return;
        InputAPI.GInput_InjectMouseScroll(0, e.Delta);
        if (_isGameView)
        {
            _gameCamera.FlyMoveSpeed = Clamp(_gameCamera.FlyMoveSpeed * (e.Delta > 0 ? 1.1 : 0.9), 0.5, 100);
        }
        else
        {
            if (_is2DMode)
                Zoom2DView(e.Delta);
            else
                _sceneCamera.Zoom(e.Delta);
        }
    }

    // =====================================================================
    //  Native input from the embedded GLFW child window (subclassed)
    // =====================================================================

    private void OnNativeMouseMove(double x, double y)
    {
        if (!_rendererInitialized) return;
        double dx = x - _lastMouseX;
        double dy = y - _lastMouseY;
        _lastMouseX = x;
        _lastMouseY = y;
        InputAPI.GInput_InjectMouseMove((float)x, (float)y);

        if (_isGameView)
        {
            if (_pointerLocked)
            {
                lock (_cameraLock) _gameCamera.Look(dx, dy);
                WarpToViewportCenter();
            }
            return;
        }

        if (_is2DMode)
        {
            if (_leftDown && _dragCaptured) UpdateGizmoDrag(x, y);
            else if (_rightDown || _middleDown) Pan2DView(dx, dy);
            return;
        }

        if (_rightDown)
        {
            lock (_cameraLock) _sceneCamera.Orbit(dx, dy);
        }
        else if (_middleDown)
        {
            lock (_cameraLock) _sceneCamera.Pan(dx, dy);
        }
        else if (_leftDown && _dragCaptured)
        {
            UpdateGizmoDrag(x, y);
        }
    }

    private void OnNativeMouseButton(int button, bool down, double x, double y)
    {
        if (!_rendererInitialized) return;
        _lastMouseX = x;
        _lastMouseY = y;
        InputAPI.GInput_InjectMouseButton(button, down ? GInputAction.Press : GInputAction.Release, (float)x, (float)y);

        if (_isGameView)
        {
            if (button == 0)
            {
                _pointerLocked = down;
                if (down) WarpToViewportCenter();
            }
            return;
        }

        if (button == 1) _rightDown = down;
        else if (button == 2) _middleDown = down;
        else if (button == 0)
        {
            if (down)
            {
                _leftDown = true;
                TryStartGizmoDrag(x, y);
            }
            else
            {
                if (_leftDown && !_dragCaptured) PickEntity(x, y);
                _leftDown = false;
                EndGizmoDrag();
            }
        }

        // Capture while dragging so orbit/pan/gizmo deltas keep flowing even
        // when the cursor leaves the viewport (no jump on re-entry).
        bool dragging = _rightDown || _middleDown || (_leftDown && _dragCaptured);
        SetViewportCapture(dragging);
    }

    private void OnNativeMouseWheel(int delta)
    {
        if (!_rendererInitialized) return;
        InputAPI.GInput_InjectMouseScroll(0, delta);
        if (_isGameView)
        {
            _gameCamera.FlyMoveSpeed = Clamp(_gameCamera.FlyMoveSpeed * (delta > 0 ? 1.1 : 0.9), 0.5, 100);
        }
        else
        {
            _sceneCamera.Zoom(delta);
        }
    }

    private void OnNativeKey(int vk, bool down)
    {
        if (down) _nativeKeys.Add(vk);
        else _nativeKeys.Remove(vk);
        if (down && vk == 0x1B) // Escape
        {
            _pointerLocked = false;
            SetViewportCapture(false);
        }
    }

    // =====================================================================
    //  Native mouse capture / pointer lock helpers
    // =====================================================================

    private IntPtr ViewportHwnd => _hwndHost?.GlfwChildHandle ?? IntPtr.Zero;

    /// <summary>Captures the GLFW child while dragging so mouse-move messages
    /// keep arriving even when the cursor leaves the viewport.</summary>
    private void SetViewportCapture(bool capture)
    {
        var h = ViewportHwnd;
        if (h == IntPtr.Zero) return;
        if (capture) SetCapture(h);
        else ReleaseCapture();
    }

    /// <summary>Wraps the OS cursor back to the viewport center (FPS pointer
    /// lock). The warp delta is suppressed by resetting the baseline.</summary>
    private void WarpToViewportCenter()
    {
        var h = ViewportHwnd;
        if (h == IntPtr.Zero) return;
        if (!GetClientRect(h, out var rc)) return;
        var pt = new NativePoint { X = rc.Right / 2, Y = rc.Bottom / 2 };
        if (!ClientToScreen(h, ref pt)) return;
        SetCursorPos(pt.X, pt.Y);
        _lastMouseX = rc.Right / 2.0;
        _lastMouseY = rc.Bottom / 2.0;
    }

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

    private static bool KeyHeld(int vk) => (GetAsyncKeyState(vk) & 0x8000) != 0;

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct NativePoint
    {
        public int X;
        public int Y;
    }



    // =====================================================================
    //  Keyboard (Game mode fly + scene shortcuts)
    // =====================================================================

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        _heldKeys.Add(e.Key);
    }

    private void OnPreviewKeyUp(object sender, KeyEventArgs e)
    {
        _heldKeys.Remove(e.Key);
        if (e.Key == Key.Escape) _pointerLocked = false;
    }

    private void OnWindowKeyDown(object sender, KeyEventArgs e)
    {
        _heldKeys.Add(e.Key);

        // Script (code editor) tab active: keep the editor shortcuts working
        // even when the WebView2 child does not currently hold keyboard focus
        // (e.g. right after clicking the tab). When the WebView has focus the
        // keys never reach WPF, so there is no double-handling.
        if (_scriptMode && !e.Handled)
        {
            var mods = Keyboard.Modifiers;
            if ((mods & ModifierKeys.Control) != 0)
            {
                if (e.Key == Key.Z)
                {
                    e.Handled = true;
                    ScriptCommand((mods & ModifierKeys.Shift) != 0 ? "redo" : "undo");
                }
                else if (e.Key == Key.Y)
                {
                    e.Handled = true;
                    ScriptCommand("redo");
                }
                else if (e.Key == Key.S)
                {
                    e.Handled = true;
                    ScriptCommand("saveRequest");
                }
            }
        }
    }

    private void OnWindowKeyUp(object sender, KeyEventArgs e)
    {
        _heldKeys.Remove(e.Key);
    }

    private void OnWindowDeactivated(object? sender, EventArgs e)
    {
        _heldKeys.Clear();
        _pointerLocked = false;
        SetViewportCapture(false);
    }

    private void OnWindowStateChanged(object? sender, EventArgs e)
    {
        var win = Window.GetWindow(this);
        _windowMinimized = win != null && win.WindowState == WindowState.Minimized;
    }

    /// <summary>Repositions the gizmo overlay immediately when the editor
    /// window moves, instead of waiting for the 30Hz overlay tick.</summary>
    private void OnWindowLocationChanged(object? sender, EventArgs e)
    {
        PositionOverlay();
    }

    /// <summary>Updates the top-right gizmo mode badge as soon as the mode
    /// changes (W/E/R), instead of waiting for the 30Hz overlay tick.</summary>
    private void OnVmPropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(EditorViewModel.GizmoMode))
        {
            _overlay?.SetMode(_vmCached?.GizmoMode ?? "Translate");
        }
    }

    // =====================================================================
    //  Picking (screen-space projection of entity positions)
    // =====================================================================

    private void PickEntity(double sx, double sy)
    {
        if (VM == null) return;
        var px = GetPixelSize();

        // 2D 模式：2D 覆盖层为屏幕空间绘制（1 世界单位 ≈ 1 像素），
        // 直接按实体原点屏幕距离拾取最近者。
        if (_is2DMode)
        {
            double worldX = _2dCenterX + (sx - px.W * 0.5) / _2dZoom;
            double worldY = _2dCenterY + (sy - px.H * 0.5) / _2dZoom;
            double best = 22.0 / _2dZoom;
            GEntityHandle bestHandle = GEntityHandle.Null;
            try
            {
                int count = EntityAPI.GEntity_GetCount();
                for (int i = 0; i < count; i++)
                {
                    var h = EntityAPI.GEntity_GetAt(i);
                    if (h == GEntityHandle.Null || h == _mainCamera) continue;
                    if (EntityAPI.GEntity_GetLocalPosition(h, out var p) != 0) continue;
                    double d = Math.Sqrt((p.X - worldX) * (p.X - worldX) +
                                         (p.Y - worldY) * (p.Y - worldY));
                    if (d < best)
                    {
                        best = d;
                        bestHandle = h;
                    }
                }
            }
            catch { /* ignore */ }

            if (bestHandle != GEntityHandle.Null)
                VM.SelectEntityByHandle(bestHandle);
            else
                VM.SelectedEntity = null;
            return;
        }

        if (_mainCamera == GEntityHandle.Null) _mainCamera = FindMainCamera();
        if (_mainCamera == GEntityHandle.Null) return;

        try
        {
            var hit = SceneAPI.GScene_PickScreen(
                (float)sx, (float)sy, px.W, px.H, _mainCamera);
            if (hit != GEntityHandle.Null)
                VM.SelectEntityByHandle(hit);
            else
                VM.SelectedEntity = null; // 空白处取消选中
        }
        catch { /* ignore */ }
    }

    // =====================================================================
    //  Gizmo overlay + drag
    // =====================================================================

    private void BuildGizmoShapes()
    {
        if (_gizmoShapesBuilt) return;
        for (int i = 0; i < 3; i++)
        {
            var brush = new SolidColorBrush(AxisColors[i]) { Opacity = 0.95 };
            var line = new Line
            {
                Stroke = brush,
                StrokeThickness = 2,
                IsHitTestVisible = false
            };
            _axisLines[i] = line;
            _overlay?.Canvas.Children.Add(line);

            var head = new Polygon
            {
                Fill = brush,
                IsHitTestVisible = false
            };
            _axisHeads[i] = head;
            _overlay?.Canvas.Children.Add(head);

            var box = new Rectangle
            {
                Width = 8,
                Height = 8,
                Fill = brush,
                RadiusX = 1.5,
                RadiusY = 1.5,
                IsHitTestVisible = false
            };
            _axisBoxes[i] = box;
            _overlay?.Canvas.Children.Add(box);

            var ring = new Polyline
            {
                Stroke = new SolidColorBrush(AxisColors[i]) { Opacity = 0.85 },
                StrokeThickness = 1.8,
                IsHitTestVisible = false
            };
            _axisRings[i] = ring;
            _overlay?.Canvas.Children.Add(ring);
        }
        _gizmoShapesBuilt = true;
    }

    private void HideGizmoShapes()
    {
        for (int i = 0; i < 3; i++)
        {
            if (_axisLines[i] != null) _axisLines[i].Visibility = Visibility.Collapsed;
            if (_axisHeads[i] != null) _axisHeads[i].Visibility = Visibility.Collapsed;
            if (_axisBoxes[i] != null) _axisBoxes[i].Visibility = Visibility.Collapsed;
            if (_axisRings[i] != null) _axisRings[i].Visibility = Visibility.Collapsed;
        }
    }

    private void UpdateGizmoOverlay()
    {
        try
        {
            UpdateGizmoOverlayCore();
        }
        catch (Exception)
        {
            // Gizmo overlay is best-effort; ignore transient errors.
        }
    }

    private void UpdateGizmoOverlayCore()
    {
        // While a modal dialog is open the overlay must stay hidden (it is a
        // top-level transparent window and could otherwise cover the dialog).
        if (_overlay?.Suppressed == true) return;

        // 设置里关闭 Gizmo 时隐藏整个覆盖（含模式徽标）
        if (!(App.Engine?.EditorSettings.ShowGizmos ?? true))
        {
            HideGizmoShapes();
            return;
        }

        PositionOverlay();
        var selected = VM?.SelectedEntity;
        if (_isGameView || selected == null || VM == null)
        {
            _overlay?.SetMode(LocalizationService.Instance.T("viewport.tool_none"));
            HideGizmoShapes();
            return;
        }

        BuildGizmoShapes();
        if (_is2DMode)
        {
            UpdateGizmoOverlay2D();
            return;
        }
        var px = GetPixelSize();
        var cam = _sceneCamera;
        cam.Aspect = px.W / (double)px.H;

        if (EntityAPI.GEntity_GetLocalPosition(selected.Handle, out var pos) != 0)
        {
            HideGizmoShapes();
            return;
        }

        var center = cam.ProjectToScreen(pos.X, pos.Y, pos.Z, px.W, px.H);
        if (double.IsNaN(center.X))
        {
            HideGizmoShapes();
            return;
        }

        _overlay?.SetMode(VM.GizmoMode);
        _overlay?.Show();
        // Constant screen-space gizmo size (independent of entity scale).
        double handleLen = Clamp(cam.Distance * 0.22, 70, 160);
        double worldLen = ScreenLengthToWorld(handleLen, cam, pos);

        bool local = VM.IsGizmoLocal;
        QuatRot axes;
        if (local && EntityAPI.GEntity_GetLocalRotation(selected.Handle, out var q) == 0)
            axes = QuatToBasis(q);
        else
            axes = QuatRot.Identity;

        var axisDirs = new[]
        {
            (axes.RightX, axes.RightY, axes.RightZ),
            (axes.UpX, axes.UpY, axes.UpZ),
            (axes.FwdX, axes.FwdY, axes.FwdZ)
        };

        string mode = VM.GizmoMode;
        for (int i = 0; i < 3; i++)
        {
            double ex = pos.X + axisDirs[i].Item1 * worldLen;
            double ey = pos.Y + axisDirs[i].Item2 * worldLen;
            double ez = pos.Z + axisDirs[i].Item3 * worldLen;
            var tip = cam.ProjectToScreen(ex, ey, ez, px.W, px.H);

            var line = _axisLines[i];
            line.X1 = center.X; line.Y1 = center.Y;
            line.X2 = tip.X; line.Y2 = tip.Y;
            line.StrokeThickness = _dragAxis == i && _dragCaptured ? 3 : 2;

            if (mode == "Scale")
            {
                _axisHeads[i].Visibility = Visibility.Collapsed;
                _axisBoxes[i].Visibility = Visibility.Visible;
                Canvas.SetLeft(_axisBoxes[i], tip.X - 4);
                Canvas.SetTop(_axisBoxes[i], tip.Y - 4);
                _axisRings[i].Visibility = Visibility.Collapsed;
            }
            else if (mode == "Rotate")
            {
                _axisHeads[i].Visibility = Visibility.Collapsed;
                _axisBoxes[i].Visibility = Visibility.Collapsed;
                _axisRings[i].Visibility = Visibility.Visible;
                UpdateRotateRing(i, axisDirs[i], center, cam, pos, handleLen);
                line.Visibility = Visibility.Collapsed;
            }
            else
            {
                _axisHeads[i].Visibility = Visibility.Visible;
                _axisBoxes[i].Visibility = Visibility.Collapsed;
                _axisRings[i].Visibility = Visibility.Collapsed;
                line.Visibility = Visibility.Visible;

                var head = _axisHeads[i];
                head.Points.Clear();
                double lx = tip.X - center.X, ly = tip.Y - center.Y;
                double ll = Math.Sqrt(lx * lx + ly * ly);
                if (ll > 1e-6)
                {
                    double ux = lx / ll, uy = ly / ll;
                    double px2 = -uy, py2 = ux;
                    head.Points.Add(new Point(tip.X, tip.Y));
                    head.Points.Add(new Point(tip.X - ux * 9 + px2 * 5, tip.Y - uy * 9 + py2 * 5));
                    head.Points.Add(new Point(tip.X - ux * 9 - px2 * 5, tip.Y - uy * 9 - py2 * 5));
                }
            }
        }
    }

    private void UpdateRotateRing(int axis, (double X, double Y, double Z) dir,
                                  (double X, double Y) center, ViewportCamera cam,
                                  GVec3 pos, double handleLen)
    {
        var ring = _axisRings[axis];
        ring.Points.Clear();
        var px = GetPixelSize();
        double worldRadius = ScreenLengthToWorld(handleLen, cam, pos);
        // Find two basis vectors perpendicular to the axis.
        var (bx1, by1, bz1) = Perpendicular(dir);
        var (bx2, by2, bz2) = Cross(dir, (bx1, by1, bz1));
        for (int k = 0; k <= 48; k++)
        {
            double t = k * Math.PI * 2.0 / 48.0;
            double wx = pos.X + (bx1 * Math.Cos(t) + bx2 * Math.Sin(t)) * worldRadius;
            double wy = pos.Y + (by1 * Math.Cos(t) + by2 * Math.Sin(t)) * worldRadius;
            double wz = pos.Z + (bz1 * Math.Cos(t) + bz2 * Math.Sin(t)) * worldRadius;
            var sp = cam.ProjectToScreen(wx, wy, wz, px.W, px.H);
            if (!double.IsNaN(sp.X)) ring.Points.Add(new Point(sp.X, sp.Y));
        }
    }

    // =====================================================================
    //  2D gizmo (translate / rotate / scale in the 2D editor)
    // =====================================================================

    private double Local2DTheta()
    {
        var selected = VM?.SelectedEntity;
        if (!(VM?.IsGizmoLocal ?? false) || selected == null) return 0.0;
        if (EntityAPI.GEntity_GetLocalRotation(selected.Handle, out var rot) == 0)
            return 2.0 * Math.Atan2(rot.Z, rot.W);   // rotation around Z
        return 0.0;
    }

    private static bool PointInBox(double x, double y, double bx, double by, double half)
        => Math.Abs(x - bx) <= half && Math.Abs(y - by) <= half;

    private void Draw2DArrowHead(Polygon head, double tipX, double tipY, double dx, double dy)
    {
        head.Visibility = Visibility.Visible;
        head.Points.Clear();
        double px = -dy, py = dx;
        head.Points.Add(new Point(tipX, tipY));
        head.Points.Add(new Point(tipX - dx * 9 + px * 5, tipY - dy * 9 + py * 5));
        head.Points.Add(new Point(tipX - dx * 9 - px * 5, tipY - dy * 9 - py * 5));
    }

    private void UpdateGizmoOverlay2D()
    {
        var vm = VM;
        if (vm == null)
        {
            HideGizmoShapes();
            return;
        }
        var selected = vm.SelectedEntity;
        if (selected == null)
        {
            HideGizmoShapes();
            return;
        }
        if (EntityAPI.GEntity_GetLocalPosition(selected.Handle, out var pos) != 0)
        {
            HideGizmoShapes();
            return;
        }

        var px = GetPixelSize();
        // Screen position of the entity's 2D world position (world unit = 1 px
        // at _2dZoom, same mapping as picking/panning).
        double cx = px.W * 0.5 + (pos.X - _2dCenterX) * _2dZoom;
        double cy = px.H * 0.5 + (pos.Y - _2dCenterY) * _2dZoom;

        _overlay?.SetMode(vm.GizmoMode);
        _overlay?.Show();

        double theta = Local2DTheta();
        double axx = Math.Cos(theta), axy = Math.Sin(theta);  // local X
        double ayx = -Math.Sin(theta), ayy = Math.Cos(theta); // local Y
        double handleLen = Clamp(Math.Min(px.W, px.H) * 0.16, 50, 140);
        string mode = vm.GizmoMode;

        for (int i = 0; i < 3; i++)
        {
            _axisLines[i].Visibility = Visibility.Collapsed;
            _axisHeads[i].Visibility = Visibility.Collapsed;
            _axisBoxes[i].Visibility = Visibility.Collapsed;
            _axisRings[i].Visibility = Visibility.Collapsed;
        }

        if (mode == "Rotate")
        {
            // Full circle in the X-Y (screen) plane; drag rotates around Z.
            var ring = _axisRings[2];
            ring.Visibility = Visibility.Visible;
            ring.Points.Clear();
            for (int k = 0; k <= 48; k++)
            {
                double t = k * Math.PI * 2.0 / 48.0;
                ring.Points.Add(new Point(cx + Math.Cos(t) * handleLen,
                                          cy + Math.Sin(t) * handleLen));
            }
        }
        else if (mode == "Scale")
        {
            var bx = _axisBoxes[0];
            var by = _axisBoxes[1];
            var bc = _axisBoxes[2];
            bx.Visibility = Visibility.Visible;
            Canvas.SetLeft(bx, cx + axx * handleLen - 4);
            Canvas.SetTop(bx, cy + axy * handleLen - 4);
            by.Visibility = Visibility.Visible;
            Canvas.SetLeft(by, cx + ayx * handleLen - 4);
            Canvas.SetTop(by, cy + ayy * handleLen - 4);
            bc.Visibility = Visibility.Visible;
            Canvas.SetLeft(bc, cx - 4);
            Canvas.SetTop(bc, cy - 4);
        }
        else // Translate
        {
            var lx = _axisLines[0];
            var ly = _axisLines[1];
            var cx0 = _axisBoxes[2];
            lx.Visibility = Visibility.Visible;
            lx.X1 = cx; lx.Y1 = cy;
            lx.X2 = cx + axx * handleLen; lx.Y2 = cy + axy * handleLen;
            lx.StrokeThickness = (_dragAxis == 0 && _dragCaptured) ? 3 : 2;
            Draw2DArrowHead(_axisHeads[0], lx.X2, lx.Y2, axx, axy);

            ly.Visibility = Visibility.Visible;
            ly.X1 = cx; ly.Y1 = cy;
            ly.X2 = cx + ayx * handleLen; ly.Y2 = cy + ayy * handleLen;
            ly.StrokeThickness = (_dragAxis == 1 && _dragCaptured) ? 3 : 2;
            Draw2DArrowHead(_axisHeads[1], ly.X2, ly.Y2, ayx, ayy);

            cx0.Visibility = Visibility.Visible;   // center = free move
            Canvas.SetLeft(cx0, cx - 4);
            Canvas.SetTop(cx0, cy - 4);
        }
    }

    private void TryStartGizmoDrag(double sx, double sy)
    {
        var selected = VM?.SelectedEntity;
        if (selected == null || _isGameView || VM == null) return;

        var px = GetPixelSize();
        var cam = _sceneCamera;
        if (EntityAPI.GEntity_GetLocalPosition(selected.Handle, out var pos) != 0) return;

        var center = cam.ProjectToScreen(pos.X, pos.Y, pos.Z, px.W, px.H);
        if (double.IsNaN(center.X)) return;

        // 2D 模式：仅平移，直接在屏幕平面拖动（1 世界单位 ≈ 1 像素）
        if (_is2DMode)
        {
            Start2DGizmoDrag(sx, sy);
            return;
        }

        double best = 18.0;
        int bestAxis = -1;
        if (VM.GizmoMode == "Rotate")
        {
            for (int i = 0; i < 3; i++)
            {
                var ring = _axisRings[i];
                if (ring.Points.Count == 0) continue;
                for (int k = 0; k < ring.Points.Count; k += 4)
                {
                    var p = ring.Points[k];
                    double d = Math.Sqrt((p.X - sx) * (p.X - sx) + (p.Y - sy) * (p.Y - sy));
                    if (d < best) { best = d; bestAxis = i; }
                }
            }
        }
        else
        {
            for (int i = 0; i < 3; i++)
            {
                var line = _axisLines[i];
                if (line.Visibility != Visibility.Visible) continue;
                double d = PointSegmentDistance(sx, sy, line.X1, line.Y1, line.X2, line.Y2);
                if (d < best) { best = d; bestAxis = i; }
            }
        }

        if (bestAxis < 0) return;

        _dragAxis = bestAxis;
        _dragMode = VM.GizmoMode;
        _dragStartScreenX = sx;
        _dragStartScreenY = sy;
        _gizmoScreenX = center.X;
        _gizmoScreenY = center.Y;
        _dragStartPosX = pos.X; _dragStartPosY = pos.Y; _dragStartPosZ = pos.Z;
        EntityAPI.GEntity_GetLocalScale(selected.Handle, out var scl);
        _dragStartScaleX = scl.X; _dragStartScaleY = scl.Y; _dragStartScaleZ = scl.Z;
        EntityAPI.GEntity_GetLocalRotation(selected.Handle, out _dragStartRot);
        _dragCaptured = true;

        bool local = VM.IsGizmoLocal;
        var axes = local ? QuatToBasis(_dragStartRot) : QuatRot.Identity;
        var axisDirs = new[]
        {
            (axes.RightX, axes.RightY, axes.RightZ),
            (axes.UpX, axes.UpY, axes.UpZ),
            (axes.FwdX, axes.FwdY, axes.FwdZ)
        };
        _dragStartScreenDist = Math.Sqrt(
            (sx - center.X) * (sx - center.X) + (sy - center.Y) * (sy - center.Y));
    }

    private void Start2DGizmoDrag(double sx, double sy)
    {
        var selected = VM?.SelectedEntity;
        if (selected == null || VM == null) return;
        if (EntityAPI.GEntity_GetLocalPosition(selected.Handle, out var pos) != 0) return;

        var px = GetPixelSize();
        double cx = px.W * 0.5 + (pos.X - _2dCenterX) * _2dZoom;
        double cy = px.H * 0.5 + (pos.Y - _2dCenterY) * _2dZoom;
        double handleLen = Clamp(Math.Min(px.W, px.H) * 0.16, 50, 140);
        double theta = Local2DTheta();
        double axx = Math.Cos(theta), axy = Math.Sin(theta);
        double ayx = -Math.Sin(theta), ayy = Math.Cos(theta);

        string mode = VM.GizmoMode;
        int hitAxis = -1;
        const double best = 18.0;

        if (mode == "Rotate")
        {
            var ring = _axisRings[2];
            if (ring.Points.Count > 0)
            {
                for (int k = 0; k < ring.Points.Count; k += 2)
                {
                    var p = ring.Points[k];
                    double d = Math.Sqrt((p.X - sx) * (p.X - sx) + (p.Y - sy) * (p.Y - sy));
                    if (d < best) hitAxis = 2;
                }
            }
        }
        else if (mode == "Scale")
        {
            if (PointInBox(sx, sy, cx, cy, 12)) hitAxis = 2;                        // center = uniform
            else if (PointInBox(sx, sy, cx + axx * handleLen, cy + axy * handleLen, 12)) hitAxis = 0;
            else if (PointInBox(sx, sy, cx + ayx * handleLen, cy + ayy * handleLen, 12)) hitAxis = 1;
        }
        else // Translate
        {
            if (PointInBox(sx, sy, cx, cy, 12)) hitAxis = 2;                        // center = free move
            else
            {
                double d0 = PointSegmentDistance(sx, sy, cx, cy,
                                                 cx + axx * handleLen, cy + axy * handleLen);
                double d1 = PointSegmentDistance(sx, sy, cx, cy,
                                                 cx + ayx * handleLen, cy + ayy * handleLen);
                if (d0 < d1 && d0 < best) hitAxis = 0;
                else if (d1 < best) hitAxis = 1;
            }
        }
        if (hitAxis < 0) return;

        _dragAxis = hitAxis;
        _dragMode = mode == "Rotate" ? "Rotate2D"
                  : mode == "Scale" ? "Scale2D"
                  : hitAxis == 2 ? "Translate2D" : "Translate2DAxis";
        _dragStartScreenX = sx;
        _dragStartScreenY = sy;
        _gizmoScreenX = cx;
        _gizmoScreenY = cy;
        _dragStartPosX = pos.X; _dragStartPosY = pos.Y; _dragStartPosZ = pos.Z;
        EntityAPI.GEntity_GetLocalScale(selected.Handle, out var scl);
        _dragStartScaleX = scl.X; _dragStartScaleY = scl.Y; _dragStartScaleZ = scl.Z;
        EntityAPI.GEntity_GetLocalRotation(selected.Handle, out _dragStartRot);
        _dragStartScreenDist = Math.Sqrt((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy));
        _dragCaptured = true;
    }

    private void UpdateGizmoDrag(double sx, double sy)
    {
        var selected = VM?.SelectedEntity;
        if (selected == null || _dragAxis < 0 || VM == null) return;
        var px = GetPixelSize();
        var cam = _sceneCamera;

        if (_dragMode == "Translate2D" || _dragMode == "Translate2DAxis")
        {
            double nx, ny;
            if (_dragMode == "Translate2D")
            {
                double dx = (sx - _dragStartScreenX) / _2dZoom;
                double dy = (sy - _dragStartScreenY) / _2dZoom;
                dx = VM?.SnapDelta(dx) ?? dx;
                dy = VM?.SnapDelta(dy) ?? dy;
                nx = _dragStartPosX + dx;
                ny = _dragStartPosY + dy;
            }
            else
            {
                double theta = (VM?.IsGizmoLocal ?? false)
                    ? 2.0 * Math.Atan2(_dragStartRot.Z, _dragStartRot.W)
                    : 0.0;
                double a2x = _dragAxis == 0 ? Math.Cos(theta) : -Math.Sin(theta);
                double a2y = _dragAxis == 0 ? Math.Sin(theta) : Math.Cos(theta);
                double along = (sx - _dragStartScreenX) * a2x + (sy - _dragStartScreenY) * a2y;
                double delta2d = VM?.SnapDelta(along / _2dZoom) ?? (along / _2dZoom);
                nx = _dragStartPosX + a2x * delta2d;
                ny = _dragStartPosY + a2y * delta2d;
            }
            lock (_gizmoApplyLock)
            {
                _gizmoApplyEntity = selected.Handle;
                _gizmoApplyPos = new GVec3((float)nx, (float)ny, (float)_dragStartPosZ);
                _gizmoApplyMask |= 1;
                _gizmoApplyDirty = true;
            }
            return;
        }

        if (_dragMode == "Rotate2D")
        {
            // Rotate around Z by the mouse's angular displacement on screen.
            double currentAngle = Math.Atan2(sy - _gizmoScreenY, sx - _gizmoScreenX);
            double startAngle = Math.Atan2(_dragStartScreenY - _gizmoScreenY, _dragStartScreenX - _gizmoScreenX);
            double angle = currentAngle - startAngle;
            var result = MulQuat(QuatAxisAngle(0.0, 0.0, 1.0, angle), _dragStartRot);
            lock (_gizmoApplyLock)
            {
                _gizmoApplyEntity = selected.Handle;
                _gizmoApplyRot = result;
                _gizmoApplyMask |= 2;
                _gizmoApplyDirty = true;
            }
            return;
        }

        if (_dragMode == "Scale2D")
        {
            double theta = (VM?.IsGizmoLocal ?? false)
                ? 2.0 * Math.Atan2(_dragStartRot.Z, _dragStartRot.W)
                : 0.0;
            double axx = Math.Cos(theta), axy = Math.Sin(theta);
            double ayx = -Math.Sin(theta), ayy = Math.Cos(theta);
            var sc = new GVec3((float)_dragStartScaleX, (float)_dragStartScaleY, (float)_dragStartScaleZ);
            if (_dragAxis == 2)
            {
                // Center handle: uniform scale from the radial screen distance.
                double curDist = Math.Sqrt((sx - _gizmoScreenX) * (sx - _gizmoScreenX) +
                                           (sy - _gizmoScreenY) * (sy - _gizmoScreenY));
                double factor = _dragStartScreenDist > 1e-6 ? curDist / _dragStartScreenDist : 1.0;
                factor = Math.Max(factor, 0.05);
                sc.X = (float)(_dragStartScaleX * factor);
                sc.Y = (float)(_dragStartScaleY * factor);
            }
            else
            {
                double a2x = _dragAxis == 0 ? axx : ayx;
                double a2y = _dragAxis == 0 ? axy : ayy;
                double along = (sx - _dragStartScreenX) * a2x + (sy - _dragStartScreenY) * a2y;
                double handleLen = Clamp(Math.Min(px.W, px.H) * 0.16, 50, 140);
                double handleWorld = handleLen / _2dZoom;
                double factor = handleWorld > 1e-6 ? 1.0 + (along / _2dZoom) / handleWorld : 1.0;
                factor = Math.Max(factor, 0.05);
                if (_dragAxis == 0) sc.X = (float)(_dragStartScaleX * factor);
                else sc.Y = (float)(_dragStartScaleY * factor);
            }
            lock (_gizmoApplyLock)
            {
                _gizmoApplyEntity = selected.Handle;
                _gizmoApplyScale = sc;
                _gizmoApplyMask |= 4;
                _gizmoApplyDirty = true;
            }
            return;
        }

        bool local = VM.IsGizmoLocal;
        var axes = local ? QuatToBasis(_dragStartRot) : QuatRot.Identity;
        var axisDirs = new[]
        {
            (axes.RightX, axes.RightY, axes.RightZ),
            (axes.UpX, axes.UpY, axes.UpZ),
            (axes.FwdX, axes.FwdY, axes.FwdZ)
        };
        double adx = axisDirs[_dragAxis].Item1;
        double ady = axisDirs[_dragAxis].Item2;
        double adz = axisDirs[_dragAxis].Item3;

        // Screen-space projection of the drag axis direction.
        double sxDir = adx * cam.RightX + ady * cam.RightY + adz * cam.RightZ;
        double syDir = -(adx * cam.UpX + ady * cam.UpY + adz * cam.UpZ);
        double slen = Math.Sqrt(sxDir * sxDir + syDir * syDir);
        if (slen < 1e-6) return;
        sxDir /= slen; syDir /= slen;

        double sdx = sx - _dragStartScreenX;
        double sdy = sy - _dragStartScreenY;
        double screenDelta = sdx * sxDir + sdy * syDir;
        double worldPerScreen = ScreenLengthToWorld(
            1.0, cam, new GVec3((float)_dragStartPosX, (float)_dragStartPosY, (float)_dragStartPosZ));
        double worldDelta = screenDelta * worldPerScreen;

        if (_dragMode == "Translate")
        {
            double snapped = VM?.SnapDelta(worldDelta) ?? worldDelta;
            double nx = _dragStartPosX + adx * snapped;
            double ny = _dragStartPosY + ady * snapped;
            double nz = _dragStartPosZ + adz * snapped;
            lock (_gizmoApplyLock)
            {
                _gizmoApplyEntity = selected.Handle;
                _gizmoApplyPos = new GVec3((float)nx, (float)ny, (float)nz);
                _gizmoApplyMask |= 1;
                _gizmoApplyDirty = true;
            }
        }
        else if (_dragMode == "Scale")
        {
            double factor = _dragStartScreenDist > 1e-6
                ? (_dragStartScreenDist + screenDelta) / _dragStartScreenDist
                : 1.0;
            var sc = new GVec3((float)_dragStartScaleX, (float)_dragStartScaleY, (float)_dragStartScaleZ);
            if (_dragAxis == 0) sc.X = (float)(_dragStartScaleX * factor);
            else if (_dragAxis == 1) sc.Y = (float)(_dragStartScaleY * factor);
            else sc.Z = (float)(_dragStartScaleZ * factor);
            lock (_gizmoApplyLock)
            {
                _gizmoApplyEntity = selected.Handle;
                _gizmoApplyScale = sc;
                _gizmoApplyMask |= 4;
                _gizmoApplyDirty = true;
            }
        }
        else if (_dragMode == "Rotate")
        {
            // Rotate by the mouse's angular displacement around the gizmo center
            // on screen. Stable and intuitive (no 180° snapping).
            double currentAngle = Math.Atan2(sy - _gizmoScreenY, sx - _gizmoScreenX);
            double startAngle = Math.Atan2(_dragStartScreenY - _gizmoScreenY, _dragStartScreenX - _gizmoScreenX);
            double angle = currentAngle - startAngle;
            var axisQuat = QuatAxisAngle(adx, ady, adz, angle);
            var result = MulQuat(axisQuat, _dragStartRot);
            lock (_gizmoApplyLock)
            {
                _gizmoApplyEntity = selected.Handle;
                _gizmoApplyRot = result;
                _gizmoApplyMask |= 2;
                _gizmoApplyDirty = true;
            }
        }
    }

    /// <summary>Applies the latest batched gizmo transform (called once per
    /// rendered frame from the render thread).</summary>
    private void ApplyPendingGizmoTransform()
    {
        GEntityHandle h;
        GVec3 p = default;
        GQuat q = default;
        GVec3 s = default;
        int mask;
        lock (_gizmoApplyLock)
        {
            if (!_gizmoApplyDirty) return;
            h = _gizmoApplyEntity;
            p = _gizmoApplyPos;
            q = _gizmoApplyRot;
            s = _gizmoApplyScale;
            mask = _gizmoApplyMask;
            _gizmoApplyDirty = false;
        }
        if (h == GEntityHandle.Null) return;
        if ((mask & 1) != 0) EntityAPI.GEntity_SetLocalPosition(h, ref p);
        if ((mask & 2) != 0) EntityAPI.GEntity_SetLocalRotation(h, ref q);
        if ((mask & 4) != 0) EntityAPI.GEntity_SetLocalScale(h, ref s);
    }

    private void EndGizmoDrag()
    {
        ApplyPendingGizmoTransform();
        GEntityHandle h;
        lock (_gizmoApplyLock) { h = _gizmoApplyEntity; }
        if (h != GEntityHandle.Null) VM?.RaiseTransformChanged(h);
        if (h != GEntityHandle.Null)
        {
            // 记录一次拖拽的 Undo：起点值在拖拽开始时已捕获，终点值从引擎读回。
            VM?.PushTransformAction(h,
                new GVec3((float)_dragStartPosX, (float)_dragStartPosY, (float)_dragStartPosZ),
                _dragStartRot,
                new GVec3((float)_dragStartScaleX, (float)_dragStartScaleY, (float)_dragStartScaleZ));
        }
        _dragCaptured = false;
        _dragAxis = -1;
    }

    // =====================================================================
    //  Helpers
    // =====================================================================

    private double ScreenLengthToWorld(double screenLen, ViewportCamera cam, GVec3 pos)
    {
        var px = GetPixelSize();
        var center = cam.ProjectToScreen(pos.X, pos.Y, pos.Z, px.W, px.H);
        if (double.IsNaN(center.X)) return 1.0;
        var edge = cam.ProjectToScreen(pos.X + cam.RightX, pos.Y + cam.RightY, pos.Z + cam.RightZ, px.W, px.H);
        if (double.IsNaN(edge.X)) return 1.0;
        double perUnit = Math.Sqrt((edge.X - center.X) * (edge.X - center.X) +
                                   (edge.Y - center.Y) * (edge.Y - center.Y));
        return perUnit > 1e-6 ? screenLen / perUnit : 1.0;
    }

    private static double PointSegmentDistance(double px, double py, double x1, double y1, double x2, double y2)
    {
        double dx = x2 - x1, dy = y2 - y1;
        double len2 = dx * dx + dy * dy;
        if (len2 < 1e-9) return Math.Sqrt((px - x1) * (px - x1) + (py - y1) * (py - y1));
        double t = Clamp(((px - x1) * dx + (py - y1) * dy) / len2, 0, 1);
        double cx = x1 + t * dx, cy = y1 + t * dy;
        return Math.Sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
    }

    private static double Clamp(double v, double lo, double hi)
        => v < lo ? lo : (v > hi ? hi : v);






    private void PositionOverlay()
    {
        var overlay = _overlay;
        if (overlay == null || ViewportBorder == null || !ViewportBorder.IsVisible) return;
        // While the window is being resized, the overlay would jitter against
        // the moving frame; hold it in place until the resize settles.
        if ((DateTime.Now - _lastResizeTime).TotalMilliseconds < 150) return;
        try
        {
            var src = PresentationSource.FromVisual(ViewportBorder);
            if (src == null) return;
            var topLeft = ViewportBorder.PointToScreen(new Point(0, 0));
            var dip = src.CompositionTarget.TransformFromDevice.Transform(topLeft);
            var px = GetPixelSize();
            double scale = px.W / Math.Max(ViewportBorder.ActualWidth, 1.0);
            if (Math.Abs(_overlayScale - scale) > 0.001 ||
                Math.Abs(overlay.Canvas.Width - px.W) > 0.5)
            {
                overlay.Canvas.LayoutTransform = new ScaleTransform(1.0 / scale, 1.0 / scale);
                overlay.Canvas.Width = px.W;
                overlay.Canvas.Height = px.H;
                _overlayScale = scale;
            }
            if (Math.Abs(overlay.Left - dip.X) > 0.5 || Math.Abs(overlay.Top - dip.Y) > 0.5 ||
                Math.Abs(overlay.Width - ViewportBorder.ActualWidth) > 0.5 ||
                Math.Abs(overlay.Height - ViewportBorder.ActualHeight) > 0.5)
            {
                overlay.Left = dip.X;
                overlay.Top = dip.Y;
                overlay.Width = ViewportBorder.ActualWidth;
                overlay.Height = ViewportBorder.ActualHeight;
            }
        }
        catch { /* ignore transient layout errors */ }
    }

    private static (double X, double Y, double Z) Perpendicular((double X, double Y, double Z) v)
    {
        if (Math.Abs(v.X) < 0.9) return (1, 0, 0);
        return (0, 1, 0);
    }

    private static (double X, double Y, double Z) Cross(
        (double X, double Y, double Z) a, (double X, double Y, double Z) b)
    {
        return (a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
    }

    private static GQuat MulQuat(GQuat a, GQuat b)
    {
        return new GQuat(
            a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
            a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
            a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
            a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z);
    }

    private static GQuat QuatAxisAngle(double x, double y, double z, double angle)
    {
        double len = Math.Sqrt(x * x + y * y + z * z);
        if (len < 1e-9) return new GQuat(0, 0, 0, 1);
        x /= len; y /= len; z /= len;
        double s = Math.Sin(angle / 2.0);
        return new GQuat((float)(x * s), (float)(y * s), (float)(z * s), (float)Math.Cos(angle / 2.0));
    }

    private struct QuatRot
    {
        public QuatRot(double r00, double r01, double r02, double r10, double r11, double r12, double r20, double r21, double r22)
        {
            RightX = r00; RightY = r10; RightZ = r20;
            UpX = r01; UpY = r11; UpZ = r21;
            FwdX = r02; FwdY = r12; FwdZ = r22;
        }
        public static QuatRot Identity => new(1, 0, 0, 0, 1, 0, 0, 0, 1);
        public double RightX, RightY, RightZ, UpX, UpY, UpZ, FwdX, FwdY, FwdZ;
    }

    private static QuatRot QuatToBasis(GQuat q)
    {
        double x = q.X, y = q.Y, z = q.Z, w = q.W;
        double xx = x * x, yy = y * y, zz = z * z;
        double xy = x * y, xz = x * z, yz = y * z;
        double wx = w * x, wy = w * y, wz = w * z;
        return new QuatRot(
            1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy),
            2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx),
            2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy));
    }

    // === Tab Switching ===

    private void OnSceneTabClick(object sender, RoutedEventArgs e)
    {
        _isGameView = false;
        try { SceneAPI.GScene_SetMode(1); } catch { /* ignore */ }
        Set2DMode(false);
        TabScene.IsChecked = true;
        Tab2D.IsChecked = false;
        TabGame.IsChecked = false;
        TabScript.IsChecked = false;
        ShowRenderSurface();
        GizmoInfo.Text = LocalizationService.Instance.T("viewport.scene_view");
        GizmoOverlay.Visibility = Visibility.Visible;
    }

    private void On2DTabClick(object sender, RoutedEventArgs e)
    {
        _isGameView = false;
        try { SceneAPI.GScene_SetMode(0); } catch { /* ignore */ }
        Set2DMode(true);
        TabScene.IsChecked = false;
        Tab2D.IsChecked = true;
        TabGame.IsChecked = false;
        TabScript.IsChecked = false;
        ShowRenderSurface();
        GizmoInfo.Text = LocalizationService.Instance.T("viewport.scene_2d");
        GizmoOverlay.Visibility = Visibility.Visible;
    }

    private void OnGameTabClick(object sender, RoutedEventArgs e)
    {
        _isGameView = true;
        Set2DMode(false);
        TabScene.IsChecked = false;
        Tab2D.IsChecked = false;
        TabGame.IsChecked = true;
        TabScript.IsChecked = false;
        ShowRenderSurface();
        GizmoInfo.Text = LocalizationService.Instance.T("viewport.game_view");
        GizmoOverlay.Visibility = Visibility.Collapsed;
    }

    private void OnScriptTabClick(object sender, RoutedEventArgs e)
    {
        _isGameView = false;
        Set2DMode(false);
        TabScene.IsChecked = false;
        Tab2D.IsChecked = false;
        TabGame.IsChecked = false;
        TabScript.IsChecked = true;
        GizmoInfo.Text = LocalizationService.Instance.T("viewport.tab_script");
        GizmoOverlay.Visibility = Visibility.Collapsed;
        EnterScriptMode(_currentScriptPath);
    }

    // === Script-tab toolbar (undo / redo / save -> Monaco) ===

    private void OnScriptUndoClick(object sender, RoutedEventArgs e) => ScriptCommand("undo");
    private void OnScriptRedoClick(object sender, RoutedEventArgs e) => ScriptCommand("redo");
    private void OnScriptSaveClick(object sender, RoutedEventArgs e) => ScriptCommand("saveRequest");

    /// <summary>Forwards an editor command to the Monaco instance in the
    /// WebView2 script tab ("undo", "redo" or "saveRequest").</summary>
    private void ScriptCommand(string cmd)
    {
        try
        {
            if (ScriptWebView.CoreWebView2 == null) return;
            ScriptWebView.Focus();
            if (cmd == "saveRequest")
            {
                ScriptWebView.CoreWebView2.ExecuteScriptAsync(
                    "window.chrome.webview.postMessage(" +
                    "{ cmd: 'save', text: window.gryceEditor.getContent() });");
            }
            else
            {
                ScriptWebView.CoreWebView2.ExecuteScriptAsync(
                    "window.gryceEditor && window.gryceEditor." + cmd + "();");
            }
        }
        catch { /* webview not ready */ }
    }

    private void EnterScriptMode(string? path)
    {
        _scriptMode = true;
        // Hidden (not Collapsed): Collapsed destroys the HwndHost window and
        // the embedded GLFW child together with its GL context, leaving the
        // sleeping render thread holding a dead context. Hidden keeps the
        // window alive (just not shown), so switching back resumes rendering
        // without a destroy/recreate race.
        HostContainer.Visibility = Visibility.Hidden;
        GizmoOverlay.Visibility = Visibility.Collapsed;
        ViewportOverlay.Visibility = Visibility.Collapsed;
        ScriptWebView.Visibility = Visibility.Visible;
        // 代码编辑器里不显示场景编辑器工具：顶栏右侧的显示模式/分辨率、
        // 底部信息栏，以及悬浮的 Gizmo 覆盖窗口。
        UpdateSceneToolsVisible(false);
        UpdateScriptToolsVisible(true);
        _overlay?.Hide();
        ScriptModeChanged?.Invoke(false);
        LoadScriptFile(path);
        // Keep keyboard focus inside Monaco so Ctrl+Z / Ctrl+Y / Ctrl+S edit
        // the script text immediately after switching to the Script tab.
        Dispatcher.BeginInvoke(new Action(() =>
        {
            try
            {
                ScriptWebView.Focus();
                ScriptWebView.CoreWebView2?.ExecuteScriptAsync(
                    "window.gryceEditor && window.gryceEditor.focus();");
            }
            catch { /* webview not ready yet */ }
        }), DispatcherPriority.Input);
    }

    private void ShowRenderSurface()
    {
        _scriptMode = false;
        ScriptWebView.Visibility = Visibility.Collapsed;
        HostContainer.Visibility = Visibility.Visible;
        UpdateSceneToolsVisible(true);
        UpdateScriptToolsVisible(false);
        if (_overlay != null && !_overlay.Suppressed)
        {
            _overlay.Show();
        }
        ScriptModeChanged?.Invoke(true);

        // The embedded GLFW child was hidden (not destroyed: the Script tab
        // uses Visibility.Hidden so the window and GL context survive).
        // Vulkan swapchains can still go stale while hidden, so force a full
        // surface recreate there; for a healthy OpenGL child a render-target
        // refresh (next-frame SetSize) is enough.
        if (_renderApi == GRenderAPI.Vulkan || !IsGlfwChildAlive())
        {
            _appliedPixelSize = default;
            _needRenderRecovery = true;
        }
        else
        {
            _appliedPixelSize = default;
        }
        StartRenderThread();

        Dispatcher.BeginInvoke(new Action(() =>
        {
            var px = GetPixelSize();
            UpdateResolutionDisplay(px.W, px.H);
            PositionOverlay();
        }), DispatcherPriority.Loaded);
    }

    /// <summary>True when the embedded GLFW child HWND still exists.</summary>
    private bool IsGlfwChildAlive()
    {
        nint child = _hwndHost?.GlfwChildHandle ?? IntPtr.Zero;
        return child != IntPtr.Zero && IsWindow(child);
    }

    /// <summary>Shows/hides viewport chrome that is only meaningful while the
    /// scene editor is active (display-mode button, resolution label, FPS bar).</summary>
    private void UpdateSceneToolsVisible(bool visible)
    {
        if (ViewportToolsRight != null)
        {
            ViewportToolsRight.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        }
        if (ViewportInfoBar != null)
        {
            ViewportInfoBar.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    /// <summary>Shows/hides the Script-tab toolbar (undo/redo/save).</summary>
    private void UpdateScriptToolsVisible(bool visible)
    {
        if (ScriptToolbar != null)
        {
            ScriptToolbar.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    private void OnOpenScriptRequested(string path)
    {
        Dispatcher.BeginInvoke(new Action(() =>
        {
            TabScene.IsChecked = false;
            Tab2D.IsChecked = false;
            TabGame.IsChecked = false;
            TabScript.IsChecked = true;
            EnterScriptMode(path);
        }), DispatcherPriority.Normal);
    }

    // === Script tab: Monaco (WebView2) ===

    private async void LoadScriptFile(string? path)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
        {
            path = FindNewScriptPath();
        }
        _currentScriptPath = path;
        try
        {
            _scriptInitialContent = File.Exists(path) ? File.ReadAllText(path) : NewScriptTemplate;
        }
        catch (Exception ex)
        {
            VM?.AppendConsole($"[Script] read failed: {ex.Message}");
            _scriptInitialContent = string.Empty;
        }

        try
        {
            await EnsureScriptWebViewAsync();
            if (!_scriptWebReady)
            {
                _scriptLoadingContent = true;
                var html = System.IO.Path.Combine(AppDomain.CurrentDomain.BaseDirectory,
                                                  "assets", "monaco", "editor.html");
                ScriptWebView.CoreWebView2.Navigate(new Uri(html).ToString());
            }
            else
            {
                SendContentToWeb(_scriptInitialContent);
            }
        }
        catch (Exception ex)
        {
            VM?.AppendConsole($"[Script] WebView error: {ex.Message}");
        }
    }

    private async System.Threading.Tasks.Task EnsureScriptWebViewAsync()
    {
        if (ScriptWebView.CoreWebView2 != null) return;
        await ScriptWebView.EnsureCoreWebView2Async(null);
        if (ScriptWebView.CoreWebView2 == null) return;
        ScriptWebView.CoreWebView2.WebMessageReceived += OnScriptWebMessage;
        ScriptWebView.CoreWebView2.NavigationCompleted += OnScriptNavigationCompleted;
        ScriptWebView.CoreWebView2.Settings.AreDefaultContextMenusEnabled = true;
        ScriptWebView.CoreWebView2.Settings.AreDevToolsEnabled = false;
    }

    private void OnScriptNavigationCompleted(object? sender, CoreWebView2NavigationCompletedEventArgs e)
    {
        if (!e.IsSuccess || _scriptLoadingContent)
        {
            _scriptLoadingContent = false;
            SendContentToWeb(_scriptInitialContent);
        }
    }

    private void SendContentToWeb(string? content)
    {
        if (ScriptWebView.CoreWebView2 == null) return;
        var payload = "{\"cmd\":\"setContent\",\"content\":\"" +
                      JsonEscape(content ?? string.Empty) + "\"}";
        try
        {
            ScriptWebView.CoreWebView2.PostWebMessageAsJson(payload);
            // Keep keyboard focus inside Monaco so Ctrl+Z / Ctrl+Y edit the
            // script text instead of falling through to the scene undo/redo.
            ScriptWebView.Focus();
            ScriptWebView.CoreWebView2.ExecuteScriptAsync(
                "window.gryceEditor && window.gryceEditor.focus();");
        }
        catch { /* page not ready yet */ }
    }

    private static string JsonEscape(string s)
    {
        var sb = new StringBuilder(s.Length + 16);
        foreach (char c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                    else sb.Append(c);
                    break;
            }
        }
        return sb.ToString();
    }

    private void OnScriptWebMessage(object? sender, CoreWebView2WebMessageReceivedEventArgs e)
    {
        string json;
        try { json = e.TryGetWebMessageAsString(); }
        catch { return; }

        string cmd = ExtractJsonString(json, "cmd") ?? string.Empty;
        if (cmd == "ready")
        {
            _scriptWebReady = true;
            SendContentToWeb(_scriptInitialContent);
        }
        else if (cmd == "save")
        {
            string text = ExtractJsonString(json, "text") ?? string.Empty;
            SaveScript(text);
        }
        else if (cmd == "changed")
        {
            if (VM != null && !string.IsNullOrEmpty(_currentScriptPath))
            {
                VM.AppendConsole($"[Script] modified: {System.IO.Path.GetFileName(_currentScriptPath)}");
            }
        }
    }

    private static string? ExtractJsonString(string json, string key)
    {
        var marker = "\"" + key + "\":";
        int idx = json.IndexOf(marker, StringComparison.Ordinal);
        if (idx < 0) return null;
        idx += marker.Length;
        while (idx < json.Length && char.IsWhiteSpace(json[idx])) idx++;
        if (idx >= json.Length || json[idx] != '"') return null;
        var sb = new StringBuilder();
        bool escape = false;
        for (int i = idx + 1; i < json.Length; i++)
        {
            char c = json[i];
            if (escape)
            {
                sb.Append(c);
                escape = false;
            }
            else if (c == '\\') escape = true;
            else if (c == '"') break;
            else sb.Append(c);
        }
        return sb.ToString();
    }

    private void SaveScript(string text)
    {
        if (string.IsNullOrEmpty(_currentScriptPath)) return;
        try
        {
            File.WriteAllText(_currentScriptPath, text);
            VM?.AppendConsole($"Saved: {_currentScriptPath}");
            VM?.ReloadScripts();
        }
        catch (Exception ex)
        {
            VM?.AppendConsole($"[Script] save failed: {ex.Message}");
        }
    }

    private static string FindNewScriptPath()
    {
        string root = EngineService.Current?.ProjectRoot ?? AppDomain.CurrentDomain.BaseDirectory;
        string scripts = System.IO.Path.Combine(root, "scripts");
        try { Directory.CreateDirectory(scripts); } catch { }
        for (int i = 1; ; i++)
        {
            string candidate = System.IO.Path.Combine(scripts, $"NewScript{i}.lua");
            if (!File.Exists(candidate)) return candidate;
        }
    }

    private const string NewScriptTemplate =
        "-- GryceSRT script template (GryceEngineUtils preloaded).\r\n" +
        "require(\"GryceEngineUtils\")\r\n" +
        "\r\n" +
        "props = {\r\n" +
        "    speed = 1.0\r\n" +
        "}\r\n" +
        "\r\n" +
        "function on_start()\r\n" +
        "    engine.log.info(\"on_start\")\r\n" +
        "end\r\n" +
        "\r\n" +
        "function on_update(dt)\r\n" +
        "    -- engine.self() / engine.entity.* / engine.component.* / GryceEngineUtils.*\r\n" +
        "end\r\n";

    private void OnRelease3DSceneClick(object sender, RoutedEventArgs e)
    {
        int rc = SceneAPI.GScene_ReleaseMode(1);
        VM?.AppendConsole(rc == 0
            ? "[Viewport] Released 3D scene memory."
            : "[Viewport] Failed to release 3D scene memory.");
    }

    private void OnRelease2DSceneClick(object sender, RoutedEventArgs e)
    {
        int rc = SceneAPI.GScene_ReleaseMode(0);
        VM?.AppendConsole(rc == 0
            ? "[Viewport] Released 2D scene memory."
            : "[Viewport] Failed to release 2D scene memory.");
    }

    // =====================================================================
    //  2D 场景编辑器：只渲染 2D 画布 + 编辑器 2D 相机 + 网格
    // =====================================================================

    private void Set2DMode(bool on)
    {
        if (_is2DMode == on) return;
        _is2DMode = on;
        try { RenderAPI.GRender_SetScene2D(on); } catch { /* ignore */ }
        _overlay?.SetMode(on ? "2D" : (_vmCached?.GizmoMode ?? "Translate"));
        if (on) Enter2DView(); else Exit2DView();
        VM?.AppendConsole(on
            ? "[Viewport] 2D Scene Editor: 2D canvas only (pan: middle/right drag, zoom: wheel)"
            : "[Viewport] 3D Scene Editor");
    }

    private void Enter2DView()
    {
        // 优先使用场景已有活动 Camera2D；否则创建编辑器 2D 相机实体
        _editor2DCamera = FindActiveCamera2D();
        _editor2DCameraCreated = false;
        _2dCameraReady = false;
        if (_editor2DCamera != GEntityHandle.Null)
        {
            Read2DCameraView();
        }
        else
        {
            _2dCenterX = 0; _2dCenterY = 0; _2dZoom = 1.0f;
            _pending2DCameraName = "Editor2DCamera";
            CreateEditor2DCameraEntity();
        }
    }

    private void Exit2DView()
    {
        if (_editor2DCameraCreated && _editor2DCamera != GEntityHandle.Null)
        {
            Span<byte> payload = stackalloc byte[sizeof(int)];
            BitConverterCompat.TryWriteBytes(payload, (int)_editor2DCamera);
            var cmd = GCommand.Create(GCommandType.DestroyEntity, payload);
            CoreAPI.GCore_PushCommand(ref cmd);
            _editor2DCameraCreated = false;
        }
        _editor2DCamera = GEntityHandle.Null;
        _pending2DCameraName = null;
        _2dCameraReady = false;
        Update2DGrid();
    }

    private void CreateEditor2DCameraEntity()
    {
        Span<byte> payload = stackalloc byte[128 + sizeof(int)];
        var nameBytes = Encoding.UTF8.GetBytes("Editor2DCamera");
        nameBytes.AsSpan().CopyTo(payload);
        BitConverterCompat.TryWriteBytes(payload.Slice(128), (int)0);
        var cmd = GCommand.Create(GCommandType.CreateEntity, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
        _editor2DCameraCreated = true;
    }

    private GEntityHandle FindActiveCamera2D()
    {
        int count = EntityAPI.GEntity_GetCount();
        for (int i = 0; i < count; i++)
        {
            var h = EntityAPI.GEntity_GetAt(i);
            if (h == GEntityHandle.Null) continue;
            int comps = ComponentAPI.GComponent_GetCount(h);
            for (int c = 0; c < comps; c++)
            {
                var sb = new StringBuilder(128);
                if (ComponentAPI.GComponent_GetTypeNameAt(h, c, sb, sb.Capacity) > 0 &&
                    sb.ToString().EndsWith("Camera2D", StringComparison.Ordinal))
                {
                    return h;
                }
            }
        }
        return GEntityHandle.Null;
    }

    private GEntityHandle FindEntityByName(string name)
    {
        int count = EntityAPI.GEntity_GetCount();
        for (int i = 0; i < count; i++)
        {
            var h = EntityAPI.GEntity_GetAt(i);
            if (h == GEntityHandle.Null) continue;
            var n = EntityAPI.GetNameUtf8(h);
            if (n == name) return h;
        }
        return GEntityHandle.Null;
    }

    private ulong FindRegisteredTypeHash(string typeName)
    {
        int count = ComponentAPI.GComponent_GetRegisteredTypeCount();
        for (int i = 0; i < count; i++)
        {
            var sb = new StringBuilder(128);
            if (ComponentAPI.GComponent_GetRegisteredTypeInfo(i, out ulong hash, sb, sb.Capacity) >= 0 &&
                sb.ToString() == typeName)
            {
                return hash;
            }
        }
        return 0;
    }

    /// <summary>每 30Hz tick 完成编辑器 2D 相机的创建/组件挂载/参数写入。</summary>
    private void Update2DCameraSetup()
    {
        if (!_is2DMode) return;

        if (_pending2DCameraName != null)
        {
            var h = FindEntityByName(_pending2DCameraName);
            if (h != GEntityHandle.Null)
            {
                _editor2DCamera = h;
                _pending2DCameraName = null;
                ulong hash = FindRegisteredTypeHash("Camera2D");
                if (hash != 0) ComponentAPI.GComponent_AddComponent(h, hash);
            }
            return;
        }

        if (_editor2DCamera != GEntityHandle.Null && !_2dCameraReady)
        {
            if (ComponentAPI.GComponent_GetTypeHashAt(_editor2DCamera, 0, out ulong hash) == 0 && hash != 0)
            {
                Apply2DCameraView(hash);
                _2dCameraReady = true;
            }
        }
    }

    private void Read2DCameraView()
    {
        if (EntityAPI.GEntity_GetLocalPosition(_editor2DCamera, out var pos) == 0)
        {
            _2dCenterX = pos.X; _2dCenterY = pos.Y;
        }
        if (ComponentAPI.GComponent_GetTypeHashAt(_editor2DCamera, 0, out ulong hash) == 0 && hash != 0)
        {
            _2dZoom = GetComponentFloat(_editor2DCamera, hash, "zoom");
            if (_2dZoom <= 0.01f) _2dZoom = 1.0f;
            _2dCameraReady = true;
        }
    }

    private void Apply2DCameraView(ulong hash)
    {
        var pos = new GVec3((float)_2dCenterX, (float)_2dCenterY, 0.0f);
        EntityAPI.GEntity_SetLocalPosition(_editor2DCamera, ref pos);
        SetComponentFloat(_editor2DCamera, hash, "zoom", _2dZoom);
        // is_active 默认 true、offset (0,0)、canvas_layer 0（世界层），无需写入
    }

    private void Apply2DCameraView()
    {
        if (!_2dCameraReady || _editor2DCamera == GEntityHandle.Null) return;
        if (ComponentAPI.GComponent_GetTypeHashAt(_editor2DCamera, 0, out ulong hash) != 0 || hash == 0) return;
        Apply2DCameraView(hash);
    }

    private void Pan2DView(double dx, double dy)
    {
        _2dCenterX -= dx / _2dZoom;
        _2dCenterY -= dy / _2dZoom;
        Apply2DCameraView();
    }

    private void Zoom2DView(double wheelDelta)
    {
        _2dZoom = (float)Clamp(_2dZoom * (wheelDelta > 0 ? 1.1 : 0.9), 0.1, 20.0);
        Apply2DCameraView();
    }

    private static void SetComponentFloat(GEntityHandle entity, ulong hash, string name, float value)
    {
        var buf = BitConverter.GetBytes(value);
        var pin = System.Runtime.InteropServices.GCHandle.Alloc(buf, System.Runtime.InteropServices.GCHandleType.Pinned);
        try { ComponentAPI.GComponent_SetProperty(entity, hash, name, pin.AddrOfPinnedObject(), buf.Length); }
        finally { pin.Free(); }
    }

    private static float GetComponentFloat(GEntityHandle entity, ulong hash, string name)
    {
        var buf = new byte[4];
        var pin = System.Runtime.InteropServices.GCHandle.Alloc(buf, System.Runtime.InteropServices.GCHandleType.Pinned);
        try
        {
            if (ComponentAPI.GComponent_GetProperty(entity, hash, name, pin.AddrOfPinnedObject(), 4) == 0)
                return BitConverter.ToSingle(buf, 0);
        }
        finally { pin.Free(); }
        return 1.0f;
    }

    // ---- 2D 网格（overlay 屏幕空间）----

    private void Update2DGrid()
    {
        if (_is2DMode && (App.Engine?.EditorSettings.ShowGrid ?? true))
        {
            if (!_gridBuilt) Build2DGrid();
        }
        else if (_gridBuilt)
        {
            foreach (var line in _gridLines)
            {
                _overlay?.Canvas.Children.Remove(line);
            }
            _gridLines.Clear();
            _gridBuilt = false;
        }
    }

    private void Build2DGrid()
    {
        var px = GetPixelSize();
        const int spacing = 64;
        var brush = new SolidColorBrush(Color.FromArgb(40, 160, 160, 160));
        var axisBrush = new SolidColorBrush(Color.FromArgb(90, 255, 255, 255));
        for (int x = 0; x <= px.W; x += spacing)
        {
            var line = new Line
            {
                X1 = x, Y1 = 0, X2 = x, Y2 = px.H,
                Stroke = x == 0 ? axisBrush : brush,
                StrokeThickness = 1,
                IsHitTestVisible = false
            };
            _overlay?.Canvas.Children.Add(line);
            _gridLines.Add(line);
        }
        for (int y = 0; y <= px.H; y += spacing)
        {
            var line = new Line
            {
                X1 = 0, Y1 = y, X2 = px.W, Y2 = y,
                Stroke = y == 0 ? axisBrush : brush,
                StrokeThickness = 1,
                IsHitTestVisible = false
            };
            _overlay?.Canvas.Children.Add(line);
            _gridLines.Add(line);
        }
        _gridBuilt = true;
    }

    private void OnDisplayModeClick(object sender, RoutedEventArgs e)
    {
        _displayMode = _displayMode switch
        {
            "Shaded" => "Wireframe",
            "Wireframe" => "Shaded Wireframe",
            "Shaded Wireframe" => "Shaded",
            _ => "Shaded"
        };
        BtnDisplayMode.Content = _displayMode switch
        {
            "Wireframe" => LocalizationService.Instance.T("viewport.wireframe"),
            "Shaded Wireframe" => LocalizationService.Instance.T("viewport.shaded_wireframe"),
            _ => LocalizationService.Instance.T("viewport.shaded")
        };

        try
        {
            RenderAPI.GRender_SetDisplayMode(_displayMode);
        }
        catch { /* ignore if not supported */ }

        VM?.AppendConsole($"[Viewport] Display mode: {_displayMode}");
    }

    private void OnAlignClick(object sender, RoutedEventArgs e)
    {
        if (sender is Button b && b.ContextMenu is System.Windows.Controls.ContextMenu cm)
        {
            cm.PlacementTarget = b;
            cm.Placement = System.Windows.Controls.Primitives.PlacementMode.Bottom;
            cm.IsOpen = true;
        }
    }

    private void OnAlignAllClick(object sender, RoutedEventArgs e) => VM?.AlignToGrid();
    private void OnAlignXClick(object sender, RoutedEventArgs e) => VM?.AlignToGrid("X");
    private void OnAlignYClick(object sender, RoutedEventArgs e) => VM?.AlignToGrid("Y");
    private void OnAlignZClick(object sender, RoutedEventArgs e) => VM?.AlignToGrid("Z");

    public void Dispose()
    {
        StopRenderThread();
        _gizmoTimer?.Stop();
        _gizmoTimer = null;
        if (_rendererInitialized)
        {
            try { RenderAPI.GRender_Shutdown(); } catch { }
            _rendererInitialized = false;
        }
        _hwndHost?.Dispose();
        _hwndHost = null;
    }
}
