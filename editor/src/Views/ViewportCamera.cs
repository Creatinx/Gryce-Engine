using System;

namespace GryceEngine.Editor.Views;

/// <summary>
/// Editor viewport camera: orbit/pan/zoom in Scene mode and a fly camera for
/// Game mode. Mirrors core/math/camera.h conventions (yaw -90 looks toward -Z,
/// pitch clamped to ±89) so overlay gizmos and picking stay aligned with the
/// native renderer.
/// </summary>
public sealed class ViewportCamera
{
    public const float MinDistance = 0.5f;
    public const float MaxDistance = 2000f;

    public double Yaw { get; private set; } = -90.0;
    public double Pitch { get; private set; }
    public double PositionX { get; set; } = 0.0;
    public double PositionY { get; set; } = 2.5;
    public double PositionZ { get; set; } = 8.0;
    public double TargetX { get; private set; }
    public double TargetY { get; private set; }
    public double TargetZ { get; private set; }
    public double Distance { get; private set; } = 10.0;

    public double Fov { get; set; } = 60.0;
    public double Near { get; set; } = 0.1;
    public double Far { get; set; } = 1000.0;
    public double Aspect { get; set; } = 16.0 / 9.0;

    // Fly-camera state (Game mode).
    public double FlyMoveSpeed { get; set; } = 5.0;

    // ---- vectors ----------------------------------------------------------
    public double ForwardX { get; private set; }
    public double ForwardY { get; private set; }
    public double ForwardZ { get; private set; }
    public double RightX { get; private set; } = 1.0;
    public double RightY { get; private set; }
    public double RightZ { get; private set; }
    public double UpX { get; private set; }
    public double UpY { get; private set; } = 1.0;
    public double UpZ { get; private set; }

    public ViewportCamera()
    {
        UpdateVectors();
    }

    public void Reset(double px, double py, double pz, double yaw, double pitch)
    {
        PositionX = px; PositionY = py; PositionZ = pz;
        Yaw = yaw; Pitch = pitch;
        TargetX = px; TargetY = py; TargetZ = pz;
        Distance = 10.0;
        UpdateVectors();
    }

    /// <summary>Positions the camera at (px,py,pz) looking at the given target.</summary>
    public void LookAtTarget(double px, double py, double pz, double tx, double ty, double tz)
    {
        PositionX = px; PositionY = py; PositionZ = pz;
        TargetX = tx; TargetY = ty; TargetZ = tz;
        double dx = tx - px, dy = ty - py, dz = tz - pz;
        Distance = Math.Sqrt(dx * dx + dy * dy + dz * dz);
        if (Distance < MinDistance) Distance = MinDistance;
        Yaw = ToDegrees(Math.Atan2(dz, dx));
        double horiz = Math.Sqrt(dx * dx + dz * dz);
        Pitch = ToDegrees(Math.Atan2(dy, Math.Max(horiz, 1e-9)));
        Pitch = Clamp(Pitch, -89.0, 89.0);
        ApplyOrbit();
    }

    public void SetOrbitTarget(double tx, double ty, double tz)
    {
        TargetX = tx; TargetY = ty; TargetZ = tz;
    }

    /// <summary>Repositions the camera from its current orbit target.</summary>
    public void ApplyOrbit()
    {
        var yawR = ToRadians(Yaw);
        var pitchR = ToRadians(Pitch);
        var offX = Math.Cos(yawR) * Math.Cos(pitchR) * Distance;
        var offY = Math.Sin(pitchR) * Distance;
        var offZ = Math.Sin(yawR) * Math.Cos(pitchR) * Distance;
        PositionX = TargetX - offX;
        PositionY = TargetY - offY;
        PositionZ = TargetZ - offZ;
        UpdateVectors();
    }

    /// <summary>Right-button drag orbit.</summary>
    public void Orbit(double dx, double dy)
    {
        // Drag right/down rotates the view so the scene follows the cursor
        // (standard editor convention). Earlier both signs were flipped.
        Yaw += dx * 0.2;
        Pitch -= dy * 0.2;
        Pitch = Clamp(Pitch, -89.0, 89.0);
        ApplyOrbit();
    }

    /// <summary>Middle-button drag pan in the camera plane.</summary>
    public void Pan(double dx, double dy)
    {
        var scale = Distance * 0.0012;
        // Dragging right moves the camera right (target moves left); dragging
        // down moves the camera down (target moves up).
        TargetX -= RightX * dx * scale + UpX * dy * scale;
        TargetY -= RightY * dx * scale + UpY * dy * scale;
        TargetZ -= RightZ * dx * scale + UpZ * dy * scale;
        ApplyOrbit();
    }

