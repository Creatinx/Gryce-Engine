using System.Runtime.InteropServices;
using System.Text;

namespace GryceEngine.Editor.Native;

public static class AssetAPI
{
    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern GAssetHandle GAsset_Import([MarshalAs(UnmanagedType.LPStr)] string sourcePath);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern GAssetHandle GAsset_Load([MarshalAs(UnmanagedType.LPStr)] string path);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GAsset_GetPath(GAssetHandle handle, StringBuilder outBuf, int bufSize);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GAsset_Unload(GAssetHandle handle);
}
