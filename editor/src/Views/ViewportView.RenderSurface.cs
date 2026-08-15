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

public partial class ViewportView
{

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


    /// <summary>True when the embedded GLFW child HWND still exists.</summary>
    private bool IsGlfwChildAlive()
    {
        nint child = _hwndHost?.GlfwChildHandle ?? IntPtr.Zero;
        return child != IntPtr.Zero && IsWindow(child);
    }

    /// <summary>Shows/hides viewport chrome that is only meaningful while the
    /// scene editor is active (display-mode button, resolution label, FPS bar).</summary>

}
