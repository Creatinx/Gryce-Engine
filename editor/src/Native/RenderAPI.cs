using System.Runtime.InteropServices;

namespace GryceEngine.Editor.Native;

public static class RenderAPI
{
    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GRender_Init(ref GRenderInitDesc desc);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_Shutdown();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GRender_IsInitialized();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_BeginFrame();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_RenderWorld();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_RenderGizmo();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_RenderGameView();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_EndFrame();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern GTextureHandle GRender_GetViewportTexture();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern GTextureHandle GRender_GetGameViewTexture();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GRender_GetViewportSize(out int outW, out int outH);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GRender_GetGameViewSize(out int outW, out int outH);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_SetVSync([MarshalAs(UnmanagedType.U1)] bool enabled);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_SetDisplayMode([MarshalAs(UnmanagedType.LPUTF8Str)] string mode);
}
