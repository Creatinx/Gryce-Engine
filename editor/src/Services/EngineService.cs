using GryceEngine.Editor.Native;
using GryceEngine.Editor.Views;
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
    /// <summary>Last constructed service instance, so models/views can flag
    /// scene mutations that bypass the command queue (Inspector, gizmo).</summary>
    public static EngineService? Current { get; private set; }

    private readonly DispatcherTimer _frameTimer;
    private readonly DispatcherTimer _autoSaveTimer;
    private bool _isPlaying;
    private bool _isPaused;
    private bool _initialized;
    private bool _sceneDirty;

    public bool IsPlaying { get => _isPlaying; private set { _isPlaying = value; OnPropertyChanged(); } }
    public bool IsPaused { get => _isPaused; private set { _isPaused = value; OnPropertyChanged(); } }
    public bool IsInitialized { get => _initialized; private set { _initialized = value; OnPropertyChanged(); } }
    public bool IsSceneDirty => _sceneDirty;
    public string ProjectRoot { get; private set; } = "";
    public EditorSettingsService.Settings EditorSettings { get; private set; } = new();

    /// <summary>Raised for editor-level engine messages (e.g. auto-save).</summary>
    public event Action<string>? LogMessage;

    /// <summary>Raised after the active project root changes (core re-initialized).</summary>
    public event Action<string>? ProjectChanged;

    public EngineService()
    {
        Current = this;
        _frameTimer = new DispatcherTimer(DispatcherPriority.Render)
        {
            Interval = TimeSpan.FromSeconds(1.0 / 60.0)
        };
        _frameTimer.Tick += OnFrameTick;

        _autoSaveTimer = new DispatcherTimer(DispatcherPriority.Background);
        _autoSaveTimer.Tick += OnAutoSaveTick;
    }

    public void Initialize(string projectRoot)
    {
        if (IsInitialized) return;

        string resolvedRoot = ResolveProjectRoot(projectRoot);
        ProjectRoot = resolvedRoot;

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
    /// Switches to another game project: shuts down the core and re-initializes
    /// it with the new project root (res:/ namespace base). Editor callbacks
    /// registered on the core survive the re-init (callback table persists).
    /// </summary>
    public void ReloadProject(string root)
    {
        if (string.IsNullOrWhiteSpace(root) || !Directory.Exists(root))
        {
            LogMessage?.Invoke($"Project root not found: {root}");
            return;
        }
        Shutdown();
        Initialize(root);
        ProjectChanged?.Invoke(ProjectRoot);
        EditorSettingsService.SaveLastProject(ProjectRoot);
        EditorSettingsService.AddRecentProject(ProjectRoot);
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
        _autoSaveTimer.Stop();
        if (IsInitialized)
        {
            CoreAPI.GCore_Shutdown();
            IsInitialized = false;
        }
    }

    /// <summary>Marks the current scene as having unsaved changes.</summary>
    public void MarkSceneDirty() => _sceneDirty = true;

    /// <summary>Clears the dirty flag (after save / load / new scene).</summary>
    public void ClearDirty() => _sceneDirty = false;

    /// <summary>
    /// Restarts the auto-save timer with the given interval in minutes.
    /// 0 or negative disables auto-save entirely.
    /// </summary>
    public void UpdateAutoSaveInterval(int minutes)
    {
        _autoSaveTimer.Stop();
        if (minutes <= 0) return;
        _autoSaveTimer.Interval = TimeSpan.FromMinutes(minutes);
        _autoSaveTimer.Start();
    }

    /// <summary>重新读取编辑器设置（主题/语言/自动保存/视口开关等）。</summary>
    public void ReloadEditorSettings()
    {
        EditorSettings = EditorSettingsService.Load();
        UpdateAutoSaveInterval(EditorSettings.AutoSaveInterval);
        EditorSettingsChanged?.Invoke();
    }

    /// <summary>编辑器设置变化后触发（视口据此应用网格/Gizmo/统计显示）。</summary>
    public event Action? EditorSettingsChanged;

    private void OnAutoSaveTick(object? sender, EventArgs e)
    {
        if (!IsInitialized || IsPlaying || !_sceneDirty) return;

        var sb = new StringBuilder(512);
        string path = SceneAPI.GScene_GetCurrentPath(sb, sb.Capacity) > 0
            ? sb.ToString()
            : (SceneAPI.GScene_GetMode() == 0
                ? "res:/scenes/scene_2d.gesc"
                : "res:/scenes/scene_3d.gesc");

        int result = SceneAPI.GScene_Save(path);
        if (result == 0)
        {
            ClearDirty();
            LogMessage?.Invoke($"Auto-saved scene: {path}");
        }
        else
        {
            LogMessage?.Invoke($"Auto-save failed: {path}");
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
        return EntityAPI.GetNameUtf8(handle);
    }

    public string? GetEntityPath(GEntityHandle handle)
    {
        return EntityAPI.GetPathUtf8(handle);
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
        // 游戏视图激活时，把编辑器注入的输入同步到核心，供 Lua 脚本读取
        //（独立运行时的 game_main.cpp 也会每帧调用 GInput_SyncToCore）。
        if (ViewportView.GameViewActive) Native.InputAPI.GInput_SyncToCore();
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
