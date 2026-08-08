using GryceEngine.Editor.Native;
using GryceEngine.Editor.Services;
using GryceEngine.Editor.ViewModels;
using System;
using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
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
    private (int W, int H) _lastPixelSize = (0, 0);

    public ViewportView()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        SizeChanged += OnSizeChanged;
    }

    private EditorViewModel? VM => DataContext as EditorViewModel;

    /// <summary>
    /// 把 WPF 布局尺寸（DIP）换算为物理像素。
    /// 嵌入的 GLFW 窗口按物理像素创建，否则在高 DPI 下只覆盖视口的一部分。
    /// </summary>
    private (int W, int H) GetPixelSize(double? width = null, double? height = null)
    {
        // 渲染表面是 ViewportBorder（中间的视口区域），不是整个 UserControl
        // （后者还包含标签栏和底栏）。以 ViewportBorder 的实际尺寸为准。
        double w = width ?? (ViewportBorder != null && ViewportBorder.ActualWidth > 0
            ? ViewportBorder.ActualWidth : ActualWidth);
        double h = height ?? (ViewportBorder != null && ViewportBorder.ActualHeight > 0
            ? ViewportBorder.ActualHeight : ActualHeight);
        var dpi = VisualTreeHelper.GetDpi(this);
        return (Math.Max((int)Math.Round(w * dpi.DpiScaleX), 100),
                Math.Max((int)Math.Round(h * dpi.DpiScaleY), 100));
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        int w = Math.Max((int)ActualWidth, 100);
        int h = Math.Max((int)ActualHeight, 100);
        var px = GetPixelSize();

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

        UpdateResolutionDisplay(px.W, px.H);
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
        var px = GetPixelSize(e.NewSize.Width, e.NewSize.Height);
        UpdateResolutionDisplay(px.W, px.H);

        if (px != _lastPixelSize && WindowAPI.GWindow_IsValid())
        {
            try
            {
                WindowAPI.GWindow_SetSize(px.W, px.H);
                ViewportAPI.GViewport_SetSize(px.W, px.H);
                _lastPixelSize = px;
            }
            catch { /* ignore during init */ }
        }
    }

    private void UpdateResolutionDisplay(int w, int h)
    {
        ViewportResolution.Text = $"{w}x{h}";
    }

    private void OnHwndCreated(object? sender, nint hwnd)
    {
        var px = GetPixelSize();
        int w = px.W;
        int h = px.H;
        // 不在初始化阶段锁定尺寸：OnLoaded/BuildWindowCore 时的 Actual* 可能不是
        // 最终布局值，交由后续 SizeChanged / 布局完成后的校正来设定。
        _lastPixelSize = (0, 0);

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
            // 布局完成后再取一次最终尺寸，确保嵌入窗口与视口像素一致。
            Dispatcher.BeginInvoke(new Action(() =>
            {
                var finalPx = GetPixelSize();
                if (WindowAPI.GWindow_IsValid())
                {
                    WindowAPI.GWindow_SetSize(finalPx.W, finalPx.H);
                    ViewportAPI.GViewport_SetSize(finalPx.W, finalPx.H);
                }
                _lastPixelSize = finalPx;
                UpdateResolutionDisplay(finalPx.W, finalPx.H);
            }), DispatcherPriority.Loaded);
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
