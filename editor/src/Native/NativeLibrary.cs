namespace GryceEngine.Editor.Native;

/// <summary>
/// Native DLL names produced by the CMake build.
/// Debug builds use the "d" postfix (GryceCored.dll); Release uses plain names.
/// </summary>
public static class NativeLibrary
{
#if DEBUG
    public const string Core = "GryceCored.dll";
    public const string Renderer = "GryceRendererd.dll";
    public const string Platform = "GrycePlatformd.dll";
    public const string Physics = "GrycePhysicsd.dll";
#else
    public const string Core = "GryceCore.dll";
    public const string Renderer = "GryceRenderer.dll";
    public const string Platform = "GrycePlatform.dll";
    public const string Physics = "GrycePhysics.dll";
#endif
}
