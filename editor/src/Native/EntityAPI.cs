using System.Runtime.InteropServices;
using System.Text;

namespace GryceEngine.Editor.Native;

public static class EntityAPI
{
    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GEntity_GetCount();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern GEntityHandle GEntity_GetAt(int index);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GEntity_GetName(GEntityHandle entity, StringBuilder outBuf, int bufSize);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GEntity_GetPath(GEntityHandle entity, StringBuilder outBuf, int bufSize);

    // The core writes UTF-8 bytes into these buffers; a StringBuilder P/Invoke
    // would decode them with the ANSI codepage and garble Chinese text. Read
    // the raw bytes and decode explicitly instead.
    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl,
        EntryPoint = "GEntity_GetName")]
    private static extern int GEntity_GetNameBytes(GEntityHandle entity, [Out] byte[] outBuf, int bufSize);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl,
        EntryPoint = "GEntity_GetPath")]
    private static extern int GEntity_GetPathBytes(GEntityHandle entity, [Out] byte[] outBuf, int bufSize);

    public static string? GetNameUtf8(GEntityHandle entity)
        => ReadUtf8(entity, 512, GEntity_GetNameBytes);

    public static string? GetPathUtf8(GEntityHandle entity)
        => ReadUtf8(entity, 1024, GEntity_GetPathBytes);

    private static string? ReadUtf8(GEntityHandle entity, int capacity,
        Func<GEntityHandle, byte[], int, int> call)
    {
        var buf = new byte[capacity];
        int len = call(entity, buf, buf.Length);
        if (len <= 0) return null;
        return Encoding.UTF8.GetString(buf, 0, len).TrimEnd('\0');
    }

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern GEntityHandle GEntity_GetParent(GEntityHandle entity);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GEntity_GetChildCount(GEntityHandle entity);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern GEntityHandle GEntity_GetChildAt(GEntityHandle entity, int index);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GEntity_GetSiblingIndex(GEntityHandle entity);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern GEntityHandle GEntity_GetSelected();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GEntity_GetLocalPosition(GEntityHandle entity, out GVec3 outPos);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GEntity_GetLocalRotation(GEntityHandle entity, out GQuat outRot);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GEntity_GetLocalScale(GEntityHandle entity, out GVec3 outScale);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GEntity_SetLocalPosition(GEntityHandle entity, ref GVec3 pos);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GEntity_SetLocalRotation(GEntityHandle entity, ref GQuat rot);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GEntity_SetLocalScale(GEntityHandle entity, ref GVec3 scale);
}
