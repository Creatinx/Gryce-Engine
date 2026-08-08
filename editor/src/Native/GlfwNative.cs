using System;
using System.Runtime.InteropServices;

namespace GryceEngine.Editor.Native;

/// <summary>
/// Minimal GLFW P/Invoke needed to move the OpenGL context to the dedicated
/// render thread so the editor UI thread is never blocked by rendering.
///
/// IMPORTANT: must bind to the SAME GLFW instance the core uses (glfw3d.dll).
/// Binding to "glfw3.dll" resolves to a second, uninitialized GLFW copy
/// (e.g. from msys64) and glfwMakeContextCurrent fails with
/// GLFW_NOT_INITIALIZED, leaving the viewport black.
/// </summary>
public static class GlfwNative
{
    [DllImport("glfw3d.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void glfwMakeContextCurrent(IntPtr window);

    [DllImport("glfw3d.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern void glfwSwapInterval(int interval);
}
