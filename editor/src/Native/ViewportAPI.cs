using System.Runtime.InteropServices;

namespace GryceEngine.Editor.Native;

public static class ViewportAPI
{
    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GViewport_SetSize(int w, int h);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GViewport_GetSize(out int outW, out int outH);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GViewport_SetCamera(GEntityHandle cameraEntity);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern GEntityHandle GViewport_GetCamera();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GGameView_SetSize(int w, int h);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GGameView_GetSize(out int outW, out int outH);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GGameView_SetCamera(GEntityHandle cameraEntity);
}
