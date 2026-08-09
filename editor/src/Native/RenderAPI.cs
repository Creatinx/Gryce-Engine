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

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_SetScene2D([MarshalAs(UnmanagedType.U1)] bool enabled);

    // Project Settings（渲染质量）
    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_SetHDR([MarshalAs(UnmanagedType.U1)] bool enabled);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GRender_IsHDR();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_SetToneMapMode(int mode);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GRender_GetToneMapMode();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_SetExposure(float exposure);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern float GRender_GetExposure();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_SetShadowEnabled([MarshalAs(UnmanagedType.U1)] bool enabled);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GRender_IsShadowEnabled();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_SetShadowMapSize(int size);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GRender_GetShadowMapSize();

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_SetAmbient(float r, float g, float b);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_GetAmbient(out float r, out float g, out float b);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GRender_SetIBLIntensity(float intensity);

    [DllImport(NativeLibrary.Renderer, CallingConvention = CallingConvention.Cdecl)]
    public static extern float GRender_GetIBLIntensity();
}
