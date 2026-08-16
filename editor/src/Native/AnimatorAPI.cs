using System.Runtime.InteropServices;
using System.Text;

namespace GryceEngine.Editor.Native;

public static class AnimatorAPI
{
    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GAnimator_GetClipCount(GEntityHandle entity, ulong compTypeHash);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GAnimator_GetClipName(GEntityHandle entity, ulong compTypeHash,
        int index, StringBuilder outBuf, int bufSize);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl,
        EntryPoint = "GAnimator_GetClipName")]
    private static extern int GAnimator_GetClipNameBytes(GEntityHandle entity, ulong compTypeHash,
        int index, [Out] byte[] outBuf, int bufSize);

    public static string? GetClipNameUtf8(GEntityHandle entity, ulong compTypeHash, int index)
    {
        var buf = new byte[256];
        int len = GAnimator_GetClipNameBytes(entity, compTypeHash, index, buf, buf.Length);
        if (len < 0) return null;
        return Encoding.UTF8.GetString(buf, 0, len).TrimEnd('\0');
    }

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern float GAnimator_GetClipDuration(GEntityHandle entity, ulong compTypeHash,
        int index);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GAnimator_GetBoneCount(GEntityHandle entity, ulong compTypeHash);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl,
        EntryPoint = "GAnimator_GetBoneName")]
    private static extern int GAnimator_GetBoneNameBytes(GEntityHandle entity, ulong compTypeHash,
        int boneIndex, [Out] byte[] outBuf, int bufSize);

    public static string? GetBoneNameUtf8(GEntityHandle entity, ulong compTypeHash, int boneIndex)
    {
        var buf = new byte[256];
        int len = GAnimator_GetBoneNameBytes(entity, compTypeHash, boneIndex, buf, buf.Length);
        if (len < 0) return null;
        return Encoding.UTF8.GetString(buf, 0, len).TrimEnd('\0');
    }

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GAnimator_GetBoneParentIndex(GEntityHandle entity, ulong compTypeHash,
        int boneIndex);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GAnimator_GetClipTrackCount(GEntityHandle entity, ulong compTypeHash,
        int clipIndex);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GAnimator_GetClipTrackBone(GEntityHandle entity, ulong compTypeHash,
        int clipIndex, int trackIndex);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GAnimator_GetClipKeyframeCount(GEntityHandle entity, ulong compTypeHash,
        int clipIndex, int trackIndex, int channel);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GAnimator_GetClipKeyframe(GEntityHandle entity, ulong compTypeHash,
        int clipIndex, int trackIndex, int channel, int keyIndex,
        [Out] float[] outFloats, int outCapacity);
}
