using System.Runtime.InteropServices;

namespace GryceEngine.Editor.Native;

public static class WindowAPI
{
    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GWindow_InitExternal(GWindowHandle hwnd, int w, int h);

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GWindow_Create([MarshalAs(UnmanagedType.LPStr)] string title, int w, int h, GWindowMode mode);

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GWindow_Destroy();

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GWindow_IsValid();

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GWindow_GetSize(out int outW, out int outH);

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GWindow_SetSize(int w, int h);

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern GWindowHandle GWindow_GetNativeHandle();

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GWindow_ShouldClose();

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GWindow_PollEvents();

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GWindow_SwapBuffers();
}
