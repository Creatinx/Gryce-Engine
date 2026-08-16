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

    private static readonly Color[] AxisColors =
    {
        Color.FromRgb(0xFF, 0x45, 0x45), // X red
        Color.FromRgb(0x45, 0xFF, 0x45), // Y green
        Color.FromRgb(0x45, 0x8C, 0xFF)  // Z blue
    };



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
        Services.EditorInteractionState.BeginBusy();

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
        Services.EditorInteractionState.BeginBusy();
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
        if (_dragCaptured) Services.EditorInteractionState.EndBusy();
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

    /// <summary>取消拖拽（失焦/Esc）：清理状态但不推 undo，也不提交半成品变换。</summary>
    private void CancelGizmoDrag()
    {
        if (_dragCaptured) Services.EditorInteractionState.EndBusy();
        _dragCaptured = false;
        _dragAxis = -1;
        _dragMode = "";
        _leftDown = false;
        _rightDown = false;
        _middleDown = false;
    }

    // =====================================================================
    //  Helpers
    // =====================================================================



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


}
