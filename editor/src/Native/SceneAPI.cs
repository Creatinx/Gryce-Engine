using System.Runtime.InteropServices;
using System.Text;

namespace GryceEngine.Editor.Native;

public static class SceneAPI
{
    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GScene_Load([MarshalAs(UnmanagedType.LPUTF8Str)] string path);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GScene_Save([MarshalAs(UnmanagedType.LPUTF8Str)] string path);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GScene_GetCurrentPath(StringBuilder outBuf, int bufSize);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GScene_New();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern GEntityHandle GScene_PickScreen(
        float sx, float sy, int viewportW, int viewportH, GEntityHandle cameraEntity);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern GEntityHandle GScene_PickRay(
        ref GVec3 origin, ref GVec3 direction, float maxDist);
}
