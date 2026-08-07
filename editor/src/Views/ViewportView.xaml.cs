using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using System;
using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Threading;

namespace GryceEngine.Editor.Views;

public partial class ViewportView : UserControl, IDisposable
{
    private ViewportHwndHost? _hwndHost;
    private DispatcherTimer? _renderTimer;
    private bool _rendererInitialized;
    private bool _isGameView;
    private string _displayMode = "Shaded";
    private readonly Stopwatch _fpsStopwatch = Stopwatch.StartNew();
    private int _frameCount;
    private int _lastFps;

    public ViewportView()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        SizeChanged += OnSizeChanged;
    }

    private EditorViewModel? VM => DataContext as EditorViewModel;

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        int w = Math.Max((int)ActualWidth, 100);
        int h = Math.Max((int)ActualHeight, 100);

        _hwndHost = new ViewportHwndHost(w, h);
        _hwndHost.HwndCreated += OnHwndCreated;
        _hwndHost.MouseMove += OnViewportMouseMove;
        _hwndHost.MouseDown += OnViewportMouseDown;
        _hwndHost.MouseUp += OnViewportMouseUp;
        _hwndHost.MouseWheel += OnViewportMouseWheel;
        HostContainer.Content = _hwndHost;

        _renderTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromSeconds(1.0 / 60.0)
        };
        _renderTimer.Tick += OnRenderTick;
        _renderTimer.Start();

        UpdateResolutionDisplay(w, h);
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _renderTimer?.Stop();
        _renderTimer = null;

        if (_rendererInitialized)
        {
            try { RenderAPI.GRender_Shutdown(); } catch { }
            _rendererInitialized = false;
        }

        if (_hwndHost != null)
        {
            _hwndHost.HwndCreated -= OnHwndCreated;
            _hwndHost.MouseMove -= OnViewportMouseMove;
            _hwndHost.MouseDown -= OnViewportMouseDown;
            _hwndHost.MouseUp -= OnViewportMouseUp;
            _hwndHost.MouseWheel -= OnViewportMouseWheel;
            _hwndHost.Dispose();
            _hwndHost = null;
        }
    }

    private void OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        int w = Math.Max((int)e.NewSize.Width, 100);
        int h = Math.Max((int)e.NewSize.Height, 100);
        UpdateResolutionDisplay(w, h);

        if (_rendererInitialized)
        {
            try { ViewportAPI.GViewport_SetSize(w, h); }
            catch { /* ignore during init */ }
        }
    }

    private void UpdateResolutionDisplay(int w, int h)
    {
        ViewportResolution.Text = $"{w}x{h}";
    }

    private void OnHwndCreated(object? sender, nint hwnd)
    {
        int w = Math.Max((int)ActualWidth, 100);
        int h = Math.Max((int)ActualHeight, 100);

        try
        {
            int result = WindowAPI.GWindow_InitExternal(new GWindowHandle(hwnd), w, h);
            if (result != 0)
            {
                VM?.AppendConsole("[Viewport] Failed to init external window");
                InitStatusText.Text = "Failed to initialize renderer window.";
                return;
            }

            var renderDesc = new GRenderInitDesc
            {
                Version = (uint)System.Runtime.InteropServices.Marshal.SizeOf<GRenderInitDesc>(),
                NativeWindow = new GWindowHandle(hwnd),
                Api = GRenderAPI.OpenGL,
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
            NoRendererMessage.Visibility = Visibility.Collapsed;
            InitStatusText.Text = "Renderer initialized.";
            VM?.AppendConsole("[Viewport] Renderer initialized successfully");
        }
        catch (Exception ex)
        {
            VM?.AppendConsole($"[Viewport] Init error: {ex.Message}");
            InitStatusText.Text = $"Error: {ex.Message}";
        }
    }

    private void OnRenderTick(object? sender, EventArgs e)
    {
        if (!_rendererInitialized) return;

        try
        {
            RenderAPI.GRender_BeginFrame();
            if (_isGameView)
            {
                RenderAPI.GRender_RenderGameView();
            }
            else
            {
                RenderAPI.GRender_RenderWorld();
                RenderAPI.GRender_RenderGizmo();
            }
            RenderAPI.GRender_EndFrame();

            // FPS calculation
            _frameCount++;
            if (_fpsStopwatch.ElapsedMilliseconds >= 1000)
            {
                _lastFps = _frameCount;
                _frameCount = 0;
                _fpsStopwatch.Restart();
                FpsCounter.Text = $"{_lastFps} FPS";
            }
        }
        catch (Exception ex)
        {
            VM?.AppendConsole($"[Viewport] Render error: {ex.Message}");
        }
    }

    // === Mouse Input Forwarding ===

    private void OnViewportMouseMove(object? sender, MouseEventArgs e)
    {
        if (!_rendererInitialized) return;
        var pos = e.GetPosition(HostContainer);
        InputAPI.GInput_InjectMouseMove((float)pos.X, (float)pos.Y);
    }

    private void OnViewportMouseDown(object? sender, MouseButtonEventArgs e)
    {
        if (!_rendererInitialized) return;
        var pos = e.GetPosition(HostContainer);
        InputAPI.GInput_InjectMouseButton((int)e.ChangedButton, GInputAction.Press, (float)pos.X, (float)pos.Y);
    }

    private void OnViewportMouseUp(object? sender, MouseButtonEventArgs e)
    {
        if (!_rendererInitialized) return;
        var pos = e.GetPosition(HostContainer);
        InputAPI.GInput_InjectMouseButton((int)e.ChangedButton, GInputAction.Release, (float)pos.X, (float)pos.Y);
    }

    private void OnViewportMouseWheel(object? sender, MouseWheelEventArgs e)
    {
        if (!_rendererInitialized) return;
        InputAPI.GInput_InjectMouseScroll(0, e.Delta);
    }

    // === Tab Switching ===

    private void OnSceneTabClick(object sender, RoutedEventArgs e)
    {
        _isGameView = false;
        TabScene.IsChecked = true;
        TabGame.IsChecked = false;
        GizmoInfo.Text = LocalizationService.Instance.T("viewport.scene_view");
        GizmoOverlay.Visibility = Visibility.Visible;
    }

    private void OnGameTabClick(object sender, RoutedEventArgs e)
    {
        _isGameView = true;
        TabScene.IsChecked = false;
        TabGame.IsChecked = true;
        GizmoInfo.Text = LocalizationService.Instance.T("viewport.game_view");
        GizmoOverlay.Visibility = Visibility.Collapsed;
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

        // Send display mode to renderer
        try
        {
            RenderAPI.GRender_SetDisplayMode(_displayMode);
        }
        catch { /* ignore if not supported */ }

        VM?.AppendConsole($"[Viewport] Display mode: {_displayMode}");
    }

    public void Dispose()
    {
        _renderTimer?.Stop();
        _renderTimer = null;
        if (_rendererInitialized)
        {
            try { RenderAPI.GRender_Shutdown(); } catch { }
            _rendererInitialized = false;
        }
        _hwndHost?.Dispose();
        _hwndHost = null;
    }
}
