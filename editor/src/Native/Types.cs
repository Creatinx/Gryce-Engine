using System.Runtime.InteropServices;

namespace GryceEngine.Editor.Native;

#region Handles
public enum GEntityHandle : int { Null = 0 }
public enum GComponentHandle : int { Null = 0 }
public enum GAssetHandle : int { Null = 0 }
public readonly record struct GTextureHandle(nint Value);
public readonly record struct GWindowHandle(nint Value);
public enum GBodyHandle : int { Null = 0 }
#endregion

#region Math Types
[StructLayout(LayoutKind.Sequential)]
public struct GVec3
{
    public float X, Y, Z;
    public GVec3(float x, float y, float z) { X = x; Y = y; Z = z; }
}

[StructLayout(LayoutKind.Sequential)]
public struct GVec4
{
    public float X, Y, Z, W;
    public GVec4(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }
}

[StructLayout(LayoutKind.Sequential)]
public struct GQuat
{
    public float X, Y, Z, W;
    public GQuat(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }
}

[StructLayout(LayoutKind.Sequential)]
public struct GMat4
{
    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 16)]
    public float[] M;
}

[StructLayout(LayoutKind.Sequential)]
public struct GColor
{
    public float R, G, B, A;
    public GColor(float r, float g, float b, float a) { R = r; G = g; B = b; A = a; }
}
#endregion

#region Enums
public enum GRenderAPI
{
    OpenGL = 0,
    Vulkan = 1,
    DX11 = 2,
    DX12 = 3
}

public enum GWindowMode
{
    Windowed = 0,
    Fullscreen,
    Borderless
}

public enum GInputAction
{
    Press = 0,
    Release,
    Repeat
}

public enum GPhysicsBackend
{
    Jolt = 0,
    Box2D
}

public enum GCommandType
{
    Nop = 0,
    LoadScene,
    SaveScene,
    CreateEntity,
    DestroyEntity,
    RenameEntity,
    ReparentEntity,
    SelectEntity,
    SetTransform,
    SetProperty,
    AddComponent,
    RemoveComponent,
    PlayMode,
    StopMode,
    PauseMode,
    StepFrame,
    ImportAsset,

    SetRenderTarget = 100,
    SetViewportSize,
    SetGameViewSize,
    SetMaterial,

    InputKey = 200,
    InputMouseMove,
    InputMouseButton,
    InputMouseScroll,
    InputMouseReset,
    InputMouseDelta,

    PhysicsSetGravity = 300,
    PhysicsAddForce,

    GizmoSetOperation = 400,
    GizmoSetSpace,
    GizmoManipulate,

    SetScript = 500,
    ReloadScripts
}
#endregion

#region Callbacks
[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void GOnEntitySelected(GEntityHandle entity, nint userData);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void GOnEntityDeselected(nint userData);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void GOnSceneLoaded([MarshalAs(UnmanagedType.LPUTF8Str)] string path, nint userData);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void GOnPlayModeChanged(bool isPlaying, bool isPaused, nint userData);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void GOnEntityListChanged(nint userData);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void GOnComponentChanged(GEntityHandle entity, ulong compTypeHash, nint userData);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void GOnLogMessage(int level,
    [MarshalAs(UnmanagedType.LPUTF8Str)] string msg,
    [MarshalAs(UnmanagedType.LPUTF8Str)] string sourceFile,
    int sourceLine, nint userData);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void GOnMouseLock(int locked, nint userData);

[UnmanagedFunctionPointer(CallingConvention.Cdecl)]
public delegate void GOnViewportTextureReady(GTextureHandle handle, int w, int h, nint userData);
#endregion

#region Command
[StructLayout(LayoutKind.Sequential)]
public unsafe struct GCommand
{
    public GCommandType Type;
    public ulong Seq;
    public fixed byte Payload[256];

    public static GCommand Create(GCommandType type, ReadOnlySpan<byte> payload)
    {
        GCommand cmd = default;
        cmd.Type = type;
        if (payload.Length > 256) payload = payload.Slice(0, 256);
        payload.CopyTo(new Span<byte>(cmd.Payload, 256));
        return cmd;
    }
}
#endregion

#region Init Descs
[StructLayout(LayoutKind.Sequential)]
public struct GCoreInitDesc
{
    public uint Version;
    [MarshalAs(UnmanagedType.LPStr)]
    public string ProjectRoot;
    [MarshalAs(UnmanagedType.U1)]
    public bool EnableReflection;
}

[StructLayout(LayoutKind.Sequential)]
public struct GRenderInitDesc
{
    public uint Version;
    public GWindowHandle NativeWindow;
    public GRenderAPI Api;
    public int ViewportW;
    public int ViewportH;
    [MarshalAs(UnmanagedType.U1)]
    public bool SyncMode;
}
#endregion
