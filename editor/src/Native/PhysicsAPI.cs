using System.Runtime.InteropServices;

namespace GryceEngine.Editor.Native;

public static class PhysicsAPI
{
    [DllImport(NativeLibrary.Physics, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GPhysics_Init(GPhysicsBackend backend);

    [DllImport(NativeLibrary.Physics, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GPhysics_Shutdown();

    [DllImport(NativeLibrary.Physics, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GPhysics_SetGravity(ref GVec3 gravity);

    [DllImport(NativeLibrary.Physics, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GPhysics_Step(float dt, int substeps);

    [DllImport(NativeLibrary.Physics, CallingConvention = CallingConvention.Cdecl)]
    public static extern GBodyHandle GPhysics_CreateBody(GEntityHandle entity, [MarshalAs(UnmanagedType.U1)] bool isStatic);

    [DllImport(NativeLibrary.Physics, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GPhysics_DestroyBody(GBodyHandle body);

    [DllImport(NativeLibrary.Physics, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GPhysics_SetBodyTransform(GBodyHandle body, ref GVec3 pos, ref GQuat rot);

    [DllImport(NativeLibrary.Physics, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GPhysics_GetBodyTransform(GBodyHandle body, out GVec3 outPos, out GQuat outRot);

    [DllImport(NativeLibrary.Physics, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GPhysics_AddForce(GBodyHandle body, ref GVec3 force);

    [DllImport(NativeLibrary.Physics, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GPhysics_AddImpulse(GBodyHandle body, ref GVec3 impulse);

    [DllImport(NativeLibrary.Physics, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GPhysics_Raycast(ref GVec3 origin, ref GVec3 dir, float maxDist,
        out GVec3 outHitPoint, out GVec3 outHitNormal, out GEntityHandle outEntity);
}