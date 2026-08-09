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

    private static string DecodeError(byte[] buf)
    {
        int len = 0;
        while (len < buf.Length && buf[len] != 0) len++;
        return Encoding.UTF8.GetString(buf, 0, len);
    }
}
