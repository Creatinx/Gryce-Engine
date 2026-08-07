using System.Runtime.InteropServices;
using System.Text;

namespace GryceEngine.Editor.Native;

public static class CoreAPI
{
    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GCore_Init(ref GCoreInitDesc desc);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_Shutdown();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GCore_IsInitialized();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_BeginFrame(float dt);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_EndFrame();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GCore_PushCommand(ref GCommand cmd);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GCore_PushCommands([MarshalAs(UnmanagedType.LPArray)] GCommand[] cmds, int count);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GCore_GetCmdQueueCapacity();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GCore_GetDroppedCmdCount();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GCore_IsPlaying();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    [return: MarshalAs(UnmanagedType.U1)]
    public static extern bool GCore_IsPaused();

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_SetCallback_UserData(nint userData);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_RegisterCallback_OnEntitySelected(GOnEntitySelected cb);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_RegisterCallback_OnEntityDeselected(GOnEntityDeselected cb);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_RegisterCallback_OnSceneLoaded(GOnSceneLoaded cb);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_RegisterCallback_OnPlayModeChanged(GOnPlayModeChanged cb);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_RegisterCallback_OnEntityListChanged(GOnEntityListChanged cb);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_RegisterCallback_OnComponentChanged(GOnComponentChanged cb);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern void GCore_RegisterCallback_OnLogMessage(GOnLogMessage cb);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern int GCore_GetLogMessages([MarshalAs(UnmanagedType.LPStr)] StringBuilder outBuf, int bufSize);

    [DllImport(NativeLibrary.Core, CallingConvention = CallingConvention.Cdecl)]
    public static extern nint GCore_GetInternalWorldPtr();
}
