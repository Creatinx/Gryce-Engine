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

    private void PushSharedCamera()
    {
        if (_is2DMode) return; // 2D 画布不受 3D 相机影响
        if (_isGameView) return; // 游戏视图由场景主相机(Lua)驱动，编辑器不覆盖
        if (++_cameraResolveCounter >= 120 || _mainCamera == GEntityHandle.Null)
        {
            _mainCamera = FindMainCamera();
            _cameraResolveCounter = 0;
        }
        if (_mainCamera == GEntityHandle.Null) return;
        var cam = _sceneCamera;
        double px, py, pz, fwdX, fwdY, fwdZ;
        lock (_cameraLock)
        {
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

    /// <summary>
    /// Applies tracked mouse input inside the render tick so camera movement
    /// stays in sync with rendering (no teleporting when input and render race).
    /// </summary>


    /// <summary>
    /// Applies tracked mouse input inside the render tick so camera movement
    /// stays in sync with rendering (no teleporting when input and render race).
    /// </summary>
    private void ProcessViewportInput()
    {
        if (_isGameView)
        {
            // 游戏相机由 Lua 接收 engine.input.mouse_delta 驱动，编辑器不直接移动相机。
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



    // =====================================================================
    //  Editor camera → scene MainCamera
    // =====================================================================

    private void UpdateViewportCamera()
    {
        if (_is2DMode || _isGameView) return; // 游戏视图相机由场景主相机(Lua)驱动
        if (++_cameraResolveCounter >= 60 || _mainCamera == GEntityHandle.Null)
        {
            _mainCamera = FindMainCamera();
            _cameraResolveCounter = 0;
        }
        if (_mainCamera == GEntityHandle.Null) return;

        var cam = _sceneCamera;
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
                // 游戏相机由 Lua 接收 engine.input.mouse_delta 驱动，编辑器不直接移动相机。
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
            SetGameCursorLocked(true);
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
            SetGameCursorLocked(false);
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
            // 滚轮事件已注入核心；游戏内由 Lua 处理。
            return;
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
                SetGameCursorLocked(down);
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
            // 滚轮事件已注入核心；游戏内由 Lua 处理。
        }
        else if (_is2DMode)
        {
            Zoom2DView(delta);
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
        if (_isGameView)
            InputAPI.GInput_InjectKey(vk, down ? GInputAction.Press : GInputAction.Release);
        if (down && vk == 0x1B) // Escape
        {
            _pointerLocked = false;
            SetGameCursorLocked(false);
            SetViewportCapture(false);
        }
    }

    // =====================================================================
    //  Native mouse capture / pointer lock helpers
    // =====================================================================



    // =====================================================================
    //  Native mouse capture / pointer lock helpers
    // =====================================================================

    private IntPtr ViewportHwnd => _hwndHost?.GlfwChildHandle ?? IntPtr.Zero;

    /// <summary>Captures the GLFW child while dragging so mouse-move messages
    /// keep arriving even when the cursor leaves the viewport.</summary>


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



    /// <summary>Hides/shows the OS cursor for FPS-style pointer lock in the
    /// game view. ShowCursor is reference-counted, so only toggle on edge.</summary>
    private void SetGameCursorLocked(bool locked)
    {
        if (locked == _cursorHidden) return;
        _cursorHidden = locked;
        // ShowCursor(false) 隐藏光标，ShowCursor(true) 显示；参考计数只在边沿切换。
        ShowCursor(!locked);
        if (locked) WarpToViewportCenter();
    }

    private static bool KeyHeld(int vk) => (GetAsyncKeyState(vk) & 0x8000) != 0;

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
        if (e.Key == Key.Escape)
        {
            _pointerLocked = false;
            SetGameCursorLocked(false);
        }
    }



    private void OnWindowKeyDown(object sender, KeyEventArgs e)
    {
        _heldKeys.Add(e.Key);
        if (_isGameView)
            InputAPI.GInput_InjectKey(KeyInterop.VirtualKeyFromKey(e.Key), GInputAction.Press);

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
        if (_isGameView)
            InputAPI.GInput_InjectKey(KeyInterop.VirtualKeyFromKey(e.Key), GInputAction.Release);
    }



    private void OnWindowDeactivated(object? sender, EventArgs e)
    {
        _heldKeys.Clear();
        _pointerLocked = false;
        SetGameCursorLocked(false);
        SetViewportCapture(false);
    }



    private void OnWindowStateChanged(object? sender, EventArgs e)
    {
        var win = Window.GetWindow(this);
        _windowMinimized = win != null && win.WindowState == WindowState.Minimized;
    }

    /// <summary>Repositions the gizmo overlay immediately when the editor
    /// window moves, instead of waiting for the 30Hz overlay tick.</summary>


    /// <summary>Repositions the gizmo overlay immediately when the editor
    /// window moves, instead of waiting for the 30Hz overlay tick.</summary>
    private void OnWindowLocationChanged(object? sender, EventArgs e)
    {
        PositionOverlay();
    }

    /// <summary>响应 Lua 的 engine.input.mouse_locked()：在 Game 视图内
    /// 锁定/隐藏光标并捕获鼠标（FPS 视角用），离开游戏视图自动释放。</summary>
    private void OnMouseLockRequested(bool locked)
    {
        if (!_isGameView) return;
        _pointerLocked = locked;
        SetGameCursorLocked(locked);
        SetViewportCapture(locked);
    }

    /// <summary>Updates the top-right gizmo mode badge as soon as the mode
    /// changes (W/E/R), instead of waiting for the 30Hz overlay tick.</summary>


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


}
