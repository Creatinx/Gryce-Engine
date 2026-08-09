using System;
using System.Runtime.InteropServices;
using System.Text;

namespace GryceEngine.Editor.Native;

/// <summary>GryceSRT C API entry points (Core embeds Lua).</summary>
public static class ScriptAPI
{
    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr GScript_GetVersion();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    private static extern int GScript_RunString(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string code,
        [Out] byte[] errBuf, int errCap);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    private static extern int GScript_RunFile(
        [MarshalAs(UnmanagedType.LPUTF8Str)] string path,
        [Out] byte[] errBuf, int errCap);

    public static string? GetVersion()
    {
        var ptr = GScript_GetVersion();
        if (ptr == IntPtr.Zero) return null;
        var bytes = new System.Collections.Generic.List<byte>();
        int i = 0;
        while (true)
        {
            byte b = Marshal.ReadByte(ptr, i++);
            if (b == 0) break;
            bytes.Add(b);
        }
        return Encoding.UTF8.GetString(bytes.ToArray());
    }

    public static (int Result, string Error) RunString(string code)
    {
        var err = new byte[512];
        int rc = GScript_RunString(code ?? string.Empty, err, err.Length);
        return (rc, DecodeError(err));
    }

    public static (int Result, string Error) RunFile(string path)
    {
        var err = new byte[512];
        int rc = GScript_RunFile(path ?? string.Empty, err, err.Length);
        return (rc, DecodeError(err));
    }

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    private static extern int GScript_GetPropCount(GEntityHandle entity, out int count);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    private static extern int GScript_GetPropInfo(GEntityHandle entity, int index,
        [Out] byte[] nameBuf, int nameCap, out int type);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    private static extern int GScript_GetPropFloat(GEntityHandle entity,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name, out float value);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    private static extern int GScript_SetPropFloat(GEntityHandle entity,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name, float value);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    private static extern int GScript_GetPropString(GEntityHandle entity,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name, [Out] byte[] valueBuf, int valueCap);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    private static extern int GScript_SetPropString(GEntityHandle entity,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string value);

    public static int GetPropCount(GEntityHandle entity)
    {
        if (GScript_GetPropCount(entity, out int count) != 0) return 0;
        return count;
    }

    public static (string Name, int Type)? GetPropInfo(GEntityHandle entity, int index)
    {
        var name = new byte[128];
        if (GScript_GetPropInfo(entity, index, name, name.Length, out int type) != 0) return null;
        return (DecodeError(name), type);
    }

    public static float? GetPropFloat(GEntityHandle entity, string name)
    {
        if (GScript_GetPropFloat(entity, name, out float value) != 0) return null;
        return value;
    }

    public static bool SetPropFloat(GEntityHandle entity, string name, float value)
        => GScript_SetPropFloat(entity, name, value) == 0;

    public static string? GetPropString(GEntityHandle entity, string name)
    {
        var buf = new byte[512];
        if (GScript_GetPropString(entity, name, buf, buf.Length) != 0) return null;
        return DecodeError(buf);
    }

    public static bool SetPropString(GEntityHandle entity, string name, string value)
        => GScript_SetPropString(entity, name, value) == 0;

    private static string DecodeError(byte[] buf)
    {
        int len = 0;
        while (len < buf.Length && buf[len] != 0) len++;
        return Encoding.UTF8.GetString(buf, 0, len);
    }
}
