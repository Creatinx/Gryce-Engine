using System.Runtime.InteropServices;

namespace GryceEngine.Editor.Native;

public static class InputAPI
{
    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GInput_InjectKey(int keyCode, GInputAction action);

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GInput_InjectMouseMove(float x, float y);

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GInput_InjectMouseButton(int button, GInputAction action, float x, float y);

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GInput_InjectMouseScroll(float deltaX, float deltaY);

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GInput_IsKeyPressed(int keyCode);

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GInput_IsKeyHeld(int keyCode);

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GInput_IsMouseButtonPressed(int button);

    [DllImport(NativeLibrary.Platform, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GInput_GetMousePosition(out float outX, out float outY);
}
