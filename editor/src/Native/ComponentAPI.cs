using System.Runtime.InteropServices;
using System.Text;

namespace GryceEngine.Editor.Native;

public static class ComponentAPI
{
    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_GetCount(GEntityHandle entity);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_GetTypeHashAt(GEntityHandle entity, int index, out ulong outHash);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_GetTypeNameAt(GEntityHandle entity, int index, StringBuilder outBuf, int bufSize);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_GetPropertyCount(GEntityHandle entity, ulong compTypeHash);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_GetPropertyInfo(GEntityHandle entity, ulong compTypeHash, int propIndex,
        StringBuilder outName, int nameBufSize,
        out int outType, out int outSize);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_GetProperty(GEntityHandle entity, ulong compTypeHash,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string propName,
        nint outValue, int valueSize);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_SetProperty(GEntityHandle entity, ulong compTypeHash,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string propName,
        nint value, int valueSize);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_TilemapGetTiles(GEntityHandle entity, ulong compTypeHash,
        [Out] int[] outTiles, int maxCount);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_TilemapSetTiles(GEntityHandle entity, ulong compTypeHash,
        int[] tiles, int count);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_AddComponent(GEntityHandle entity, ulong compTypeHash);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_RemoveComponent(GEntityHandle entity, ulong compTypeHash);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_GetRegisteredTypeCount();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GComponent_GetRegisteredTypeInfo(int index, out ulong outHash, StringBuilder outName, int nameBufSize);
}
