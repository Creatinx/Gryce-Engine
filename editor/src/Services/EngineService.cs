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
    private bool _autoSaveRetry;
    private string? _deferredSavePath;
    private int _deferredSaveTries;

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
        WriteEngineLog("ReloadProject: shutdown " + root);
        Shutdown();
        WriteEngineLog("ReloadProject: initialize " + root);
        Initialize(root);
        WriteEngineLog("ReloadProject: project changed");
        ProjectChanged?.Invoke(ProjectRoot);
        WriteEngineLog("ReloadProject: persist");
        EditorSettingsService.SaveLastProject(ProjectRoot);
        EditorSettingsService.AddRecentProject(ProjectRoot);
        WriteEngineLog("ReloadProject: done");
    }

    private static void WriteEngineLog(string message)
    {
        try
        {
            string path = System.IO.Path.Combine(
                AppDomain.CurrentDomain.BaseDirectory, "editor_crash.log");
            System.IO.File.AppendAllText(path,
                $"[{DateTime.Now:HH:mm:ss.fff}] {message}\r\n");
        }
        catch
        {
        }
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
        // 退出前若还有被 2D 相机清理推迟的保存，直接落盘（宁可包含临时相机，
        // 也不能丢用户的保存请求）。
        if (_deferredSavePath != null)
        {
            string path = _deferredSavePath;
            _deferredSavePath = null;
            SaveSceneNow(path);
        }
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

    public enum SaveResult
    {
        Completed,
        Pending, // 需要先清理编辑器 2D 相机，稍后由帧循环落盘
        Failed
    }

    /// <summary>保存场景的统一入口：需要清理编辑器 2D 相机时先推销毁命令，
    /// 等核心处理完再写盘，避免临时相机被序列化进场景文件。</summary>
    public SaveResult SaveScene(string path)
    {
        if (ViewportView.TryDestroyEditor2DCamera())
        {
            _deferredSavePath = path;
            _deferredSaveTries = 0;
            LogMessage?.Invoke("Scene save deferred: removing editor 2D camera first.");
            return SaveResult.Pending;
        }
        return SaveSceneNow(path);
    }

    private SaveResult SaveSceneNow(string path)
    {
        var sb = new StringBuilder(512);
        string target = string.IsNullOrWhiteSpace(path)
            ? (SceneAPI.GScene_GetCurrentPath(sb, sb.Capacity) > 0
                ? sb.ToString()
                : (SceneAPI.GScene_GetMode() == 0
                    ? "res:/scenes/scene_2d.gesc"
                    : "res:/scenes/scene_3d.gesc"))
            : path;
        int result = SceneAPI.GScene_Save(target);
        if (result == 0)
        {
            ClearDirty();
            return SaveResult.Completed;
        }
        return SaveResult.Failed;
    }

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

        // 拖拽/输入进行中：改为 5 秒后重试，而不是等下一个完整周期。
        if (EditorInteractionState.IsBusy)
        {
            _autoSaveRetry = true;
            _autoSaveTimer.Interval = TimeSpan.FromSeconds(5);
            return;
        }

        var sb = new StringBuilder(512);
        string path = SceneAPI.GScene_GetCurrentPath(sb, sb.Capacity) > 0
            ? sb.ToString()
            : (SceneAPI.GScene_GetMode() == 0
                ? "res:/scenes/scene_2d.gesc"
                : "res:/scenes/scene_3d.gesc");

        var result = SaveScene(path);
        if (result == SaveResult.Completed)
        {
            LogMessage?.Invoke($"Auto-saved scene: {path}");
        }
        else if (result == SaveResult.Failed)
        {
            LogMessage?.Invoke($"Auto-save failed: {path}");
        }
        ResetAutoSaveInterval();
    }

    private void ResetAutoSaveInterval()
    {
        if (!_autoSaveRetry) return;
        _autoSaveRetry = false;
        UpdateAutoSaveInterval(EditorSettings.AutoSaveInterval);
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
        FlushDeferredSave();
        IsPlaying = CoreAPI.GCore_IsPlaying();
        IsPaused = CoreAPI.GCore_IsPaused();
    }

    /// <summary>帧循环里落盘被 2D 相机清理推迟的保存（销毁命令已随 BeginFrame 处理）。</summary>
    private void FlushDeferredSave()
    {
        if (_deferredSavePath == null) return;
        if (!ViewportView.Editor2DCameraDestroyed && ++_deferredSaveTries <= 240)
        {
            return;
        }

        string path = _deferredSavePath;
        _deferredSavePath = null;
        ViewportView.ResumeEditor2DCamera();
        var result = SaveSceneNow(path);
        LogMessage?.Invoke(result == SaveResult.Completed
            ? $"Deferred save completed: {path}"
            : $"Deferred save failed: {path}");
    }

    public void Dispose() => Shutdown();

    public event PropertyChangedEventHandler? PropertyChanged;
    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
