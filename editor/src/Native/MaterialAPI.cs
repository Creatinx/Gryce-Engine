using System.Runtime.InteropServices;
using System.Text;

namespace GryceEngine.Editor.Native;

/// <summary>PBR material field ids, matching GMaterialField in material_api.h.</summary>
public enum GMaterialField
{
    AlbedoColor = 0,
    Roughness,
    Metallic,
    AO,
    EmissiveColor,
    Opacity,
    BlendMode,
    TwoSided,
    UvScale,
    UvOffset,
    AlbedoMapPath,
    NormalMapPath,
    RoughnessMapPath,
    MetallicMapPath,
    AoMapPath,
    EmissiveMapPath,
    UseAlbedoMap,
    UseNormalMap,
    UseRoughnessMap,
    UseMetallicMap,
    UseAoMap,
    UseEmissiveMap
}

public static class MaterialAPI
{
    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GMaterial_GetField(GEntityHandle entity, ulong compTypeHash, int field,
        [Out] float[] outFloats, int floatCapacity,
        StringBuilder outStr, int strCapacity);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GMaterial_SetField(GEntityHandle entity, ulong compTypeHash, int field,
        [In] float[] inFloats, int floatCount,
        [MarshalAs(UnmanagedType.LPUTF8Str)] string? inStr);

    // UTF-8-safe read for string material fields (paths may contain Chinese).
    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl,
        EntryPoint = "GMaterial_GetField")]
    private static extern int GMaterial_GetFieldBytes(GEntityHandle entity, ulong compTypeHash, int field,
        [Out] float[]? outFloats, int floatCapacity,
        [Out] byte[]? outStr, int strCapacity);

    public static string? GetFieldStringUtf8(GEntityHandle entity, ulong compTypeHash, int field)
    {
        var buf = new byte[512];
        int len = GMaterial_GetFieldBytes(entity, compTypeHash, field, null, 0, buf, buf.Length);
        if (len < 0) return null;
        return Encoding.UTF8.GetString(buf, 0, len).TrimEnd('\0');
    }
}
