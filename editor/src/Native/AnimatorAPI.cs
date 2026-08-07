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

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern float GAnimator_GetClipDuration(GEntityHandle entity, ulong compTypeHash,
        int index);
}
