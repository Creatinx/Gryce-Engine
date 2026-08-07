using System.Runtime.InteropServices;
using System.Text;

namespace GryceEngine.Editor.Native;

public static class SceneAPI
{
    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GScene_Load([MarshalAs(UnmanagedType.LPStr)] string path);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GScene_Save([MarshalAs(UnmanagedType.LPStr)] string path);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GScene_GetCurrentPath(StringBuilder outBuf, int bufSize);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GScene_New();
}
