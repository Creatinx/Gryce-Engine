using GryceEngine.Editor.Native;
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Threading;

namespace GryceEngine.Editor.Services;

public sealed class EngineService : INotifyPropertyChanged, IDisposable
{
    private readonly DispatcherTimer _frameTimer;
    private bool _isPlaying;
    private bool _isPaused;
    private bool _initialized;

    public bool IsPlaying { get => _isPlaying; private set { _isPlaying = value; OnPropertyChanged(); } }
    public bool IsPaused { get => _isPaused; private set { _isPaused = value; OnPropertyChanged(); } }
    public bool IsInitialized { get => _initialized; private set { _initialized = value; OnPropertyChanged(); } }

    public EngineService()
    {
        _frameTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromSeconds(1.0 / 60.0)
        };
        _frameTimer.Tick += OnFrameTick;
    }

    public void Initialize(string projectRoot)
    {
        if (IsInitialized) return;

        string resolvedRoot = ResolveProjectRoot(projectRoot);

        var desc = new GCoreInitDesc
        {
            Version = (uint)Marshal.SizeOf<GCoreInitDesc>(),
            ProjectRoot = resolvedRoot,
            EnableReflection = true
        };

        int result = CoreAPI.GCore_Init(ref desc);
        if (result != 0)
            throw new InvalidOperationException($"GCore_Init failed with code {result}");

        // 挂载物理系统（Jolt 3D + Box2D 2D），让 Play Mode 能真实模拟场景物理。
        nint worldPtr = CoreAPI.GCore_GetInternalWorldPtr();
        PhysicsAPI.GPhysics_Init(GPhysicsBackend.Jolt);
        PhysicsAPI.GPhysics_AttachSystems(worldPtr);

        IsInitialized = true;
        _frameTimer.Start();
    }

    /// <summary>
    /// Resolves the game project root passed to the native core.
    /// The engine treats this as the base of the virtual "res:/" namespace, so it
    /// must be a real filesystem path — "res:/" itself is not a valid root.
    /// Defaults to examples/3dtest when the caller did not provide a concrete path.
    /// </summary>
    private static string ResolveProjectRoot(string requested)
    {
        if (!string.IsNullOrWhiteSpace(requested) &&
            !requested.Equals("res:/", StringComparison.OrdinalIgnoreCase))
        {
            return requested.TrimEnd('\\', '/');
        }

        // Walk up from the executable directory to locate the engine repository root.
        var dir = new DirectoryInfo(AppDomain.CurrentDomain.BaseDirectory);
        while (dir != null)
        {
            if (Directory.Exists(Path.Combine(dir.FullName, "examples", "3dtest")))
            {
                return Path.Combine(dir.FullName, "examples", "3dtest");
            }
            if (File.Exists(Path.Combine(dir.FullName, "CMakeLists.txt")))
            {
                // Engine repo root found but no bundled example project — fall back to it.
                return dir.FullName;
            }
            dir = dir.Parent;
        }
        return AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\', '/');
    }

    public void Shutdown()
    {
        _frameTimer.Stop();
        if (IsInitialized)
        {
            CoreAPI.GCore_Shutdown();
            IsInitialized = false;
        }
    }

    public void Play() => PushCommand(GCommandType.PlayMode);
    public void Stop() => PushCommand(GCommandType.StopMode);
    public void Pause() => PushCommand(GCommandType.PauseMode);

    public void PushCommand(GCommandType type, ReadOnlySpan<byte> payload = default)
    {
        if (!IsInitialized) return;
        var cmd = GCommand.Create(type, payload);
        CoreAPI.GCore_PushCommand(ref cmd);
    }

    public void PushCommands(ReadOnlySpan<GCommand> cmds)
    {
        if (!IsInitialized || cmds.IsEmpty) return;
        var arr = cmds.ToArray();
        CoreAPI.GCore_PushCommands(arr, arr.Length);
    }

    public string? GetEntityName(GEntityHandle handle)
    {
        var sb = new StringBuilder(256);
        if (EntityAPI.GEntity_GetName(handle, sb, sb.Capacity) > 0)
            return sb.ToString();
        return null;
    }

    public string? GetEntityPath(GEntityHandle handle)
    {
        var sb = new StringBuilder(512);
        if (EntityAPI.GEntity_GetPath(handle, sb, sb.Capacity) > 0)
            return sb.ToString();
        return null;
    }

    public int GetEntityCount() => EntityAPI.GEntity_GetCount();

    public GEntityHandle GetEntityAt(int index) => EntityAPI.GEntity_GetAt(index);

    public GEntityHandle SelectedEntity
    {
        get => EntityAPI.GEntity_GetSelected();
        set
        {
            if (!IsInitialized) return;
            Span<byte> payload = stackalloc byte[sizeof(int)];
            BitConverterCompat.TryWriteBytes(payload, (int)value);
            PushCommand(GCommandType.SelectEntity, payload);
        }
    }

    private void OnFrameTick(object? sender, EventArgs e)
    {
        if (!IsInitialized) return;
        CoreAPI.GCore_BeginFrame((float)_frameTimer.Interval.TotalSeconds);
        CoreAPI.GCore_EndFrame();
        IsPlaying = CoreAPI.GCore_IsPlaying();
        IsPaused = CoreAPI.GCore_IsPaused();
    }

    public void Dispose() => Shutdown();

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
