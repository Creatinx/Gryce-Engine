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

    // === Tab Switching ===

    private void OnSceneTabClick(object sender, RoutedEventArgs e)
    {
        _isGameView = false;
        GameViewActive = false;
        SetGameCursorLocked(false);
        // 离开游戏视图即停止 Play，恢复场景编辑态。
        if (VM != null && VM.IsPlaying) VM.Stop();
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
        GameViewActive = false;
        SetGameCursorLocked(false);
        if (VM != null && VM.IsPlaying) VM.Stop();
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
        GameViewActive = true;
        // 游戏视图即游戏画布：进入即开始 Play，让 Lua 脚本接管场景与相机。
        if (VM != null && !VM.IsPlaying) VM.Play();
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
        GameViewActive = false;
        SetGameCursorLocked(false);
        if (VM != null && VM.IsPlaying) VM.Stop();
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


    /// <summary>Shows/hides the Script-tab toolbar (undo/redo/save).</summary>
    private void UpdateScriptToolsVisible(bool visible)
    {
        if (ScriptToolbar != null)
        {
            ScriptToolbar.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        }
    }



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