    /// <summary>Mouse wheel zoom toward the target.</summary>
    public void Zoom(int wheelDelta)
    {
        Distance *= Math.Pow(0.88, wheelDelta / 120.0);
        Distance = Clamp(Distance, MinDistance, MaxDistance);
        ApplyOrbit();
    }

    /// <summary>FPS-style look delta (Game mode).</summary>
    public void Look(double dx, double dy)
    {
        Yaw += dx * 0.12;
        Pitch += dy * 0.12;
        Pitch = Clamp(Pitch, -89.0, 89.0);
        UpdateVectors();
    }

    /// <summary>FPS-style movement (Game mode). dx/dy/dz in world axes.</summary>
    public void FlyMove(double forward, double strafe, double up, double dt)
    {
        PositionX += (ForwardX * forward + RightX * strafe) * FlyMoveSpeed * dt;
        PositionY += (ForwardY * forward + RightY * strafe + up) * FlyMoveSpeed * dt;
        PositionZ += (ForwardZ * forward + RightZ * strafe) * FlyMoveSpeed * dt;
        UpdateVectors();
    }

    // ---- matrices ---------------------------------------------------------
    public double[] GetViewMatrix()
    {
        double fx = PositionX + ForwardX, fy = PositionY + ForwardY, fz = PositionZ + ForwardZ;
        return LookAt(PositionX, PositionY, PositionZ, fx, fy, fz, UpX, UpY, UpZ);
    }

    public double[] GetProjectionMatrix()
    {
        return Perspective(ToRadians(Fov), Aspect, Near, Far);
    }

    /// <summary>Projects a world point to normalized device coordinates.</summary>
    public (double X, double Y) ProjectToScreen(double wx, double wy, double wz, double viewportW, double viewportH)
    {
        var view = GetViewMatrix();
        var proj = GetProjectionMatrix();
        var v = MulVec(view, wx, wy, wz, 1.0);
        var c = MulVec(proj, v.X, v.Y, v.Z, v.W);
        if (Math.Abs(c.W) < 1e-9) return (double.NaN, double.NaN);
        double ndcX = c.X / c.W;
        double ndcY = c.Y / c.W;
        return ((ndcX * 0.5 + 0.5) * viewportW, (1.0 - (ndcY * 0.5 + 0.5)) * viewportH);
    }

    /// <summary>Builds a world ray (origin + direction) from a screen point.</summary>
    public (double Ox, double Oy, double Oz, double Dx, double Dy, double Dz) ScreenToRay(
        double sx, double sy, double viewportW, double viewportH)
    {
        double ndcX = (sx / viewportW) * 2.0 - 1.0;
        double ndcY = 1.0 - (sy / viewportH) * 2.0;
        var inv = Inverse(GetViewMatrix(), GetProjectionMatrix());
        var near = MulVec(inv, ndcX, ndcY, -1.0, 1.0);
        var far = MulVec(inv, ndcX, ndcY, 1.0, 1.0);
        double ox = near.X / near.W, oy = near.Y / near.W, oz = near.Z / near.W;
        double fx = far.X / far.W, fy = far.Y / far.W, fz = far.Z / far.W;
        double dx = fx - ox, dy = fy - oy, dz = fz - oz;
        double len = Math.Sqrt(dx * dx + dy * dy + dz * dz);
        if (len < 1e-9) return (ox, oy, oz, 0, 0, 0);
        return (ox, oy, oz, dx / len, dy / len, dz / len);
    }

    /// <summary>Closest point on a 3D line segment (axis) to a world ray.</summary>
    public static (double X, double Y, double Z) ClosestPointOnLine(
        double ax, double ay, double az, double adx, double ady, double adz,
        double rox, double roy, double roz, double rdx, double rdy, double rdz)
    {
        // Line (A + s*Ad), Ray (O + t*Rd). Solve least-squares for s.
        double b0 = adx * adx + ady * ady + adz * adz;
        double b1 = adx * rdx + ady * rdy + adz * rdz;
        double b2 = rdx * rdx + rdy * rdy + rdz * rdz;
        double d = b0 * b2 - b1 * b1;
        double s = 0.0;
        if (Math.Abs(d) > 1e-12)
        {
            double w0x = ax - rox, w0y = ay - roy, w0z = az - roz;
            double c0 = adx * w0x + ady * w0y + adz * w0z;
            double c1 = rdx * w0x + rdy * w0y + rdz * w0z;
            s = (b0 * c1 - b1 * c0) / d;
        }
        return (ax + adx * s, ay + ady * s, az + adz * s);
    }

