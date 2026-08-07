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