    // ---- internals --------------------------------------------------------
    private void UpdateVectors()
    {
        double yawR = ToRadians(Yaw);
        double pitchR = ToRadians(Pitch);
        ForwardX = Math.Cos(yawR) * Math.Cos(pitchR);
        ForwardY = Math.Sin(pitchR);
        ForwardZ = Math.Sin(yawR) * Math.Cos(pitchR);
        double len = Math.Sqrt(ForwardX * ForwardX + ForwardY * ForwardY + ForwardZ * ForwardZ);
        if (len > 1e-9) { ForwardX /= len; ForwardY /= len; ForwardZ /= len; }

        // right = forward x worldUp
        RightX = ForwardY * 0 - ForwardZ * 1;
        RightY = ForwardZ * 0 - ForwardX * 0;
        RightZ = ForwardX * 1 - ForwardY * 0;
        len = Math.Sqrt(RightX * RightX + RightY * RightY + RightZ * RightZ);
        if (len > 1e-9) { RightX /= len; RightY /= len; RightZ /= len; }

        // up = right x forward
        UpX = RightY * ForwardZ - RightZ * ForwardY;
        UpY = RightZ * ForwardX - RightX * ForwardZ;
        UpZ = RightX * ForwardY - RightY * ForwardX;
    }

    private static (double X, double Y, double Z, double W) MulVec(double[] m, double x, double y, double z, double w)
    {
        return (
            m[0] * x + m[4] * y + m[8] * z + m[12] * w,
            m[1] * x + m[5] * y + m[9] * z + m[13] * w,
            m[2] * x + m[6] * y + m[10] * z + m[14] * w,
            m[3] * x + m[7] * y + m[11] * z + m[15] * w);
    }

    private static double[] LookAt(double ex, double ey, double ez, double cx, double cy, double cz,
                                   double upX, double upY, double upZ)
    {
        double fx = cx - ex, fy = cy - ey, fz = cz - ez;
        double fl = Math.Sqrt(fx * fx + fy * fy + fz * fz);
        fx /= fl; fy /= fl; fz /= fl;

        double sx = fy * upZ - fz * upY;
        double sy = fz * upX - fx * upZ;
        double sz = fx * upY - fy * upX;
        double sl = Math.Sqrt(sx * sx + sy * sy + sz * sz);
        sx /= sl; sy /= sl; sz /= sl;

        double ux = sy * fz - sz * fy;
        double uy = sz * fx - sx * fz;
        double uz = sx * fy - sy * fx;

        return new[]
        {
            sx, ux, -fx, 0,
            sy, uy, -fy, 0,
            sz, uz, -fz, 0,
            -(sx * ex + sy * ey + sz * ez),
            -(ux * ex + uy * ey + uz * ez),
            (fx * ex + fy * ey + fz * ez),
            1
        };
    }

    private static double[] Perspective(double fovY, double aspect, double zNear, double zFar)
    {
        double f = 1.0 / Math.Tan(fovY / 2.0);
        return new[]
        {
            f / aspect, 0, 0, 0,
            0, f, 0, 0,
            0, 0, (zFar + zNear) / (zNear - zFar), -1,
            0, 0, (2.0 * zFar * zNear) / (zNear - zFar), 0
        };
    }

    private static double[] Inverse(double[] view, double[] proj)
    {
        // World -> view -> clip is proj * view (column-major). The unproject
        // matrix must invert that product, not view * proj.
        var vp = Mul(proj, view);
        return Invert4(vp);
    }

    private static double[] Mul(double[] a, double[] b)
    {
        var r = new double[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                double s = 0;
                for (int k = 0; k < 4; k++) s += a[i + k * 4] * b[k + j * 4];
                r[i + j * 4] = s;
            }
        return r;
    }

    private static double[] Invert4(double[] m)
    {
        var inv = new double[16];
        inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
        inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
        inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
        inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
        inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
        inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
        inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
        inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
        inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
        inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
        inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
        inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
        inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
        inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
        inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
        inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

        double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (Math.Abs(det) < 1e-12) return new double[16] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        det = 1.0 / det;
        for (int i = 0; i < 16; i++) inv[i] *= det;
        return inv;
    }

    private static double ToRadians(double deg) => deg * Math.PI / 180.0;
    private static double ToDegrees(double rad) => rad * 180.0 / Math.PI;
    private static double Clamp(double v, double lo, double hi) => v < lo ? lo : (v > hi ? hi : v);
}
